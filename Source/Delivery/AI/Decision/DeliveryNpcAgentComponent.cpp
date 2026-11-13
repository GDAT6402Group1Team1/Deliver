#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Conversation/DeliveryNpcConversationTrace.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Algo/AllOf.h"
#include "AIController.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Net/UnrealNetwork.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeliveryNpcAgent, Log, All);

namespace
{
    bool IsConversationMemory(FName Kind)
    {
        return Kind==TEXT("PlayerSpeech") || Kind==TEXT("Speech") || Kind==TEXT("OverheardSpeech") ||
            Kind==TEXT("SelfSpeech") || Kind==TEXT("ActionReply");
    }

    bool IsSightMemory(const FDeliveryNpcEvidence& Item)
    {
        return Item.Source==EDeliveryNpcEvidenceSource::Seen &&
            (Item.Kind==TEXT("VisibleNow") || Item.Kind==TEXT("Perceived") ||
             Item.Kind==TEXT("LostSight") || Item.Kind==TEXT("LastSeen"));
    }
}

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
    auto IsReferenced = [this](const FDeliveryNpcEvidence& Item)
    {
        return (ActiveRequest.RequestId.IsValid() && ActiveRequest.Evidence.ContainsByPredicate(
            [&Item](const auto& Sent) { return Sent.EventId==Item.EventId; })) ||
            (HasActiveAction() && ActionEvidenceIds.Contains(Item.EventId));
    };
    // 原定时限照常生效，引用保护只防止重复观察和容量淘汰删掉正在使用的记录
    bool bChanged = Evidence.RemoveAll([Now](const auto& Item) { return Item.ExpiresAt<=Now; })>0;
    for (auto& Item : Evidence)
    {
        // 转头后仍记得刚才见过谁，但不再向模型声称对方仍在原地
        // 当前请求保留发送时的原描述，等这一轮结束后再标为历史观察
        if (IsSightMemory(Item) && Item.Kind!=TEXT("LostSight") && Item.Kind!=TEXT("LastSeen") &&
            Now-Item.ObservedAt>=45.0 && !IsReferenced(Item))
        {
            Item.Kind=TEXT("LastSeen");
            Item.Description=TEXT("此前见过这个对象；记录的位置是当时所见，当前位置和行为未知");
            bChanged=true;
        }
    }
    TSet<FGuid> SeenSubjects;
    for (int32 Index=Evidence.Num()-1; Index>=0; --Index)
    {
        const auto& Item=Evidence[Index];
        if (!IsSightMemory(Item) || !Item.SubjectId.IsValid()) continue;
        // 同一个人的定时视野更新只留最新一条，正在被请求或动作引用的编号暂时保留
        if (SeenSubjects.Contains(Item.SubjectId) && !IsReferenced(Item))
        { Evidence.RemoveAt(Index); bChanged=true; }
        else SeenSubjects.Add(Item.SubjectId);
    }
    while (Evidence.Num()>32)
    {
        int32 RemoveIndex=INDEX_NONE;
        int32 LowestPriority=3;
        // 保留刚收到的新事件，避免满额时把玩家刚说的话立即删掉
        for (int32 Index=0; Index<Evidence.Num()-1; ++Index)
        {
            const auto& Item=Evidence[Index];
            if (IsReferenced(Item)) continue;
            const int32 Priority=IsSightMemory(Item)?0:IsConversationMemory(Item.Kind)?2:1;
            if (Priority<LowestPriority) { RemoveIndex=Index; LowestPriority=Priority; }
        }
        // 极端情况下全部记录都被引用，仍沿用原来的容量上限，避免记忆无限增长
        Evidence.RemoveAt(RemoveIndex==INDEX_NONE?0:RemoveIndex);
        bChanged=true;
    }
    if (bChanged) ++ContextRevision;
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
    // 刚说过的话和见过的人记住五分钟，碰撞、受击等现场事件仍使用各自的短时限
    const float Lifetime=IsConversationMemory(Item.Kind) || IsSightMemory(Item)?300.f:
        FMath::Clamp(Observation.Lifetime, .1f, 60.f);
    Item.ExpiresAt = Item.ObservedAt + Lifetime;
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
    PruneEvidence();
    RecentEventIds.Add(EventId);
    if (RecentEventIds.Num() > 128) RecentEventIds.RemoveAt(0);
    ++ContextRevision;
    return true;
}

bool UDeliveryNpcAgentComponent::BuildDecisionRequest(FGuid WorldEpoch, FDeliveryNpcDecisionRequest& OutRequest)
{
    if (!IsAuthority() || !WorldEpoch.IsValid() || HasActiveAction()) return false;
    if (const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        // 每次请求只保存一份自己的当前战况，不把对方的私有数值发给模型
        Evidence.RemoveAll([](const auto& Item){ return Item.Kind==TEXT("SelfCombat"); });
        FDeliveryNpcObservation Body;
        Body.Kind=TEXT("SelfCombat"); Body.Subject=GetOwner(); Body.Source=EDeliveryNpcEvidenceSource::ActionResult;
        Body.Description=Npc->Combat->DescribeCombatState(); Body.Lifetime=30;
        ObserveEvent(Body);
    }
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
    const auto* Settings = GetDefault<UDeliveryAiSettings>();
    OutRequest.bStreamSentenceAudio = Settings->bStreamReplies && Settings->bStreamSentenceAudio;
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
            for (auto Action : {EDeliveryNpcAction::LookAt,EDeliveryNpcAction::MoveTo,EDeliveryNpcAction::StrikeOnce,EDeliveryNpcAction::HoldVehicle,EDeliveryNpcAction::MoveAside,EDeliveryNpcAction::Fight,EDeliveryNpcAction::Flee})
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
    PlayerReplyMessage = MessageId;
    const auto* PlayerPawn=Cast<APawn>(Speaker);
    PlayerReplyController=PlayerPawn?Cast<APlayerController>(PlayerPawn->GetController()):nullptr;
    ListeningStartedAt = ServerTime();
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
    if (!CanReceiveReply(RequestId)) return;
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
        Npc->Presentation->QueueSentence(ReplyAction, PublicState.ReplyToEventId, Sentence);
}

bool UDeliveryNpcAgentComponent::CanReceiveReply(FGuid RequestId) const
{
    return IsAuthority() && RequestId.IsValid() && RequestId == ReplyRequest && ReplyAction.IsValid() &&
        PublicState.ActionId == ReplyAction && (PublicState.Status == EDeliveryNpcActionStatus::Accepted ||
        PublicState.Status == EDeliveryNpcActionStatus::Running || PublicState.Status == EDeliveryNpcActionStatus::Succeeded);
}

bool UDeliveryNpcAgentComponent::ReceiveSpeechBegin(FGuid RequestId, const FDeliveryNpcSpeechBegin& Begin)
{
    if (!CanReceiveReply(RequestId)) return false;
    auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    return Npc && Npc->Presentation->BeginSentenceAudio(ReplyAction, PublicState.ReplyToEventId, Begin);
}

bool UDeliveryNpcAgentComponent::ReceiveAudioChunk(FGuid RequestId, const FDeliveryNpcAudioChunk& Chunk)
{
    if (!CanReceiveReply(RequestId)) return false;
    auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    return Npc && Npc->Presentation->AppendSentenceAudio(ReplyAction, PublicState.ReplyToEventId, Chunk);
}

bool UDeliveryNpcAgentComponent::ReceiveAudioEnd(FGuid RequestId, const FDeliveryNpcAudioEnd& End)
{
    if (!CanReceiveReply(RequestId)) return false;
    auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    return Npc && Npc->Presentation->EndSentenceAudio(ReplyAction, PublicState.ReplyToEventId, End);
}

void UDeliveryNpcAgentComponent::FinishReply(FGuid RequestId, bool bSucceeded, int32 SentenceCount)
{
    if (!IsAuthority() || !RequestId.IsValid() || RequestId != ActiveRequest.RequestId) return;
    if (!ReplyRequest.IsValid()) { ReceiveDecision(RequestId, nullptr, TEXT("TransportFailure")); return; }
    if (ReplyRequest!=RequestId || ReplyAction!=PublicState.ActionId) return;
    if (bSucceeded && (SentenceCount<0 || SentenceCount>3)) bSucceeded=false;
    // 动作失败后，表现组件可能已经停止对白；是否零台词仍以本次后台结束通知为准
    const bool bSilent=bSucceeded && SentenceCount==0 && PublicState.bStreamSpeech;
    FGuid FailedMessage;
    AActor* FailedSpeaker=nullptr;
    if (!bSucceeded) GetRequestedPlayerMessage(FailedMessage,FailedSpeaker);
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        if (bSucceeded) Npc->Presentation->FinishSentences(ReplyAction, SentenceCount);
        else Npc->Presentation->CancelSpeech();
    }
    if (bSucceeded) RecordActionReply();
    const bool bContinueConversation = bDecisionQueued;
    ActiveRequest = {}; ReplyRequest.Invalidate(); ReplyAction.Invalidate();
    bDecisionPending = false; bDecisionQueued = false;
    if (bSucceeded)
    {
        // 动作先失败、后端随后确认没有台词时，也结束这次界面等待
        if (bSilent && DeliveryNpc::IsTerminal(PublicState.Status) && PublicState.Status!=EDeliveryNpcActionStatus::Succeeded)
            FinishFailedPlayerReply(PublicState.ReplyToEventId,nullptr,false);
        bPlayerRequest = false;
        // 没有台词时等动作真正完成，再通知原来发言的玩家
        const bool bAwaitingAction=HasActiveAction() && PlayerReplyMessage.IsValid() &&
            PlayerReplyMessage==PublicState.ReplyToEventId && bSilent;
        if (!bAwaitingAction) { PlayerReplyMessage.Invalidate(); PlayerReplyController.Reset(); }
        EndListening();
    }
    OnDecisionFinished.Broadcast(bSucceeded ? NAME_None : FName(TEXT("TransportFailure")));
    // 播放上一句期间有人接话，等这句结束后继续处理，不让完成通知清掉新话
    if (bContinueConversation && bSucceeded)
        if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->WakeDecision();
    if (!bSucceeded)
    {
        const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
        FinishFailedPlayerReply(FailedMessage,FailedSpeaker,Npc && Npc->IsDecisionRetryScheduled());
    }
    GetOwner()->ForceNetUpdate();
}

bool UDeliveryNpcAgentComponent::IsRespondingToAttackFrom(const AActor* Attacker) const
{
    if (!bDecisionPending || !IsValid(Attacker)) return false;
    return ActiveRequest.Evidence.ContainsByPredicate([this,Attacker](const auto& Item)
    {
        return (Item.Kind==TEXT("FightThreat") || Item.Kind==TEXT("LightMeleeHit") || Item.Kind==TEXT("Hit")) &&
            KnownSubjects.FindRef(Item.SubjectId).Get()==Attacker;
    });
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
    Reason = TEXT("InvalidDuration");
    if ((Proposal.Action==EDeliveryNpcAction::Fight || Proposal.Action==EDeliveryNpcAction::Flee) &&
        (!FMath::IsFinite(Proposal.Duration) || Proposal.Duration<UDeliveryNpcCombatComponent::StrikeDuration || Proposal.Duration>5.f)) return false;
    Reason = TEXT("InvalidEvidence");
    for (const auto& Id : Proposal.EvidenceIds)
        if (!ActiveRequest.Evidence.ContainsByPredicate([&Id](const auto& Item) { return Item.EventId == Id; }) ||
            !Evidence.ContainsByPredicate([&Id](const auto& Item) { return Item.EventId == Id; })) return false;
    AActor* Target = nullptr;
    Reason = TEXT("InvalidTarget");
    if (Proposal.Action == EDeliveryNpcAction::LookAt || Proposal.Action == EDeliveryNpcAction::MoveTo ||
        Proposal.Action == EDeliveryNpcAction::StrikeOnce || Proposal.Action == EDeliveryNpcAction::Fight || Proposal.Action == EDeliveryNpcAction::Flee || Proposal.Action == EDeliveryNpcAction::HoldVehicle || Proposal.Action == EDeliveryNpcAction::MoveAside)
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
            Proposal.Action==EDeliveryNpcAction::StrikeOnce || Proposal.Action==EDeliveryNpcAction::Fight || Proposal.Action==EDeliveryNpcAction::HoldVehicle ||
            Proposal.Action==EDeliveryNpcAction::ReleaseVehicle || Proposal.Action==EDeliveryNpcAction::LieDown ||
            Proposal.Action==EDeliveryNpcAction::Pace || Proposal.Action==EDeliveryNpcAction::MoveAside || Proposal.Action==EDeliveryNpcAction::Flee;
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
    FGuid FailedMessage;
    AActor* FailedSpeaker=nullptr;
    if (!bAccepted) GetRequestedPlayerMessage(FailedMessage,FailedSpeaker);
    ActiveRequest = {};
    bDecisionPending=false;
    bDecisionQueued = false;
    UE_LOG(LogDeliveryNpcAgent, Display, TEXT("Decision npc=%s request=%s accepted=%d reason=%s"),
        *NpcId.ToString(), *RequestId.ToString(), bAccepted, *Error.ToString());
    if (bAccepted) PublishState();
    if (bAccepted)
    {
        bPlayerRequest = false;
        const bool bAwaitingAction=HasActiveAction() && PlayerReplyMessage.IsValid() &&
            PlayerReplyMessage==PublicState.ReplyToEventId && PublicState.Speech.TrimStartAndEnd().IsEmpty();
        if (!bAwaitingAction) { PlayerReplyMessage.Invalidate(); PlayerReplyController.Reset(); }
        EndListening();
    }
    OnDecisionFinished.Broadcast(Error);
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    const bool bRetryAllowed=!FailedMessage.IsValid() || FailedMessage!=PlayerReplyMessage || (Npc && Npc->IsDecisionRetryScheduled());
    const bool bRetryStarted=bRetry && bRetryAllowed && !IsDecisionPending() && !HasActiveAction() && RequestDecision();
    if (!bAccepted)
    {
        FinishFailedPlayerReply(FailedMessage,FailedSpeaker,bRetryStarted || (Npc && Npc->IsDecisionRetryScheduled()));
    }
    GetOwner()->ForceNetUpdate();
}

void UDeliveryNpcAgentComponent::GetRequestedPlayerMessage(FGuid& OutMessage, AActor*& OutSpeaker) const
{
    OutMessage.Invalidate();
    OutSpeaker=nullptr;
    if (!bPlayerRequest || !ActiveRequest.bPlayerRequest || !PlayerReplyMessage.IsValid()) return;
    const auto* Message=ActiveRequest.Evidence.FindByPredicate([this](const auto& Item)
        { return Item.Kind==TEXT("PlayerSpeech") && Item.EventId==PlayerReplyMessage; });
    if (!Message) return;
    OutMessage=Message->EventId;
    OutSpeaker=KnownSubjects.FindRef(Message->SubjectId).Get();
}

void UDeliveryNpcAgentComponent::FinishFailedPlayerReply(FGuid MessageId, AActor* Speaker, bool bWillRetry)
{
    // 失败通知只处理原来那句玩家发言，新的插话和新请求继续按自己的状态执行
    if (!MessageId.IsValid() || PlayerReplyMessage!=MessageId || ActiveRequest.RequestId.IsValid() ||
        (ListeningMessage.IsValid() && ListeningMessage!=MessageId)) return;
    if (bWillRetry)
    {
        // 模型已选动作后若连接中断，重试期间继续看着原来的玩家，不重播短呼声
        if (!IsListening() && IsValid(Speaker) && Speaker->GetWorld()==GetWorld())
        {
            ListeningSpeaker=Speaker;
            ListeningMessage=MessageId;
            GetOwner()->ForceNetUpdate();
        }
        return;
    }
    bPlayerRequest=false;
    PlayerReplyMessage.Invalidate();
    auto* Controller=PlayerReplyController.Get();
    PlayerReplyController.Reset();
    EndListening();
    // 玩家换 Pawn 后仍通知原来的控制器，不把消息发给后来占有该 Pawn 的人
    if (auto* Conversation=Controller && Controller->GetWorld()==GetWorld()?Controller->FindComponentByClass<UDeliveryConversationComponent>():nullptr)
        Conversation->ClientResult(TEXT("ReplyFailed"),MessageId);
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
    const auto* FightingNpc=Cast<ADeliveryNpcCharacter>(GetOwner());
    const FString FightDescription=PublicState.Action==EDeliveryNpcAction::Fight && FightingNpc
        ?FightingNpc->Combat->DescribeFight(ActionId,Reason):FString();
    FString FleeDescription;
    if (PublicState.Action==EDeliveryNpcAction::Flee)
    {
        const TCHAR* Stop=TEXT("这次逃跑动作已停止");
        if (Reason==TEXT("FleeReachedDistance")) Stop=TEXT("已经与对方拉开距离，停止这次逃跑");
        else if (Reason==TEXT("FleeTimeLimit") || Reason==TEXT("FleeTravelLimit")) Stop=TEXT("这次短程逃跑结束");
        else if (Reason==TEXT("FleePathUnavailable")) Stop=TEXT("前面的路走不通，我停下了");
        else if (Reason==TEXT("ReconsiderAfterHit")) Stop=TEXT("逃跑时挨了一拳，我停下重新判断");
        else if (Reason==TEXT("FleeTargetLost")) Stop=TEXT("原先躲避的对象已经不在当前场景里");
        FleeDescription=Stop;
        if (IsValid(PublicState.Target))
            FleeDescription+=FString::Printf(TEXT("，当前与对方相距约 %.0f 厘米"),FVector::Dist2D(GetOwner()->GetActorLocation(),PublicState.Target->GetActorLocation()));
        FleeDescription+=TEXT("。这不代表对方已经放弃追赶。");
    }
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
    if (!FightDescription.IsEmpty()) ResultEvent.Description=FightDescription;
    else if (!FleeDescription.IsEmpty()) ResultEvent.Description=FleeDescription;
    ResultEvent.Lifetime = 30.f;
    ObserveEvent(ResultEvent);
    RecordActionReply();
    // 回复已结束时处理动作回执；等待重试的玩家仍要收到下一次请求的结果
    if (!bDecisionPending && !bPlayerRequest && PlayerReplyMessage==PublicState.ReplyToEventId)
    {
        const bool bSilent=PublicState.bStreamSpeech
            ? FightingNpc && FightingNpc->Presentation->IsReplyWithoutSpeech(ActionId)
            : PublicState.Speech.TrimStartAndEnd().IsEmpty();
        if (Result!=EDeliveryNpcActionStatus::Succeeded && bSilent)
            FinishFailedPlayerReply(PublicState.ReplyToEventId,nullptr,false);
        PlayerReplyMessage.Invalidate(); PlayerReplyController.Reset();
    }
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
    if (PlayerReplyMessage!=Reply.ReplyToEventId) return;
    auto* Controller=PlayerReplyController.Get();
    PlayerReplyMessage.Invalidate();
    PlayerReplyController.Reset();
    // 控制器换过 Pawn 也只通知原玩家，界面继续按发言编号过滤旧回执
    if (auto* Conversation=Controller && Controller->GetWorld()==GetWorld()?Controller->FindComponentByClass<UDeliveryConversationComponent>():nullptr)
        Conversation->ClientResult(TEXT("ActionReply"),Reply.ReplyToEventId);
}

void UDeliveryNpcAgentComponent::CancelPlan(FName Reason)
{
    if (!IsAuthority()) return;
    CancelDecision();
    TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Cancelled, Reason);
    EndListening();
    bPlayerRequest = false;
    PlayerReplyMessage.Invalidate();
    PlayerReplyController.Reset();
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
    DOREPLIFETIME(UDeliveryNpcAgentComponent,ListeningStartedAt);
}

void UDeliveryNpcAgentComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 退出时先禁止新请求，再取消剩余操作，不再通知即将销毁的表现组件
    bEndingPlay = true;
    ListeningSpeaker=nullptr;
    ListeningMessage.Invalidate();
    PlayerReplyMessage.Invalidate();
    PlayerReplyController.Reset();
    bPlayerRequest=false;
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
