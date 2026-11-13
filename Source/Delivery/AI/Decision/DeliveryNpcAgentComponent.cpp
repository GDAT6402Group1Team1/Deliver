#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Conversation/DeliveryNpcConversationTrace.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Algo/AllOf.h"
#include "AIController.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Vehicle/DeliveryMotorbike.h"
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
        if (!It.Value().IsValid() || !Evidence.ContainsByPredicate([&It](const auto& Item)
            { return Item.SubjectId == It.Key() || Item.AddresseeId == It.Key(); }))
            It.RemoveCurrent();
    }
}

bool UDeliveryNpcAgentComponent::ObserveEvent(const FDeliveryNpcObservation& Observation)
{
    if (!IsAuthority() || Observation.Kind.IsNone() || !FMath::IsFinite(Observation.Lifetime) || Observation.Lifetime <= 0 ||
        (Observation.bHasObservedLocation && Observation.ObservedLocation.ContainsNaN()) ||
        (Observation.Subject && (!IsValid(Observation.Subject) || Observation.Subject->GetWorld() != GetWorld())) ||
        (Observation.Addressee && (!IsValid(Observation.Addressee) || Observation.Addressee->GetWorld() != GetWorld()))) return false;
    const FGuid EventId = Observation.EventId.IsValid() ? Observation.EventId : FGuid::NewGuid();
    if (RecentEventIds.Contains(EventId)) return false;
    PruneEvidence();
    FDeliveryNpcEvidence Item;
    Item.EventId = EventId;
    Item.ReplyToEventId = Observation.ReplyToEventId;
    Item.Kind = Observation.Kind;
    Item.Source = Observation.Source;
    Item.Description = Observation.Description.Left(512);
    Item.ObservedAt = GetWorld()->GetTimeSeconds();
    Item.ExpiresAt = Item.ObservedAt + FMath::Clamp(Observation.Lifetime, .1f, 60.f);
    Item.bHasObservedLocation = Observation.bHasObservedLocation;
    Item.ObservedLocation = Observation.bHasObservedLocation ? Observation.ObservedLocation : FVector::ZeroVector;
    auto RememberSubject = [this](AActor* Actor)
    {
        if (!Actor) return FGuid();
        if (Actor == GetOwner() && !NpcId.IsValid()) NpcId = FGuid::NewGuid();
        for (const auto& Pair : KnownSubjects)
            if (Pair.Value == Actor) return Pair.Key;
        const FGuid SubjectId = Actor == GetOwner() ? NpcId : FGuid::NewGuid();
        KnownSubjects.Add(SubjectId, Actor);
        return SubjectId;
    };
    Item.SubjectId = RememberSubject(Observation.Subject);
    Item.AddresseeId = RememberSubject(Observation.Addressee);
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
    OutRequest.bPlayerRequest = bPlayerRequest;
    OutRequest.SelfSubjectId = NpcId;
    OutRequest.SelfName = TEXT("NPC");
    if (const auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        OutRequest.Voice = Npc->Presentation->TtsVoice.ToString();
        OutRequest.SelfName = Npc->DisplayName.ToString();
        if (!Npc->Profile.RowName.IsNone()) OutRequest.ProfileId = Npc->Profile.RowName.ToString();
    }
    OutRequest.Subjects.Add({NpcId, OutRequest.SelfName.Left(64), TEXT("npc")});
    for (const auto& Pair : KnownSubjects)
    {
        const AActor* Actor = Pair.Value.Get();
        if (!Actor || Actor == GetOwner()) continue;
        FDeliveryNpcSubject Subject;
        Subject.SubjectId = Pair.Key;
        if (const auto* Npc = Cast<ADeliveryNpcCharacter>(Actor))
        { Subject.Name = Npc->DisplayName.ToString(); Subject.Kind = TEXT("npc"); }
        else if (const auto* Pawn = Cast<APawn>(Actor); Pawn && Pawn->IsPlayerControlled())
        {
            const auto* Player = Pawn->GetPlayerState();
            Subject.Name = Player ? FString::Printf(TEXT("快递员%d"), Player->GetPlayerId()) : TEXT("快递员");
            Subject.Kind = TEXT("player");
        }
        else if (Actor->IsA<ADeliveryMotorbike>())
        { Subject.Name = TEXT("电瓶车"); Subject.Kind = TEXT("vehicle"); }
        else { Subject.Name = TEXT("附近的对象"); Subject.Kind = TEXT("other"); }
        Subject.Name = Subject.Name.Left(64);
        OutRequest.Subjects.Add(MoveTemp(Subject));
    }
    OutRequest.Evidence = Evidence;
    OutRequest.AllowedActions.Add(EDeliveryNpcAction::Wait);
    for (const auto& Pair : KnownSubjects)
        if (CanLookAt(Pair.Value.Get())) { OutRequest.AllowedActions.Add(EDeliveryNpcAction::LookAt); break; }
    if (auto* Resident=Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        auto AddCandidate=[&](EDeliveryNpcAction Action,FGuid TargetId,FGuid EvidenceId)
        {
            OutRequest.AllowedActions.AddUnique(Action);
            FDeliveryNpcCandidate Candidate; Candidate.Action=Action; Candidate.TargetId=TargetId;
            if (EvidenceId.IsValid()) Candidate.EvidenceIds.Add(EvidenceId);
            OutRequest.Candidates.Add(Candidate);
        };
        for (auto Action : {EDeliveryNpcAction::Wait,EDeliveryNpcAction::LieDown,EDeliveryNpcAction::ReleaseVehicle,EDeliveryNpcAction::Pace})
            if (Resident->CanExecute(Action,nullptr)) AddCandidate(Action,{},{});
        for (const auto& Pair : KnownSubjects)
        {
            AActor* Target=Pair.Value.Get();
            if (!CanLookAt(Target)) continue;
            const int32 FactIndex=Evidence.FindLastByPredicate([&](const auto& Item){ return Item.SubjectId==Pair.Key; });
            if (FactIndex==INDEX_NONE) continue;
            const auto* Fact=&Evidence[FactIndex];
            for (auto Action : {EDeliveryNpcAction::LookAt,EDeliveryNpcAction::MoveTo,EDeliveryNpcAction::StrikeOnce,EDeliveryNpcAction::HoldVehicle,EDeliveryNpcAction::MoveAside})
                if (Resident->CanExecute(Action,Target)) AddCandidate(Action,Pair.Key,Fact->EventId);
        }
    }
    return true;
}

bool UDeliveryNpcAgentComponent::RequestDecision()
{
    if (!IsAuthority()) return false;
    if (IsDecisionPending()) { bDecisionQueued = true; return true; }
    if (HasActiveAction()) return false;
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
    ReplyRequest.Invalidate();
    ReplyAction.Invalidate();
}

void UDeliveryNpcAgentComponent::BeginListening(AActor* Speaker, FGuid MessageId)
{
    if (!IsAuthority() || !IsValid(Speaker)) return;
    // 新后端支持实际取消；旧后端沿用合并请求，避免重复占用模型
    if (GetDefault<UDeliveryAiSettings>()->bStreamReplies) CancelPlan(TEXT("PlayerInterrupted"));
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        Npc->StopAction(PublicState.ActionId);
        Npc->Presentation->CancelSpeech();
    }
    ListeningSpeaker = Speaker;
    ListeningMessage = MessageId;
    ListeningUntil = FPlatformTime::Seconds() + 30;
    bPlayerRequest = true;
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
        Npc->Presentation->PlayReaction(EDeliveryNpcVoiceReaction::HeardPlayer,MessageId);
    GetOwner()->ForceNetUpdate();
    UE_LOG(LogDeliveryNpcAgent, Log, TEXT("NpcLatency speech=%s stage=listening"), *MessageId.ToString());
}

void UDeliveryNpcAgentComponent::EndListening()
{
    ListeningSpeaker = nullptr;
    ListeningMessage.Invalidate();
    GetOwner()->ForceNetUpdate();
}

bool UDeliveryNpcAgentComponent::ReceiveStreamDecision(FGuid RequestId, const FDeliveryNpcDecisionProposal& Proposal)
{
    if (!IsAuthority() || RequestId != ActiveRequest.RequestId) return false;
    FName Error;
    if (!TryAcceptProposal(Proposal, Error))
    {
        ReceiveDecision(RequestId, nullptr, Error);
        return false;
    }
    PublicState.bStreamSpeech = true;
    ReplyRequest = RequestId;
    ReplyAction = PublicState.ActionId;
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
        Npc->Presentation->BeginReply(ReplyAction, PublicState.ReplyToEventId);
    bDecisionQueued = false;
    EndListening();
    PublishState();
    UE_LOG(LogDeliveryNpcAgent, Log, TEXT("NpcLatency request=%s stage=approved"), *RequestId.ToString());
    return true;
}

void UDeliveryNpcAgentComponent::ReceiveSentence(FGuid RequestId, const FDeliveryNpcSentence& Sentence)
{
    if (!IsAuthority() || RequestId != ReplyRequest || PublicState.ActionId != ReplyAction ||
        PublicState.Status == EDeliveryNpcActionStatus::Failed || PublicState.Status == EDeliveryNpcActionStatus::Cancelled) return;
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
        Npc->Presentation->QueueSentence(ReplyAction, PublicState.ReplyToEventId, Sentence);
}

void UDeliveryNpcAgentComponent::FinishReply(FGuid RequestId, bool bSucceeded, int32 SentenceCount)
{
    if (!IsAuthority() || RequestId != ActiveRequest.RequestId) return;
    if (!ReplyRequest.IsValid()) { ReceiveDecision(RequestId, nullptr, TEXT("TransportFailure")); return; }
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        if (bSucceeded) Npc->Presentation->FinishSentences(ReplyAction, SentenceCount);
        else Npc->Presentation->CancelSpeech();
    }
    if (bSucceeded) RecordActionReply();
    const bool bContinueConversation = bDecisionQueued;
    ActiveRequest = {}; ReplyRequest.Invalidate(); ReplyAction.Invalidate();
    bDecisionPending = false; bDecisionQueued = false; bPlayerRequest = false;
    EndListening();
    OnDecisionFinished.Broadcast(bSucceeded ? NAME_None : FName(TEXT("TransportFailure")));
    // 播放上一句期间有人接话，等这句结束后继续处理，不让完成通知清掉新话
    if (bContinueConversation)
        if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->WakeDecision();
    GetOwner()->ForceNetUpdate();
}

bool UDeliveryNpcAgentComponent::CanLookAt(AActor* Target) const
{
    const auto* Pawn = Cast<APawn>(GetOwner());
    if (!Pawn || !Cast<AAIController>(Pawn->GetController()) || !IsValid(Target) || Target == GetOwner() ||
        Target->GetWorld() != GetWorld() || FVector::DistSquared(Target->GetActorLocation(), GetOwner()->GetActorLocation()) >
        FMath::Square(FMath::Clamp(MaxLookAtDistance, 100.f, 3000.f))) return false;
    // 居民视线取躯干高度，车辆根点可能位于地面，不能把地面误判成目标前的墙
    if (Cast<ADeliveryNpcCharacter>(GetOwner()))
        return DeliveryNpcConversationTrace::HasClearPath(GetOwner(),Target,FVector(0,0,40));
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(DeliveryNpcLookAt), false, GetOwner());
    return !GetWorld()->LineTraceSingleByChannel(Hit, GetOwner()->GetActorLocation(), Target->GetActorLocation(), ECC_Visibility, Params) ||
        Hit.GetActor() == Target;
}

bool UDeliveryNpcAgentComponent::TryAcceptProposal(const FDeliveryNpcDecisionProposal& Proposal, FName& Reason)
{
    PruneEvidence();
    Reason = TEXT("StaleProposal");
    const bool bResident=GetOwner()->IsA<ADeliveryNpcCharacter>();
    // 按发送时保存的信息核对回复，普通视野更新不应让已经生成的台词被丢弃
    // 其他类型的角色仍要求上下文版本完全一致；受击时通过 CancelPlan 取消当前请求
    if (Proposal.RequestId != ActiveRequest.RequestId || Proposal.WorldEpoch != ActiveRequest.WorldEpoch ||
        Proposal.NpcId != NpcId || Proposal.ContextRevision != ActiveRequest.ContextRevision ||
        (!bResident && Proposal.ContextRevision != ContextRevision) ||
        Proposal.PlanRevision != ActiveRequest.PlanRevision || HasActiveAction()) return false;
    // 玩家或同伴又对自己说了新话时，重新结合上下文回答；旁听不打断当前回答
    if (bResident && Evidence.ContainsByPredicate([this](const auto& Item)
        {
            return (Item.Kind==TEXT("PlayerSpeech") || Item.Kind==TEXT("Speech")) && !ActiveRequest.Evidence.ContainsByPredicate(
                [&Item](const auto& Sent){ return Sent.EventId==Item.EventId; });
        })) return false;
    Reason = TEXT("UnsupportedAction");
    if (!ActiveRequest.AllowedActions.Contains(Proposal.Action)) return false;
    Reason = TEXT("InvalidEvidence");
    for (const auto& Id : Proposal.EvidenceIds)
        if (!ActiveRequest.Evidence.ContainsByPredicate([&Id](const auto& Item) { return Item.EventId == Id; }) ||
            !Evidence.ContainsByPredicate([&Id](const auto& Item) { return Item.EventId == Id; })) return false;
    AActor* Target = nullptr;
    Reason = TEXT("InvalidTarget");
    if (Proposal.Action == EDeliveryNpcAction::LookAt || Proposal.Action == EDeliveryNpcAction::MoveTo ||
        Proposal.Action == EDeliveryNpcAction::StrikeOnce || Proposal.Action == EDeliveryNpcAction::HoldVehicle || Proposal.Action == EDeliveryNpcAction::MoveAside)
    {
        Target = KnownSubjects.FindRef(Proposal.TargetId).Get();
        if (!CanLookAt(Target) || !Evidence.ContainsByPredicate([&Proposal](const auto& Item)
            { return Item.SubjectId == Proposal.TargetId && Proposal.EvidenceIds.Contains(Item.EventId); })) return false;
    }
    else if (Proposal.TargetId.IsValid()) return false;
    if (auto* Resident=Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        if (!Resident->CanExecute(Proposal.Action,Target)) return false;
        const bool bOffered=ActiveRequest.Candidates.ContainsByPredicate([&](const auto& Item)
            { return Item.Action==Proposal.Action && Item.TargetId==Proposal.TargetId; });
        if (!bOffered) return false;
    }
    Reason = TEXT("InvalidDuration");
    if (!FMath::IsFinite(Proposal.Duration) || Proposal.Duration < .1f || Proposal.Duration > 5.f) return false;
    FGuid PlayerMessage;
    AActor* SpeechTarget = nullptr;
    if (bResident)
    {
        for (const auto& Item : ActiveRequest.Evidence)
            if (Item.Kind==TEXT("PlayerSpeech")) { PlayerMessage=Item.EventId; SpeechTarget=KnownSubjects.FindRef(Item.SubjectId).Get(); }
        if (ActiveRequest.Evidence.ContainsByPredicate([&PlayerMessage](const auto& Item)
            { return (Item.Kind==TEXT("SelfSpeech") || Item.Kind==TEXT("ActionReply")) && Item.ReplyToEventId==PlayerMessage; })) PlayerMessage.Invalidate();
        // 没有台词时仍要做出玩家能看见的动作，空等不能算作回应
        const bool bVisibleAction=Proposal.Action==EDeliveryNpcAction::LookAt || Proposal.Action==EDeliveryNpcAction::MoveTo ||
            Proposal.Action==EDeliveryNpcAction::StrikeOnce || Proposal.Action==EDeliveryNpcAction::HoldVehicle ||
            Proposal.Action==EDeliveryNpcAction::ReleaseVehicle || Proposal.Action==EDeliveryNpcAction::LieDown ||
            Proposal.Action==EDeliveryNpcAction::Pace || Proposal.Action==EDeliveryNpcAction::MoveAside;
        if (PlayerMessage.IsValid() && Proposal.Speech.TrimStartAndEnd().IsEmpty() && !bVisibleAction)
        { Reason=TEXT("MissingPlayerReply"); return false; }
        if (!PlayerMessage.IsValid())
        {
            SpeechTarget = nullptr;
            for (const auto& Item : ActiveRequest.Evidence)
                if (Item.Kind==TEXT("Speech"))
                { PlayerMessage=Item.EventId; SpeechTarget=KnownSubjects.FindRef(Item.SubjectId).Get(); }
            if (ActiveRequest.Evidence.ContainsByPredicate([&PlayerMessage](const auto& Item)
                { return PlayerMessage.IsValid() && (Item.Kind==TEXT("SelfSpeech") || Item.Kind==TEXT("ActionReply")) && Item.ReplyToEventId==PlayerMessage; }))
            { PlayerMessage.Invalidate(); SpeechTarget=nullptr; }
        }
        // 台词对象和动作目标可以不同，例如看着小芳，却回答正在拱火的玩家
        if (Proposal.bHasSpeechTarget)
        {
            SpeechTarget = KnownSubjects.FindRef(Proposal.SpeechTargetId).Get();
            if (Proposal.SpeechTargetId.IsValid() && (!IsValid(SpeechTarget) || SpeechTarget==GetOwner() ||
                !Cast<APawn>(SpeechTarget) || SpeechTarget->GetWorld()!=GetWorld()))
            { Reason=TEXT("StaleProposal"); return false; }
        }
    }
    PublicState = {};
    PublicState.ReplyToEventId=PlayerMessage;
    PublicState.ActionId = FGuid::NewGuid();
    PublicState.PlanRevision = Proposal.PlanRevision;
    PublicState.Action = Proposal.Action;
    PublicState.Status = EDeliveryNpcActionStatus::Accepted;
    PublicState.Target = Target;
    PublicState.SpeechTarget = SpeechTarget;
    PublicState.Duration = Proposal.Duration;
    PublicState.Speech=Proposal.Speech.Left(120); PublicState.Emotion=Proposal.Emotion;
    ActionTargetId = Proposal.TargetId;
    ActionEvidenceIds = Proposal.EvidenceIds;
    // 行为树一直没有开始执行时，动作也会超时，避免 NPC 永久卡在等待状态
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
    if (bAccepted) { bPlayerRequest = false; EndListening(); }
    OnDecisionFinished.Broadcast(Error);
    if (bRetry && !IsDecisionPending() && !HasActiveAction()) RequestDecision();
}

bool UDeliveryNpcAgentComponent::HasActiveAction() const
{
    return PublicState.Status == EDeliveryNpcActionStatus::Accepted || PublicState.Status == EDeliveryNpcActionStatus::Running;
}

bool UDeliveryNpcAgentComponent::StartAction(FDeliveryNpcPublicState& OutAction)
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
    if (auto* Resident=Cast<ADeliveryNpcCharacter>(GetOwner()))
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
    // 先记下动作已经结束，再发送通知，避免通知过程中再次结束同一动作
    PublicState.Status = Result;
    PublicState.Reason = Reason;
    // 先停止移动和尚未发生的命中，避免打断后到下一帧仍补出一拳
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->StopAction(ActionId);
    UE_LOG(LogDeliveryNpcAgent, Display, TEXT("Action npc=%s action=%s revision=%d result=%s reason=%s"),
        *NpcId.ToString(), *ActionId.ToString(), PublicState.PlanRevision,
        *StaticEnum<EDeliveryNpcActionStatus>()->GetNameStringByValue(static_cast<int64>(Result)), *Reason.ToString());
    AActor* FinishedTarget = PublicState.Target;
    PublicState.Target = nullptr;
    ActionEvidenceIds.Reset();
    ActionTargetId.Invalidate();
    FDeliveryNpcObservation ResultEvent;
    ResultEvent.EventId = FGuid::NewGuid();
    ResultEvent.Kind = TEXT("ActionFinished");
    ResultEvent.Source = EDeliveryNpcEvidenceSource::ActionResult;
    ResultEvent.Subject = GetOwner();
    ResultEvent.Addressee = IsValid(FinishedTarget)?FinishedTarget:nullptr;
    ResultEvent.Description = FString::Printf(TEXT("%s %s %s"), *DeliveryNpc::ActionName(PublicState.Action),
        *StaticEnum<EDeliveryNpcActionStatus>()->GetNameStringByValue(static_cast<int64>(Result)), *Reason.ToString());
    ResultEvent.Lifetime = 30.f;
    ObserveEvent(ResultEvent);
    RecordActionReply();
    PublishState();
    return true;
}

void UDeliveryNpcAgentComponent::RecordActionReply()
{
    if (PublicState.Status!=EDeliveryNpcActionStatus::Succeeded || !PublicState.ReplyToEventId.IsValid()) return;
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    const bool bSilent=PublicState.bStreamSpeech
        ? Npc && Npc->Presentation->IsReplyWithoutSpeech(PublicState.ActionId)
        : PublicState.Speech.TrimStartAndEnd().IsEmpty();
    if (!bSilent || Evidence.ContainsByPredicate([this](const auto& Item)
        { return Item.Kind==TEXT("ActionReply") && Item.ReplyToEventId==PublicState.ReplyToEventId; })) return;
    // 动作成功且确认没有台词后，才记录这次回应；台词记忆仍由播放完成时写入
    FDeliveryNpcObservation Reply;
    Reply.Kind=TEXT("ActionReply"); Reply.Source=EDeliveryNpcEvidenceSource::ActionResult;
    Reply.Subject=GetOwner(); Reply.Addressee=IsValid(PublicState.SpeechTarget)?PublicState.SpeechTarget.Get():nullptr;
    Reply.ReplyToEventId=PublicState.ReplyToEventId; Reply.Lifetime=60;
    Reply.Description=FString::Printf(TEXT("%s 动作已完成，这次没有台词"),*DeliveryNpc::ActionName(PublicState.Action));
    ObserveEvent(Reply);
}

void UDeliveryNpcAgentComponent::CancelPlan(FName Reason)
{
    if (!IsAuthority()) return;
    CancelDecision();
    TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Cancelled, Reason);
    EndListening();
    bPlayerRequest = false;
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->Presentation->CancelSpeech();
}

void UDeliveryNpcAgentComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!IsAuthority()) return;
    if (ListeningMessage.IsValid() && (!IsValid(ListeningSpeaker) || FPlatformTime::Seconds() > ListeningUntil || ListeningSpeaker->GetWorld() != GetWorld() ||
        FVector::DistSquared(GetOwner()->GetActorLocation(), ListeningSpeaker->GetActorLocation()) > FMath::Square(900.f)))
        CancelPlan(TEXT("SpeakerLeft"));
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
    DOREPLIFETIME(UDeliveryNpcAgentComponent,ListeningSpeaker);
}

void UDeliveryNpcAgentComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 退出时先禁止新请求，再取消剩余操作，不再通知即将销毁的表现组件
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
