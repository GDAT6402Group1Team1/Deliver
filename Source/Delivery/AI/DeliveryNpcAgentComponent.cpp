#include "AI/DeliveryNpcAgentComponent.h"
#include "AI/Residents/DeliveryResidentCharacter.h"
#include "AI/DeliveryAiGatewaySubsystem.h"
#include "Algo/AllOf.h"
#include "AIController.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeliveryNpcAgent, Log, All);

UDeliveryNpcAgentComponent::UDeliveryNpcAgentComponent()
{
    SetIsReplicatedByDefault(true);
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickInterval = .1f;
}

void UDeliveryNpcAgentComponent::BeginPlay()
{
    Super::BeginPlay();
    if (IsAuthority()) NpcId = FGuid::NewGuid();
    else SetComponentTickEnabled(false);
}

bool UDeliveryNpcAgentComponent::IsAuthority() const
{
    return !bEndingPlay && GetOwner() && GetOwner()->HasAuthority() && GetWorld() && GetWorld()->GetNetMode() != NM_Client;
}

double UDeliveryNpcAgentComponent::ServerTime() const
{
    if (const auto* State = GetWorld()->GetGameState()) return State->GetServerWorldTimeSeconds();
    return GetWorld()->GetTimeSeconds();
}

void UDeliveryNpcAgentComponent::PruneEvidence()
{
    const double Now = GetWorld()->GetTimeSeconds();
    if (Evidence.RemoveAll([Now](const auto& Item) { return Item.ExpiresAt <= Now; }) > 0) ++ContextRevision;
    for (auto It = KnownSubjects.CreateIterator(); It; ++It)
    {
        if (!It.Value().IsValid() || !Evidence.ContainsByPredicate([&It](const auto& Item) { return Item.SubjectId == It.Key(); }))
            It.RemoveCurrent();
    }
}

bool UDeliveryNpcAgentComponent::ObserveEvent(const FDeliveryNpcObservation& Observation)
{
    if (!IsAuthority() || Observation.Kind.IsNone() || !FMath::IsFinite(Observation.Lifetime) || Observation.Lifetime <= 0 ||
        (Observation.bHasObservedLocation && Observation.ObservedLocation.ContainsNaN()) ||
        (Observation.Subject && (!IsValid(Observation.Subject) || Observation.Subject->GetWorld() != GetWorld()))) return false;
    const FGuid EventId = Observation.EventId.IsValid() ? Observation.EventId : FGuid::NewGuid();
    if (RecentEventIds.Contains(EventId)) return false;
    PruneEvidence();
    FDeliveryNpcEvidence Item;
    Item.EventId = EventId;
    Item.Kind = Observation.Kind;
    Item.Source = Observation.Source;
    Item.Description = Observation.Description.Left(512);
    Item.ObservedAt = GetWorld()->GetTimeSeconds();
    Item.ExpiresAt = Item.ObservedAt + FMath::Clamp(Observation.Lifetime, .1f, 60.f);
    Item.bHasObservedLocation = Observation.bHasObservedLocation;
    Item.ObservedLocation = Observation.bHasObservedLocation ? Observation.ObservedLocation : FVector::ZeroVector;
    if (Observation.Subject)
    {
        for (const auto& Pair : KnownSubjects)
            if (Pair.Value == Observation.Subject) { Item.SubjectId = Pair.Key; break; }
        if (!Item.SubjectId.IsValid()) Item.SubjectId = FGuid::NewGuid();
        KnownSubjects.Add(Item.SubjectId, Observation.Subject);
    }
    Evidence.Add(MoveTemp(Item));
    if (Evidence.Num() > 32) Evidence.RemoveAt(0);
    RecentEventIds.Add(EventId);
    if (RecentEventIds.Num() > 128) RecentEventIds.RemoveAt(0);
    ++ContextRevision;
    return true;
}

bool UDeliveryNpcAgentComponent::BuildDecisionRequest(FGuid WorldEpoch, FDeliveryNpcDecisionRequest& OutRequest)
{
    if (!IsAuthority() || !WorldEpoch.IsValid() || HasActiveAction()) return false;
    PruneEvidence();
    if (!NpcId.IsValid()) NpcId = FGuid::NewGuid();
    OutRequest = {};
    OutRequest.WorldEpoch = WorldEpoch;
    OutRequest.NpcId = NpcId;
    OutRequest.RequestId = FGuid::NewGuid();
    OutRequest.ContextRevision = ContextRevision;
    OutRequest.PlanRevision = ++NextPlanRevision;
    OutRequest.Persona = Persona.Left(1024);
    OutRequest.Evidence = Evidence;
    OutRequest.AllowedActions.Add(EDeliveryNpcAction::Wait);
    for (const auto& Pair : KnownSubjects)
        if (CanLookAt(Pair.Value.Get())) { OutRequest.AllowedActions.Add(EDeliveryNpcAction::LookAt); break; }
    if (auto* Resident=Cast<ADeliveryResidentCharacter>(GetOwner()))
    {
        auto AddCandidate=[&](EDeliveryNpcAction Action,FGuid TargetId,FGuid EvidenceId)
        {
            OutRequest.AllowedActions.AddUnique(Action);
            FDeliveryNpcCandidate Candidate; Candidate.Action=Action; Candidate.TargetId=TargetId;
            if (EvidenceId.IsValid()) Candidate.EvidenceIds.Add(EvidenceId);
            OutRequest.Candidates.Add(Candidate);
        };
        for (auto Action : {EDeliveryNpcAction::Wait,EDeliveryNpcAction::LieDown,EDeliveryNpcAction::ReleaseVehicle})
            if (Resident->CanExecute(Action,nullptr)) AddCandidate(Action,{},{});
        for (const auto& Pair : KnownSubjects)
        {
            AActor* Target=Pair.Value.Get();
            if (!CanLookAt(Target)) continue;
            const int32 FactIndex=Evidence.FindLastByPredicate([&](const auto& Item){ return Item.SubjectId==Pair.Key; });
            if (FactIndex==INDEX_NONE) continue;
            const auto* Fact=&Evidence[FactIndex];
            for (auto Action : {EDeliveryNpcAction::LookAt,EDeliveryNpcAction::MoveTo,EDeliveryNpcAction::StrikeOnce,EDeliveryNpcAction::HoldVehicle})
                if (Resident->CanExecute(Action,Target)) AddCandidate(Action,Pair.Key,Fact->EventId);
        }
    }
    return true;
}

bool UDeliveryNpcAgentComponent::RequestDecision()
{
    if (!IsAuthority() || HasActiveAction()) return false;
    if (IsDecisionPending()) { bDecisionQueued = true; return true; }
    auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>();
    if (!Gateway || !BuildDecisionRequest(Gateway->GetWorldEpoch(), ActiveRequest)) return false;
    if (!Gateway->Enqueue(this, ActiveRequest)) { ActiveRequest = {}; return false; }
    bDecisionPending=true; GetOwner()->ForceNetUpdate();
    return true;
}

void UDeliveryNpcAgentComponent::CancelDecision()
{
    if (!IsAuthority()) return;
    if (auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>()) Gateway->Cancel(this);
    ActiveRequest = {};
    bDecisionPending=false;
    bDecisionQueued = false;
}

bool UDeliveryNpcAgentComponent::CanLookAt(AActor* Target) const
{
    const auto* Pawn = Cast<APawn>(GetOwner());
    if (!Pawn || !Cast<AAIController>(Pawn->GetController()) || !IsValid(Target) || Target == GetOwner() ||
        Target->GetWorld() != GetWorld() || FVector::DistSquared(Target->GetActorLocation(), GetOwner()->GetActorLocation()) >
        FMath::Square(FMath::Clamp(MaxLookAtDistance, 100.f, 3000.f))) return false;
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(DeliveryNpcLookAt), false, GetOwner());
    // 居民视线取躯干高度，车辆根点可能位于地面，不能把地面误判成目标前的墙
    const FVector Offset=Cast<ADeliveryResidentCharacter>(GetOwner())?FVector(0,0,40):FVector::ZeroVector;
    return !GetWorld()->LineTraceSingleByChannel(Hit, GetOwner()->GetActorLocation()+Offset, Target->GetActorLocation()+Offset, ECC_Visibility, Params) ||
        Hit.GetActor() == Target;
}

bool UDeliveryNpcAgentComponent::TryAcceptProposal(const FDeliveryNpcDecisionProposal& Proposal, FName& Reason)
{
    PruneEvidence();
    Reason = TEXT("StaleProposal");
    const bool bResident=GetOwner()->IsA<ADeliveryResidentCharacter>();
    // 回包必须对应发出时的快照，普通感知更新和无关证据过期不能吞掉已经生成的台词
    // 非居民仍沿用基础 Agent 的严格版本契约；受击等硬中断通过 CancelPlan 作废请求编号
    if (Proposal.RequestId != ActiveRequest.RequestId || Proposal.WorldEpoch != ActiveRequest.WorldEpoch ||
        Proposal.NpcId != NpcId || Proposal.ContextRevision != ActiveRequest.ContextRevision ||
        (!bResident && Proposal.ContextRevision != ContextRevision) ||
        Proposal.PlanRevision != ActiveRequest.PlanRevision || HasActiveAction()) return false;
    // 新的玩家话题需要合并后重答，不能让旧回应误标记后来的玩家消息已回答
    if (bResident && Evidence.ContainsByPredicate([this](const auto& Item)
        {
            return Item.Kind==TEXT("PlayerSpeech") && !ActiveRequest.Evidence.ContainsByPredicate(
                [&Item](const auto& Sent){ return Sent.EventId==Item.EventId; });
        })) return false;
    Reason = TEXT("UnsupportedAction");
    if (!ActiveRequest.AllowedActions.Contains(Proposal.Action)) return false;
    Reason = TEXT("InvalidEvidence");
    for (const auto& Id : Proposal.EvidenceIds)
        if (!Evidence.ContainsByPredicate([&Id](const auto& Item) { return Item.EventId == Id; })) return false;
    AActor* Target = nullptr;
    Reason = TEXT("InvalidTarget");
    if (Proposal.Action == EDeliveryNpcAction::LookAt || Proposal.Action == EDeliveryNpcAction::MoveTo ||
        Proposal.Action == EDeliveryNpcAction::StrikeOnce || Proposal.Action == EDeliveryNpcAction::HoldVehicle)
    {
        Target = KnownSubjects.FindRef(Proposal.TargetId).Get();
        if (!CanLookAt(Target) || !Evidence.ContainsByPredicate([&Proposal](const auto& Item)
            { return Item.SubjectId == Proposal.TargetId && Proposal.EvidenceIds.Contains(Item.EventId); })) return false;
    }
    else if (Proposal.TargetId.IsValid()) return false;
    if (auto* Resident=Cast<ADeliveryResidentCharacter>(GetOwner()))
    {
        if (!Resident->CanExecute(Proposal.Action,Target)) return false;
        const bool bOffered=ActiveRequest.Candidates.ContainsByPredicate([&](const auto& Item)
            { return Item.Action==Proposal.Action && Item.TargetId==Proposal.TargetId; });
        if (!bOffered) return false;
    }
    Reason = TEXT("InvalidDuration");
    if (!FMath::IsFinite(Proposal.Duration) || Proposal.Duration < .1f || Proposal.Duration > 5.f) return false;
    PublicState = {};
    PublicState.ActionId = FGuid::NewGuid();
    PublicState.PlanRevision = Proposal.PlanRevision;
    PublicState.Action = Proposal.Action;
    PublicState.Status = EDeliveryNpcActionStatus::Accepted;
    PublicState.Target = Target;
    PublicState.Duration = Proposal.Duration;
    PublicState.Speech=Proposal.Speech.Left(120); PublicState.Emotion=Proposal.Emotion;
    ActionTargetId = Proposal.TargetId;
    ActionEvidenceIds = Proposal.EvidenceIds;
    // BT 未领取也有截止时间，避免没有运行行为树时永远占用动作
    ActionDeadline = GetWorld()->GetTimeSeconds() + 1.0;
    Reason = NAME_None;
    return true;
}

void UDeliveryNpcAgentComponent::ReceiveDecision(FGuid RequestId, const FDeliveryNpcDecisionProposal* Proposal, FName Error)
{
    if (!IsAuthority() || RequestId != ActiveRequest.RequestId) return;
    const bool bAccepted = Error.IsNone() && Proposal && TryAcceptProposal(*Proposal, Error);
    const bool bRetry = bDecisionQueued && !bAccepted;
    ActiveRequest = {};
    bDecisionPending=false;
    bDecisionQueued = false;
    UE_LOG(LogDeliveryNpcAgent, Display, TEXT("Decision npc=%s request=%s accepted=%d reason=%s"),
        *NpcId.ToString(), *RequestId.ToString(), bAccepted, *Error.ToString());
    if (bAccepted) PublishState();
    OnDecisionFinished.Broadcast(Error);
    if (bRetry && !IsDecisionPending() && !HasActiveAction()) RequestDecision();
}

bool UDeliveryNpcAgentComponent::HasActiveAction() const
{
    return PublicState.Status == EDeliveryNpcActionStatus::Accepted || PublicState.Status == EDeliveryNpcActionStatus::Running;
}

bool UDeliveryNpcAgentComponent::StartApprovedAction(FDeliveryNpcPublicState& OutAction)
{
    if (!IsAuthority() || PublicState.Status != EDeliveryNpcActionStatus::Accepted) return false;
    PruneEvidence();
    const bool bEvidenceValid = Algo::AllOf(ActionEvidenceIds, [this](const FGuid& Id)
        { return Evidence.ContainsByPredicate([&Id](const auto& Item) { return Item.EventId == Id; }); });
    if (GetWorld()->GetTimeSeconds() >= ActionDeadline || !bEvidenceValid ||
        (PublicState.Action == EDeliveryNpcAction::LookAt && (!KnownSubjects.Contains(ActionTargetId) || !CanLookAt(PublicState.Target))))
    {
        TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Failed, TEXT("PreconditionChanged"));
        return false;
    }
    if (auto* Resident=Cast<ADeliveryResidentCharacter>(GetOwner()))
        if (!Resident->CanExecute(PublicState.Action,PublicState.Target))
        { TryFinishAction(PublicState.ActionId,EDeliveryNpcActionStatus::Failed,TEXT("PreconditionChanged")); return false; }
    PublicState.Status = EDeliveryNpcActionStatus::Running;
    PublicState.ServerStartTime = ServerTime();
    ActionDeadline = GetWorld()->GetTimeSeconds() + PublicState.Duration + 1.0;
    OutAction = PublicState;
    PublishState();
    return true;
}

bool UDeliveryNpcAgentComponent::TryFinishAction(FGuid ActionId, EDeliveryNpcActionStatus Result, FName Reason)
{
    if (!IsAuthority() || !HasActiveAction() || ActionId != PublicState.ActionId || !DeliveryNpc::IsTerminal(Result) ||
        (Result == EDeliveryNpcActionStatus::Succeeded && PublicState.Status != EDeliveryNpcActionStatus::Running)) return false;
    // 先写终态再广播，重入和迟到的完成回调都无法二次结束同一个动作
    PublicState.Status = Result;
    PublicState.Reason = Reason;
    UE_LOG(LogDeliveryNpcAgent, Display, TEXT("Action npc=%s action=%s revision=%d result=%s reason=%s"),
        *NpcId.ToString(), *ActionId.ToString(), PublicState.PlanRevision,
        *StaticEnum<EDeliveryNpcActionStatus>()->GetNameStringByValue(static_cast<int64>(Result)), *Reason.ToString());
    PublicState.Target = nullptr;
    ActionEvidenceIds.Reset();
    ActionTargetId.Invalidate();
    FDeliveryNpcObservation ResultEvent;
    ResultEvent.EventId = FGuid::NewGuid();
    ResultEvent.Kind = TEXT("ActionFinished");
    ResultEvent.Source = EDeliveryNpcEvidenceSource::ActionResult;
    ResultEvent.Description = FString::Printf(TEXT("%s %s %s"), *DeliveryNpc::ActionName(PublicState.Action),
        *StaticEnum<EDeliveryNpcActionStatus>()->GetNameStringByValue(static_cast<int64>(Result)), *Reason.ToString());
    ResultEvent.Lifetime = 30.f;
    ObserveEvent(ResultEvent);
    PublishState();
    return true;
}

void UDeliveryNpcAgentComponent::CancelPlan(FName Reason)
{
    if (!IsAuthority()) return;
    CancelDecision();
    TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Cancelled, Reason);
}

void UDeliveryNpcAgentComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!IsAuthority()) return;
    if (HasActiveAction() && GetWorld()->GetTimeSeconds() >= ActionDeadline)
        TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Failed, TEXT("ExecutorTimeout"));
    else if (PublicState.Status == EDeliveryNpcActionStatus::Running && PublicState.Action == EDeliveryNpcAction::LookAt && !CanLookAt(PublicState.Target))
        TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Cancelled, TEXT("TargetUnavailable"));
}

void UDeliveryNpcAgentComponent::PublishState()
{
    GetOwner()->ForceNetUpdate();
    OnRep_PublicState();
}

void UDeliveryNpcAgentComponent::OnRep_PublicState() { OnPublicStateChanged.Broadcast(PublicState); }

void UDeliveryNpcAgentComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UDeliveryNpcAgentComponent, PublicState);
    DOREPLIFETIME(UDeliveryNpcAgentComponent,bDecisionPending);
}

void UDeliveryNpcAgentComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 先禁止新请求，不向即将销毁的外部表现广播，避免 EndPlay 回调重入
    bEndingPlay = true;
    if (GetWorld())
        if (auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>()) Gateway->Cancel(this);
    ActiveRequest = {};
    bDecisionPending=false;
    bDecisionQueued = false;
    if (HasActiveAction())
    {
        PublicState.Status = EDeliveryNpcActionStatus::Cancelled;
        PublicState.Reason = TEXT("EndPlay");
        PublicState.Target = nullptr;
    }
    Evidence.Reset();
    KnownSubjects.Reset();
    OnPublicStateChanged.Clear();
    OnDecisionFinished.Clear();
    Super::EndPlay(EndPlayReason);
}
