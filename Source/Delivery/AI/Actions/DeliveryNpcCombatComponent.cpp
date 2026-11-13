#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "GameplayEffect.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Actions/DeliveryVehicleHoldComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AIController.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

UDeliveryNpcCombatComponent::UDeliveryNpcCombatComponent()
{
    SetIsReplicatedByDefault(true); PrimaryComponentTick.bCanEverTick=true;
}
bool UDeliveryNpcCombatComponent::CanStrikeTarget(AActor* Target,float MaxDistance) const
{
    if (!GetOwner() || !GetWorld() || bDown || !IsValid(Target) || Target==GetOwner() || Target->GetWorld()!=GetWorld()) return false;
    const auto* Player=Cast<ADeliveryCharacter>(Target);
    const auto* Receiver=Cast<IDeliveryImpactReceiver>(Target);
    const bool bValidReceiver=Receiver && !Receiver->IsKnockedDown();
    const bool bValidPlayer=Player && Player->GetActiveRagdoll() && Player->GetActiveRagdoll()->GetControlMode()==EDeliveryRagdollControlMode::Active;
    if ((!bValidReceiver && !bValidPlayer) ||
        FVector::DistSquared(Target->GetActorLocation(),GetOwner()->GetActorLocation())>FMath::Square(MaxDistance)) return false;
    FHitResult Hit; FCollisionQueryParams Params(SCENE_QUERY_STAT(ResidentStrike),false,GetOwner());
    return !GetWorld()->LineTraceSingleByChannel(Hit,GetOwner()->GetActorLocation(),Target->GetActorLocation(),ECC_Visibility,Params) || Hit.GetActor()==Target;
}
bool UDeliveryNpcCombatComponent::CanStrike(AActor* Target) const
{
    return CanStrikeTarget(Target,StrikeRange);
}
bool UDeliveryNpcCombatComponent::CanApproachStrike(AActor* Target) const
{
    if (!CanContinueStrikeApproach(Target)) return false;
    if (CanStrike(Target)) return true;
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    return Npc && Npc->CanReachTarget(Target);
}
bool UDeliveryNpcCombatComponent::CanContinueStrikeApproach(AActor* Target) const
{
    return IsReadyToStrike() &&
        CanStrikeTarget(Target,FMath::Max(StrikeRange,StrikeApproachRange));
}
bool UDeliveryNpcCombatComponent::StartStrike(AActor* Target,FGuid Id)
{
    return StartStrikeInternal(Target,Id,Id,false);
}
bool UDeliveryNpcCombatComponent::StartStrikeInternal(AActor* Target,FGuid Id,FGuid ActionId,bool bLightHit)
{
    if (!GetOwner()->HasAuthority() || !Id.IsValid() || !ActionId.IsValid() || Id==FinishedStrikeId ||
        !IsReadyToStrike() || !CanStrike(Target)) return false;
    AttackId=Id; AttackTarget=Target; StrikeAnimationId=Id; StrikeActionId=ActionId;
    StrikeStartedAt=GetWorld()->GetTimeSeconds(); bCurrentStrikeIsLight=bLightHit;
    FinishedStrikeId.Invalidate(); bFinishedStrikeHit=false;
    HitAt=StrikeStartedAt+StrikeWindup; NextStrikeAt=HitAt+1;
    GetOwner()->ForceNetUpdate();
    return true;
}
bool UDeliveryNpcCombatComponent::GetStrikeResult(FGuid Id,bool& bOutHit) const
{
    bOutHit=false;
    if (!Id.IsValid() || FinishedStrikeId!=Id) return false;
    bOutHit=bFinishedStrikeHit;
    return true;
}
double UDeliveryNpcCombatComponent::GetStrikeStartTime(FGuid Id) const
{
    return Id.IsValid() && StrikeActionId==Id?StrikeStartedAt:-1;
}
bool UDeliveryNpcCombatComponent::IsReadyToStrike() const
{
    return GetWorld() && !bDown && !IsReactingToHit() && !AttackId.IsValid() && GetWorld()->GetTimeSeconds()>=NextStrikeAt;
}
bool UDeliveryNpcCombatComponent::IsReactingToHit() const
{
    if (!GetWorld() || bDown || HitReactionStartedAt<0) return false;
    const auto* State=GetWorld()->GetGameState();
    const double Now=State?State->GetServerWorldTimeSeconds():GetWorld()->GetTimeSeconds();
    return Now>=HitReactionStartedAt && Now-HitReactionStartedAt<HitReactionDuration;
}
bool UDeliveryNpcCombatComponent::CanBeginFight(AActor* Target) const
{
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!GetWorld() || !Npc || FightActionId.IsValid() || GetWorld()->GetTimeSeconds()<NextFightAt ||
        !CanStrikeTarget(Target,FMath::Max(StrikeRange,StrikeApproachRange))) return false;
    // 短暂后仰或收拳不会作废模型决定，行为树会等身体恢复后再挥拳
    return CanStrike(Target) || Npc->CanReachTarget(Target);
}
bool UDeliveryNpcCombatComponent::BeginFight(AActor* Target,FGuid ActionId,float Duration)
{
    if (!GetOwner()->HasAuthority() || !ActionId.IsValid() || !FMath::IsFinite(Duration) || Duration<StrikeDuration ||
        Duration>5.f || !CanBeginFight(Target)) return false;
    FightActionId=ActionId; FightTarget=Target; FightOrigin=GetOwner()->GetActorLocation();
    FightEndsAt=GetWorld()->GetTimeSeconds()+Duration; FightPunches=0; FightHits=0;
    // 对方看见有人冲自己来，记下现场变化；是否还手仍由对方自己的模型决定
    if (auto* Other=Cast<ADeliveryNpcCharacter>(Target))
        Other->ObserveFact(TEXT("FightThreat"),GetOwner(),TEXT("这个人朝我冲过来，准备动手"),EDeliveryNpcEvidenceSource::Seen);
    return true;
}
FName UDeliveryNpcCombatComponent::GetFightStopReason(FGuid ActionId) const
{
    if (!GetWorld() || !ActionId.IsValid() || FightActionId!=ActionId) return TEXT("FightCancelled");
    if (bDown) return TEXT("AttackerDown");
    const auto* Target=FightTarget.Get();
    if (!IsValid(Target)) return TEXT("TargetLost");
    if (const auto* Receiver=Cast<IDeliveryImpactReceiver>(Target); Receiver && Receiver->IsKnockedDown()) return TEXT("TargetDown");
    if (const auto* Player=Cast<ADeliveryCharacter>(Target); Player && Player->GetActiveRagdoll() &&
        Player->GetActiveRagdoll()->GetControlMode()!=EDeliveryRagdollControlMode::Active) return TEXT("TargetDown");
    if (GetWorld()->GetTimeSeconds()>=FightEndsAt) return TEXT("FightTimeLimit");
    if (FVector::DistSquared(GetOwner()->GetActorLocation(),FightOrigin)>FMath::Square(FMath::Clamp(FightTravelRange,160.f,1200.f)) ||
        FVector::DistSquared(GetOwner()->GetActorLocation(),Target->GetActorLocation())>FMath::Square(FMath::Max(StrikeRange,StrikeApproachRange)))
        return TEXT("TargetEscaped");
    return NAME_None;
}
bool UDeliveryNpcCombatComponent::StartFightStrike(AActor* Target,FGuid ActionId,FGuid PunchId)
{
    if (!GetOwner()->HasAuthority() || Target!=FightTarget.Get() || !GetFightStopReason(ActionId).IsNone() ||
        FightPunches>=GetFightPunchLimit()) return false;
    if (!StartStrikeInternal(Target,PunchId,ActionId,true)) return false;
    ++FightPunches;
    return true;
}
void UDeliveryNpcCombatComponent::EndFight(FGuid ActionId)
{
    if (!ActionId.IsValid() || ActionId!=FightActionId) return;
    CancelStrike(); FightActionId.Invalidate(); FightTarget.Reset(); FightEndsAt=0;
    if (GetWorld()) NextFightAt=GetWorld()->GetTimeSeconds()+FMath::Clamp(FightCooldown,1.f,15.f);
}
FString UDeliveryNpcCombatComponent::DescribeFight(FGuid ActionId,FName Reason) const
{
    if (!ActionId.IsValid() || ActionId!=FightActionId) return FString();
    const auto* TargetNpc=Cast<ADeliveryNpcCharacter>(FightTarget.Get());
    const FString Name=TargetNpc?TargetNpc->DisplayName.ToString():TEXT("对方");
    FString End=TEXT("这次交战已经停止");
    if (Reason==TEXT("TargetDown")) End=TEXT("对方倒地，我停手了");
    else if (Reason==TEXT("TargetEscaped")) End=TEXT("对方跑远，我没有继续追");
    else if (Reason==TEXT("FightTimeLimit")) End=TEXT("追打时间已到，我停手了");
    else if (Reason==TEXT("FightPunchLimit")) End=TEXT("这轮挥拳次数已到，我停手了");
    else if (Reason==TEXT("PathUnavailable") || Reason==TEXT("PathInterrupted")) End=TEXT("前面的路走不通，我停下了");
    else if (Reason==TEXT("Hit")) End=TEXT("我被击倒，交战中断");
    return FString::Printf(TEXT("我向%s挥拳 %d 次，命中 %d 次。%s。"),*Name,FightPunches,FightHits,*End);
}
void UDeliveryNpcCombatComponent::CancelStrike()
{
    AttackId.Invalidate(); AttackTarget.Reset(); StrikeAnimationId.Invalidate(); StrikeActionId.Invalidate(); StrikeStartedAt=-1;
    bCurrentStrikeIsLight=false;
    FinishedStrikeId.Invalidate(); bFinishedStrikeHit=false;
    if (GetOwner() && GetOwner()->HasAuthority()) GetOwner()->ForceNetUpdate();
}
void UDeliveryNpcCombatComponent::ReceiveImpact(const FVector& Impulse,AActor* Source)
{
    auto* Resident=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Resident || !Resident->HasAuthority() || bDown || Impulse.ContainsNaN()) return;
    CancelStrike(); Resident->Agent->CancelPlan(TEXT("Hit"));
    if (auto* Hold=Resident->GetHeldVehicle()) Hold->Release(TEXT("HolderHit"));
    bDown=true; bDeliberate=false; bRecovering=false;
    HitReactionStartedAt=-1; ReceivedLightHits=0;
    DownStartedAt=GetWorld()->GetTimeSeconds(); RecoveryStartedAt=-1; RecoverAt=DownStartedAt+RecoverySeconds;
    // 使用 CharacterMovement 处理 NPC 击退和地面碰撞，并限制击退速度
    Resident->LaunchCharacter(Impulse.GetClampedToMaxSize(450.f)+FVector(0,0,100),true,true);
    OnRep_Down(); Resident->ForceNetUpdate();
    Resident->ObserveFact(TEXT("Hit"),Source,TEXT("我被这个对象击倒了"),EDeliveryNpcEvidenceSource::Seen);
}
void UDeliveryNpcCombatComponent::ReceiveFightHit(AActor* Source,FGuid PunchId)
{
    auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || !Npc->HasAuthority() || bDown || !IsValid(Source) || !PunchId.IsValid() || RecentLightHits.Contains(PunchId)) return;
    RecentLightHits.Add(PunchId);
    if (RecentLightHits.Num()>32) RecentLightHits.RemoveAt(0);
    const double Now=GetWorld()->GetTimeSeconds();
    if (Now-LastLightHitAt>FMath::Clamp(LightHitResetSeconds,2.f,20.f)) ReceivedLightHits=0;
    LastLightHitAt=Now; ++ReceivedLightHits;
    if (ReceivedLightHits>=FMath::Clamp(LightHitsToKnockDown,2,8))
    {
        Npc->ReceiveImpactFrom((Npc->GetActorLocation()-Source->GetActorLocation()).GetSafeNormal()*35000.f,Npc->GetActorLocation(),Source);
        return;
    }
    // 轻拳中断尚未打出的这一拳，保留整轮交战；后仰结束后行为树再决定是否能继续
    if (AttackId.IsValid())
    {
        FinishedStrikeId=AttackId; bFinishedStrikeHit=false;
        AttackId.Invalidate(); AttackTarget.Reset(); StrikeStartedAt=-1; bCurrentStrikeIsLight=false;
    }
    if (!FightActionId.IsValid() && Npc->Agent->HasActiveAction()) Npc->Agent->CancelPlan(TEXT("MeleeHit"));
    else if (FightActionId.IsValid() || Npc->Presentation->HasPendingSpeech() || !Npc->Agent->IsRespondingToAttackFrom(Source))
    {
        // 停止旧闲聊及其声音任务，已经结合本次攻击的请求可以继续，避免连续轻拳让回复永远算不完
        Npc->Agent->CancelDecision();
    }
    Npc->Presentation->CancelSpeech();
    if (auto* AI=Cast<AAIController>(Npc->GetController())) AI->StopMovement();
    Npc->GetCharacterMovement()->StopMovementImmediately();
    HitReactionStartedAt=Now;
    Npc->ForceNetUpdate();
    // 使用这次命中的编号播放人物短呼声，声音直接来自游戏资源
    Npc->Presentation->PlayReaction(EDeliveryNpcVoiceReaction::Hit,PunchId);
    Npc->ObserveFact(TEXT("LightMeleeHit"),Source,TEXT("这个人打了我一拳，我后仰了一下，仍然站着"),EDeliveryNpcEvidenceSource::Seen);
}
bool UDeliveryNpcCombatComponent::SetDeliberateProne(bool bProne)
{
    if (!GetOwner()->HasAuthority() || (bDown && !bDeliberate)) return false;
    if (bProne && !bDown) DownStartedAt=GetWorld()->GetTimeSeconds();
    if (!bProne) { DownStartedAt=-1; RecoveryStartedAt=-1; bRecovering=false; }
    bDown=bProne; bDeliberate=bProne; RecoverAt=GetWorld()->GetTimeSeconds()+4;
    OnRep_Down(); GetOwner()->ForceNetUpdate(); return true;
}
void UDeliveryNpcCombatComponent::OnRep_Down()
{
    if (auto* Resident=Cast<ADeliveryNpcCharacter>(GetOwner())) Resident->GetCharacterMovement()->MaxWalkSpeed=bDown?0.f:Resident->WalkSpeed;
}
void UDeliveryNpcCombatComponent::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn)
{
    Super::TickComponent(Dt,Type,Fn); if (!GetOwner()->HasAuthority()) return;
    const double Now=GetWorld()->GetTimeSeconds();
    if (AttackId.IsValid() && Now>=HitAt)
    {
        auto* Target=AttackTarget.Get(); FinishedStrikeId=AttackId;
        const bool bLightHit=bCurrentStrikeIsLight;
        AttackId.Invalidate(); AttackTarget.Reset(); // 先结束命中判定，再通知目标，防止同一拳重复命中
        bFinishedStrikeHit=CanStrike(Target);
        if (bFinishedStrikeHit)
        {
            if (bLightHit && FightActionId.IsValid()) ++FightHits;
            const FVector Direction=(Target->GetActorLocation()-GetOwner()->GetActorLocation()).GetSafeNormal();
            if (auto* Npc=Cast<ADeliveryNpcCharacter>(Target); bLightHit && Npc)
                Npc->Combat->ReceiveFightHit(GetOwner(),FinishedStrikeId);
            else if (auto* Receiver=Cast<IDeliveryImpactReceiver>(Target))
                Receiver->ReceiveImpactFrom(Direction*35000.f,Target->GetActorLocation(),GetOwner());
            else if (auto* Player=Cast<ADeliveryCharacter>(Target))
            {
                if (auto* ASC=Player->GetAbilitySystemComponent())
                {
                    auto Context=ASC->MakeEffectContext(); Context.AddSourceObject(GetOwner());
                    auto Spec=ASC->MakeOutgoingSpec(Player->GetDamageEffect(),1,Context);
                    if (Spec.IsValid()) { Spec.Data->SetSetByCallerMagnitude(TAG_Effect_Type_Damage,-10.f); ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data); }
                }
                Player->GetActiveRagdoll()->ApplyMeleeImpact(Direction*900.f,Target->GetActorLocation());
            }
        }
    }
    if (bDown && Now>=RecoverAt)
    {
        if (!bRecovering)
        {
            auto* Resident=CastChecked<ADeliveryNpcCharacter>(GetOwner());
            if (!Resident->GetCharacterMovement()->IsMovingOnGround()) return;
            bRecovering=true; RecoveryStartedAt=Now; RecoverAt=Now+1.6; GetOwner()->ForceNetUpdate(); return;
        }
        bDown=false; bDeliberate=false; bRecovering=false; DownStartedAt=-1; RecoveryStartedAt=-1;
        OnRep_Down(); GetOwner()->ForceNetUpdate();
        if (auto* Resident=Cast<ADeliveryNpcCharacter>(GetOwner()))
            Resident->ObserveFact(TEXT("Recovered"),nullptr,TEXT("我已经恢复站立，可以继续行动"),EDeliveryNpcEvidenceSource::ActionResult);
    }
}
void UDeliveryNpcCombatComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    EndFight(FightActionId); CancelStrike(); DownStartedAt=-1; RecoveryStartedAt=-1; HitReactionStartedAt=-1;
    RecentLightHits.Reset(); ReceivedLightHits=0;
    Super::EndPlay(Reason);
}
void UDeliveryNpcCombatComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(UDeliveryNpcCombatComponent,bDown); DOREPLIFETIME(UDeliveryNpcCombatComponent,bDeliberate); DOREPLIFETIME(UDeliveryNpcCombatComponent,bRecovering);
    DOREPLIFETIME(UDeliveryNpcCombatComponent,StrikeAnimationId); DOREPLIFETIME(UDeliveryNpcCombatComponent,StrikeStartedAt);
    DOREPLIFETIME(UDeliveryNpcCombatComponent,DownStartedAt); DOREPLIFETIME(UDeliveryNpcCombatComponent,RecoveryStartedAt);
    DOREPLIFETIME(UDeliveryNpcCombatComponent,StrikeActionId); DOREPLIFETIME(UDeliveryNpcCombatComponent,HitReactionStartedAt);
}
