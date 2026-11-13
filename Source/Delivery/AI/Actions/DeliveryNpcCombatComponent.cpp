#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "GameplayEffect.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Actions/DeliveryVehicleHoldComponent.h"
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
    return !AttackId.IsValid() && GetWorld() && GetWorld()->GetTimeSeconds()>=NextStrikeAt &&
        CanStrikeTarget(Target,FMath::Max(StrikeRange,StrikeApproachRange));
}
bool UDeliveryNpcCombatComponent::StartStrike(AActor* Target,FGuid Id)
{
    if (!GetOwner()->HasAuthority() || !Id.IsValid() || AttackId.IsValid() || GetWorld()->GetTimeSeconds()<NextStrikeAt || !CanStrike(Target)) return false;
    AttackId=Id; AttackTarget=Target; StrikeAnimationId=Id; StrikeStartedAt=GetWorld()->GetTimeSeconds();
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
    return Id.IsValid() && StrikeAnimationId==Id?StrikeStartedAt:-1;
}
void UDeliveryNpcCombatComponent::CancelStrike()
{
    AttackId.Invalidate(); AttackTarget.Reset(); StrikeAnimationId.Invalidate(); StrikeStartedAt=-1;
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
    DownStartedAt=GetWorld()->GetTimeSeconds(); RecoveryStartedAt=-1; RecoverAt=DownStartedAt+RecoverySeconds;
    // 使用 CharacterMovement 处理 NPC 击退和地面碰撞，并限制击退速度
    Resident->LaunchCharacter(Impulse.GetClampedToMaxSize(450.f)+FVector(0,0,100),true,true);
    OnRep_Down(); Resident->ForceNetUpdate();
    Resident->ObserveFact(TEXT("Hit"),Source,TEXT("我被这个对象击倒了"),EDeliveryNpcEvidenceSource::Seen);
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
        AttackId.Invalidate(); AttackTarget.Reset(); // 先结束命中判定，再通知目标，防止同一拳重复命中
        bFinishedStrikeHit=CanStrike(Target);
        if (bFinishedStrikeHit)
        {
            const FVector Direction=(Target->GetActorLocation()-GetOwner()->GetActorLocation()).GetSafeNormal();
            if (auto* Receiver=Cast<IDeliveryImpactReceiver>(Target))
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
    CancelStrike(); DownStartedAt=-1; RecoveryStartedAt=-1;
    Super::EndPlay(Reason);
}
void UDeliveryNpcCombatComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(UDeliveryNpcCombatComponent,bDown); DOREPLIFETIME(UDeliveryNpcCombatComponent,bDeliberate); DOREPLIFETIME(UDeliveryNpcCombatComponent,bRecovering);
    DOREPLIFETIME(UDeliveryNpcCombatComponent,StrikeAnimationId); DOREPLIFETIME(UDeliveryNpcCombatComponent,StrikeStartedAt);
    DOREPLIFETIME(UDeliveryNpcCombatComponent,DownStartedAt); DOREPLIFETIME(UDeliveryNpcCombatComponent,RecoveryStartedAt);
}
