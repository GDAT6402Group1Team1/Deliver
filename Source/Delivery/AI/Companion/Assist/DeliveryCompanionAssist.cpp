#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionTuning.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "Combat/Types/DeliveryCombatEvents.h"
#include "Combat/Types/DeliveryImpactReceiver.h"
#include "Character/DeliveryCharacter.h"
#include "Delivery.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AIController.h"
#include "AIHelpers.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISense_Sight.h"

namespace
{
constexpr uint8 OwnerHitResidentMemory=1;
constexpr uint8 ResidentHitOwnerMemory=2;

bool IsFiniteWitnessPoint(const FVector& Point)
{
    return FMath::IsFinite(Point.X) && FMath::IsFinite(Point.Y) && FMath::IsFinite(Point.Z);
}

bool TryDescribeWitnessedResidentHit(const ADeliveryCompanionCharacter* Npc,
    const UDeliveryCombatHitEvent& Hit,bool bOwnerAttacked,FString& OutDescription)
{
    const auto* Attacker=Hit.Attacker.Get();
    const auto* Victim=Hit.Target.Get();
    const auto* Resident=Cast<ADeliveryNpcCharacter>(bOwnerAttacked?Victim:Attacker);
    UWorld* World=IsValid(Npc)?Npc->GetWorld():nullptr;
    if (!World || Npc->IsActorBeingDestroyed() || Npc->IsHidden() || Npc->IsKnockedDown() ||
        !IsValid(Attacker) || !IsValid(Victim) || Attacker->IsActorBeingDestroyed() || Victim->IsActorBeingDestroyed() ||
        Attacker->GetWorld()!=World || Victim->GetWorld()!=World || Attacker->IsHidden() || Victim->IsHidden() ||
        !Resident || Resident->IsA<ADeliveryCompanionCharacter>() || !IsFiniteWitnessPoint(Hit.ImpactPoint)) return false;
    const FString Name=Resident->DisplayName.ToString().TrimStartAndEnd();
    if (Name.IsEmpty() || Name.Len()>64 || Name==TEXT("居民")) return false;
    for (TCHAR Character:Name)
        if (Character<32 || Character==127) return false;

    const auto* Controller=Cast<AAIController>(Npc->GetController());
    const auto* Perception=IsValid(Controller)?Controller->GetAIPerceptionComponent():nullptr;
    const auto* Sight=IsValid(Perception)?Perception->GetSenseConfig<UAISenseConfig_Sight>():nullptr;
    const FAISenseID SightId=UAISense::GetSenseID<UAISense_Sight>();
    if (!Sight || Controller->GetWorld()!=World || Perception->GetWorld()!=World || !Perception->IsRegistered() ||
        !Perception->IsSenseEnabled(UAISense_Sight::StaticClass()) || !SightId.IsValid() ||
        !Perception->HasActiveStimulus(*Attacker,SightId) || !Perception->HasActiveStimulus(*Victim,SightId) ||
        !FMath::IsFinite(Sight->SightRadius) || Sight->SightRadius<=0 ||
        !FMath::IsFinite(Sight->PeripheralVisionAngleDegrees) ||
        !FMath::IsFinite(Sight->PointOfViewBackwardOffset) || Sight->PointOfViewBackwardOffset<0 ||
        !FMath::IsFinite(Sight->NearClippingRadius) || Sight->NearClippingRadius<0) return false;
    FVector Eye=FVector::ZeroVector;
    FVector Direction=FVector::ZeroVector;
    Perception->GetLocationAndDirection(Eye,Direction);
    if (!IsFiniteWitnessPoint(Eye) || !IsFiniteWitnessPoint(Direction) || !Direction.Normalize()) return false;
    const float PeripheralCos=FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Sight->PeripheralVisionAngleDegrees,0.f,180.f)));
    const float RadiusSquared=FMath::Square(Sight->SightRadius);
    const float NearSquared=FMath::Square(Sight->NearClippingRadius);
    const float FarSquared=FMath::Square(Sight->SightRadius+Sight->PointOfViewBackwardOffset);
    if (!FMath::IsFinite(RadiusSquared) || !FMath::IsFinite(NearSquared) || !FMath::IsFinite(FarSquared)) return false;
    const auto IsInCurrentSight=[&](const FVector& Point)
    {
        return IsFiniteWitnessPoint(Point) && FVector::DistSquared(Eye,Point)<=RadiusSquared &&
            FAISystem::CheckIsTargetInSightCone(Eye,Direction,PeripheralCos,Sight->PointOfViewBackwardOffset,
                NearSquared,FarSquared,Point);
    };
    // 活跃视觉仍可能来自前几帧，双方与接触点都重新检查当前视锥和遮挡
    const FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionWitnessedHit),false,Npc);
    for (const AActor* Participant:{Attacker,Victim})
    {
        const FVector Point=Participant->GetActorLocation();
        if (!IsInCurrentSight(Point)) return false;
        FHitResult Obstacle;
        if (World->LineTraceSingleByChannel(Obstacle,Eye,Point,ECC_Visibility,Params) &&
            Obstacle.GetActor()!=Participant) return false;
    }
    if (!IsInCurrentSight(Hit.ImpactPoint)) return false;
    FHitResult ContactObstacle;
    if (World->LineTraceSingleByChannel(ContactObstacle,Eye,Hit.ImpactPoint,ECC_Visibility,Params) &&
        (ContactObstacle.GetActor()!=Victim ||
            FVector::DistSquared(ContactObstacle.ImpactPoint,Hit.ImpactPoint)>FMath::Square(2.f))) return false;
    OutDescription=bOwnerAttacked?FString::Printf(TEXT("我亲眼看见主人打中了街坊“%s”"),*Name):
        FString::Printf(TEXT("我亲眼看见街坊“%s”打中了主人"),*Name);
    return true;
}
}

void UDeliveryCompanionComponent::RefreshOwnerCombatSubscription()
{
    auto* Npc=GetCompanion();
    if (!IsValid(Npc) || !Npc->HasAuthority()) return;
    auto* AbilityOwner=CanCommand(OwnerPlayerState)?Cast<IAbilitySystemInterface>(OwnerPlayerState.Get()):nullptr;
    auto* Desired=AbilityOwner?AbilityOwner->GetAbilitySystemComponent():nullptr;
    if (BoundOwnerCombat.Get()==Desired && (!Desired || OwnerCombatHandle.IsValid())) return;
    UnbindOwnerCombat();
    if (!IsValid(Desired) || Desired->GetWorld()!=GetWorld()) return;
    BoundOwnerCombat=Desired;
    FGameplayTagContainer Filter(TAG_Event_Combat_ConfirmedHit);
    OwnerCombatHandle=Desired->AddGameplayEventTagContainerDelegate(Filter,
        FGameplayEventTagMulticastDelegate::FDelegate::CreateUObject(this,&ThisClass::OnOwnerConfirmedHit));
}

void UDeliveryCompanionComponent::UnbindOwnerCombat()
{
    if (auto* ASC=BoundOwnerCombat.Get(); ASC && OwnerCombatHandle.IsValid())
    {
        FGameplayTagContainer Filter(TAG_Event_Combat_ConfirmedHit);
        ASC->RemoveGameplayEventTagContainerDelegate(Filter,OwnerCombatHandle);
    }
    OwnerCombatHandle.Reset();
    BoundOwnerCombat.Reset();
}

bool UDeliveryCompanionComponent::IsAllowedAssistTarget(const AActor* Target) const
{
    const auto* Npc=GetCompanion();
    if (!IsValid(Npc) || !IsValid(Target) || Target->IsActorBeingDestroyed() || Target==Npc || Target->GetWorld()!=GetWorld() ||
        !CanCommand(OwnerPlayerState) || Target==GetOwnerPawn() || Target==OwnerPlayerState.Get() ||
        DeliveryCombatEvents::FindPlayerState(Target)==OwnerPlayerState) return false;
    // 旧主人身体、当前驾驶对象和同属主同伴都不能成为护主目标
    if (const auto* Player=Cast<ADeliveryCharacter>(Target); Player && Player->GetTaskPlayerState()==OwnerPlayerState) return false;
    if (const auto* Other=Cast<ADeliveryCompanionCharacter>(Target); Other && Other->Companion->GetOwnerPlayerState()==OwnerPlayerState) return false;
    if (const auto* Bike=Cast<ADeliveryMotorbike>(Target))
    {
        const auto* Driver=Bike->GetDriver();
        if (Driver && Driver->GetTaskPlayerState()==OwnerPlayerState) return false;
    }
    // 车辆、道具等不借护主规则扩展成新的攻击种类
    return Cast<ADeliveryCharacter>(Target)!=nullptr || Cast<IDeliveryImpactReceiver>(Target)!=nullptr;
}

bool UDeliveryCompanionComponent::IsAssistEvidenceCurrent(const AActor* Target,FGuid EvidenceId) const
{
    const auto* Npc=GetCompanion();
    return IsValid(Npc) && Npc->HasAuthority() && !Npc->IsActorBeingDestroyed() && GetWorld() &&
        Mode!=EDeliveryCompanionMode::Unrecruited && Npc->GetEncounterState().Phase==EDeliveryCompanionEncounterPhase::Joined &&
        IsValid(OwnerPlayerState) && IsValid(OwnerPlayerState->GetPlayerController()) &&
        AssistRevision>0 && Target==AssistTarget && IsAllowedAssistTarget(Target) &&
        EvidenceId.IsValid() && EvidenceId==AssistEvidenceId && (bAssistCommitted ||
            (GetWorld()->GetTimeSeconds()<PendingAssistExpiresAt && Npc->Agent->HasObservationEvent(EvidenceId)));
}

bool UDeliveryCompanionComponent::CanJoinAssist(APlayerState* Requester) const
{
    return !bAssistCommitted && IsAssistEvidenceCurrent(AssistTarget,AssistEvidenceId) &&
        (Requester?CanCommand(Requester):StoppedAssistTarget.Get()!=AssistTarget);
}

bool UDeliveryCompanionComponent::CanStopAssist(APlayerState* Requester) const
{
    return bAssistCommitted && CanCommand(Requester) && IsAssistEvidenceCurrent(AssistTarget,AssistEvidenceId);
}

bool UDeliveryCompanionComponent::JoinAssist(APlayerState* Requester,int32 ExpectedAssistRevision)
{
    if (ExpectedAssistRevision!=AssistRevision || !CanJoinAssist(Requester)) return false;
    bAssistCommitted=true;
    StoppedAssistTarget.Reset();
    ++AssistRevision;
    GetOwner()->ForceNetUpdate();
    return true;
}

bool UDeliveryCompanionComponent::StopAssist(APlayerState* Requester,int32 ExpectedAssistRevision)
{
    if (ExpectedAssistRevision!=AssistRevision || !CanStopAssist(Requester)) return false;
    StoppedAssistTarget=AssistTarget.Get();
    AssistStoppedAt=GetWorld()->GetTimeSeconds();
    CancelAssist(TEXT("OwnerStoppedAssist"));
    return true;
}

void UDeliveryCompanionComponent::PauseAssistForOwnerConversation(APlayerState* Requester)
{
    if (CanStopAssist(Requester)) PauseAssistRound(TEXT("OwnerSpeaking"));
}

float UDeliveryCompanionComponent::GetAssistRange() const
{
    return FMath::Clamp(GetTuning().AssistRange,200.f,1200.f);
}

bool UDeliveryCompanionComponent::CanExecuteAssist(AActor* Target,int32 RuleRevision) const
{
    const auto* Npc=GetCompanion();
    const auto* OwnerPawn=GetOwnerPawn();
    if (!bAssistCommitted || RuleRevision!=AssistRevision || !IsAssistEvidenceCurrent(Target,AssistEvidenceId) ||
        !IsValid(Npc) || Npc->IsHidden() || !IsValid(OwnerPawn) || OwnerPawn->IsActorBeingDestroyed() ||
        Target->IsHidden() || (GetCarriedItem() && !GetCarriedTaskPackage())) return false;
    if (const auto* Receiver=Cast<IDeliveryImpactReceiver>(Target); Receiver && Receiver->IsKnockedDown()) return false;
    const float Range=GetAssistRange();
    return FVector::DistSquared(Npc->GetActorLocation(),Target->GetActorLocation())<=FMath::Square(Range) &&
        FVector::DistSquared(OwnerPawn->GetActorLocation(),Target->GetActorLocation())<=FMath::Square(Range);
}

void UDeliveryCompanionComponent::OnOwnerConfirmedHit(FGameplayTag EventTag,const FGameplayEventData* EventData)
{
    auto* Npc=GetCompanion();
    if (!IsValid(Npc) || !Npc->HasAuthority() || !CanCommand(OwnerPlayerState) ||
        Mode==EDeliveryCompanionMode::Unrecruited || Npc->GetEncounterState().Phase!=EDeliveryCompanionEncounterPhase::Joined ||
        EventTag!=TAG_Event_Combat_ConfirmedHit || !EventData) return;
    const auto* Hit=Cast<UDeliveryCombatHitEvent>(EventData->OptionalObject.Get());
    if (!Hit || !Hit->PunchId.IsValid() ||
        !Hit->Attacker.IsValid() || !Hit->Target.IsValid() || Hit->ImpactPoint.ContainsNaN() ||
        Hit->Attacker->GetWorld()!=GetWorld() || Hit->Target->GetWorld()!=GetWorld() ||
        EventData->Instigator.Get()!=Hit->Attacker.Get() || EventData->Target.Get()!=Hit->Target.Get()) return;
    const bool bOwnerAttacked=Hit->AttackerPlayerState.Get()==OwnerPlayerState;
    const bool bOwnerWasHit=Hit->TargetPlayerState.Get()==OwnerPlayerState;
    if (bOwnerAttacked==bOwnerWasHit) return;
    if (bOwnerAttacked && Hit->Target.Get()==Npc)
    {
        RecordOwnerPunch(Hit->PunchId,Npc->WasDownBeforeMeleeHit(Hit->PunchId));
        return;
    }
    if (RecentAssistHits.Contains(Hit->PunchId)) return;
    RecentAssistHits.Add(Hit->PunchId);
    if (RecentAssistHits.Num()>128) RecentAssistHits.RemoveAt(0);
    auto* Target=bOwnerAttacked?Hit->Target.Get():Hit->Attacker.Get();
    if (!IsAllowedAssistTarget(Target)) return;
    // 喊停之后只有主人自己又动手打这个对手，才重新算作要帮忙；留三秒避开喊停前已经挥出去的拳
    if (bOwnerAttacked && StoppedAssistTarget.Get()==Target && GetWorld()->GetTimeSeconds()-AssistStoppedAt>=3.)
        StoppedAssistTarget.Reset();
    const float Range=GetAssistRange();
    if (FVector::DistSquared(Npc->GetActorLocation(),Hit->ImpactPoint)>FMath::Square(Range) ||
        FVector::DistSquared(Npc->GetActorLocation(),Target->GetActorLocation())>FMath::Square(Range)) return;

    const float Lifetime=FMath::Clamp(GetTuning().AssistLifetimeSeconds,5.f,60.f);
    FDeliveryNpcObservation Fact;
    Fact.EventId=Hit->PunchId;
    Fact.Kind=bOwnerAttacked?TEXT("OwnerConfirmedAttack"):TEXT("OwnerConfirmedHit");
    Fact.Source=EDeliveryNpcEvidenceSource::ActionResult;
    Fact.Subject=Target;
    Fact.Description=bOwnerAttacked?TEXT("主人刚刚实际打中了这个对象，可以商量加入这场协战"):
        TEXT("这个对象刚刚实际打中了主人，可以商量加入这场协战");
    Fact.Lifetime=Lifetime;
    Fact.bHasObservedLocation=true;
    Fact.ObservedLocation=Hit->ImpactPoint;
    Fact.bNeedsReply=false;
    const double Now=GetWorld()->GetTimeSeconds();
    const TWeakObjectPtr<APlayerState> HitOwner(OwnerPlayerState.Get());
    const int32 HitCommandRevision=CommandRevision;
    if (!Npc->Agent->ObserveEvent(Fact)) return;
    const bool bContinuedAssist=AssistTarget==Target && (bAssistCommitted ||
        (Now<PendingAssistExpiresAt && Npc->Agent->HasObservationEvent(AssistEvidenceId)));
    // 已确认的协战始终跟当前对手，别的真实命中只记作当前所见
    if (bAssistCommitted && !bContinuedAssist) return;
    if (!bContinuedAssist)
    {
        CancelAssist(TEXT("AssistTargetChanged"));
        if (!IsValid(this) || !IsValid(Npc) || HitOwner.Get()!=OwnerPlayerState || HitCommandRevision!=CommandRevision ||
            !IsAllowedAssistTarget(Target)) return;
        AssistTarget=Target;
        ++AssistRevision;
        Target->OnDestroyed.AddUniqueDynamic(this,&ThisClass::OnAssistTargetDestroyed);
        AssistEvidenceId=Fact.EventId;
        PendingAssistExpiresAt=Now+Lifetime;
        RememberedAssistHitDirections=0;
        UE_LOG(LogDelivery,Log,TEXT("CompanionAssistPending npc=%s owner=%s target=%s punch=%s revision=%d expires_in=%.1f distance=%.1f"),
            *GetNameSafe(Npc),*GetNameSafe(HitOwner.Get()),*GetNameSafe(Target),*AssistEvidenceId.ToString(),AssistRevision,Lifetime,
            FVector::Dist(Npc->GetActorLocation(),Target->GetActorLocation()));
        // 主人真实动手或挨打后直接加入协战，不等模型选择；主人对这个对手喊过停手时仍交给模型判断
        if (CanJoinAssist(nullptr))
        {
            if (bJoinAssistWithoutModel) JoinAssist(nullptr,AssistRevision);
            if (!IsValid(this) || !IsValid(Npc)) return;
            Npc->WakeDecision();
        }
    }
    const uint8 DirectionBit=bOwnerAttacked?OwnerHitResidentMemory:ResidentHitOwnerMemory;
    FString WitnessDescription;
    // 每个方向只留一次亲眼目睹的真实命中，未看见时不占本轮记录位置
    if (HitOwner.IsValid() && HitOwner.Get()==OwnerPlayerState && HitCommandRevision==CommandRevision &&
        !(RememberedAssistHitDirections&DirectionBit) &&
        TryDescribeWitnessedResidentHit(Npc,*Hit,bOwnerAttacked,WitnessDescription) &&
        RecordMemory(Hit->PunchId,bOwnerAttacked?TEXT("WitnessedOwnerHitResident"):TEXT("WitnessedResidentHitOwner"),
            WitnessDescription,0)) RememberedAssistHitDirections|=DirectionBit;
    Npc->ForceNetUpdate();
}

void UDeliveryCompanionComponent::CancelAssist(FName Reason)
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority()) return;
    if (Reason==TEXT("OwnerChangedArrangement"))
    {
        // 主人给了新安排就停手，和明说别打了一样记下这个对手，之后主人挨打也不再自动冲上去
        if (!bAssistCommitted) return;
        StoppedAssistTarget=AssistTarget.Get();
        AssistStoppedAt=GetWorld()->GetTimeSeconds();
    }
    const int32 OldRevision=AssistRevision;
    const auto State=Npc->Agent->GetPublicState();
    const bool bHadAssist=AssistTarget!=nullptr || AssistEvidenceId.IsValid();
    if (IsValid(AssistTarget)) AssistTarget->OnDestroyed.RemoveDynamic(this,&ThisClass::OnAssistTargetDestroyed);
    AssistTarget=nullptr;
    bAssistCommitted=false;
    AssistEvidenceId.Invalidate();
    PendingAssistExpiresAt=0;
    RememberedAssistHitDirections=0;
    if (Reason==TEXT("OwnerLost") || Reason==TEXT("CompanionEndPlay")) StoppedAssistTarget.Reset();
    if (bHadAssist) ++AssistRevision;
    // 先撤销授权再结束对应规则动作，模型对白与新代次不受旧取消通知影响
    if (IsValid(Npc) && State.Origin==EDeliveryNpcActionOrigin::GameplayRule && State.RuleRevision==OldRevision && Npc->Agent->HasActiveAction())
        Npc->Agent->TryFinishAction(State.ActionId,EDeliveryNpcActionStatus::Cancelled,Reason);
    if (bHadAssist && IsValid(Npc)) Npc->ForceNetUpdate();
}

void UDeliveryCompanionComponent::PauseAssistRound(FName Reason)
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority() || !bAssistCommitted) return;
    const auto State=Npc->Agent->GetPublicState();
    if (State.Action==EDeliveryNpcAction::Fight && State.Target==AssistTarget && State.RuleRevision==AssistRevision &&
        State.Origin==EDeliveryNpcActionOrigin::GameplayRule && Npc->Agent->HasActiveAction())
        Npc->Agent->TryFinishAction(State.ActionId,EDeliveryNpcActionStatus::Cancelled,Reason);
}

void UDeliveryCompanionComponent::OnAssistTargetDestroyed(AActor* DestroyedActor)
{
    if (DestroyedActor==AssistTarget) CancelAssist(TEXT("AssistTargetLost"));
}

void UDeliveryCompanionComponent::UpdateAssist()
{
    auto* Npc=GetCompanion();
    if (!IsValid(Npc) || !Npc->HasAuthority() || (!AssistTarget && !AssistEvidenceId.IsValid())) return;
    if (!IsAssistEvidenceCurrent(AssistTarget,AssistEvidenceId))
    {
        UE_LOG(LogDelivery,Log,TEXT("CompanionAssistInvalid npc=%s target=%s punch=%s revision=%d committed=%d proof_valid=%d pending_remaining=%.2f"),
            *GetNameSafe(Npc),*GetNameSafe(AssistTarget.Get()),*AssistEvidenceId.ToString(),AssistRevision,bAssistCommitted,
            Npc->Agent->HasObservationEvent(AssistEvidenceId),PendingAssistExpiresAt-GetWorld()->GetTimeSeconds());
        CancelAssist(TEXT("AssistNoLongerValid"));
        return;
    }
    if (!CanExecuteAssist(AssistTarget,AssistRevision)) return;
    auto* Agent=Npc->Agent.Get();
    // 玩家正在说话或等待本次回复时保留目标，不能用每轮续打抢走停止命令
    // 没人问话时自己的等待、张望和闲聊请求让位给已答应的协战，答应后不再干站几秒
    const auto IsBusy=[Agent]()
    {
        if (Agent->IsListening() || Agent->IsWaitingToReplyToPlayer()) return true;
        if (!Agent->HasActiveAction()) return false;
        const auto State=Agent->GetPublicState();
        const bool bIdleChat=State.Origin==EDeliveryNpcActionOrigin::Model && !State.ReplyToEventId.IsValid() &&
            (State.Action==EDeliveryNpcAction::Wait || State.Action==EDeliveryNpcAction::LookAt);
        return !bIdleChat;
    };
    if (Npc->IsKnockedDown() || Npc->Combat->IsReactingToHit() || IsBusy()) return;
    if (HasActiveErrand() || HasActiveHomeArrangement() || HomeState.CarriedProp || PendingHomeArrangementId.IsValid()) return;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const TWeakObjectPtr<AActor> Target(AssistTarget.Get());
    const int32 Revision=AssistRevision;
    const auto StillCurrent=[&]()
    {
        return WeakThis.IsValid() && WeakNpc.IsValid() && Target.IsValid() && CanExecuteAssist(Target.Get(),Revision) &&
            !HasActiveErrand() && !HasActiveHomeArrangement() && !HomeState.CarriedProp && !PendingHomeArrangementId.IsValid() &&
            !IsBusy();
    };
    if (!EnsureOnFoot() || !StillCurrent() || !Npc->Combat->CanBeginFight(Target.Get(),GetAssistRange())) return;
    StopErrandMovement();
    if (!StillCurrent()) return;
    StopFollowMovement();
    if (!StillCurrent()) return;
    const bool bAccepted=Agent->RequestGameplayRuleAction(EDeliveryNpcAction::Fight,Target.Get(),Revision,
        AssistEvidenceId,FMath::Clamp(GetTuning().AssistActionSeconds,UDeliveryNpcCombatComponent::StrikeDuration,5.f));
    if (bAccepted && WeakThis.IsValid() && WeakNpc.IsValid() && AssistRevision==Revision && AssistTarget==Target.Get() && bAssistCommitted)
        SetSuspension(TEXT("AssistCombat"));
}
