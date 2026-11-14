#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Conversation/DeliveryConversationTurnSubsystem.h"
#include "AI/Conversation/DeliveryNpcConversationTrace.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
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
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Net/UnrealNetwork.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeliveryNpcAgent, Log, All);

namespace
{
    FString TraceText(const FString& Text, int32 Limit)
    {
        FString Result=Text.Left(Limit);
        for (TCHAR& Character:Result)
            if (FChar::IsControl(Character)) Character=TEXT(' ');
        return Result.TrimStartAndEnd();
    }

    FString TraceSubject(const FDeliveryNpcDecisionRequest& Request, FGuid Id)
    {
        const auto* Subject=Request.Subjects.FindByPredicate([Id](const auto& Item){ return Item.SubjectId==Id; });
        return Subject?TraceText(Subject->Name,64):FString();
    }

    bool IsConversationMemory(FName Kind)
    {
        return Kind==TEXT("NpcMessage") || Kind==TEXT("NpcMessageSent") ||
            Kind==TEXT("PlayerSpeech") || Kind==TEXT("Speech") || Kind==TEXT("OverheardSpeech") ||
            Kind==TEXT("SelfSpeech") || Kind==TEXT("ActionReply");
    }

    bool IsSightMemory(const FDeliveryNpcEvidence& Item)
    {
        return Item.Source==EDeliveryNpcEvidenceSource::Seen &&
            (Item.Kind==TEXT("VisibleNow") || Item.Kind==TEXT("Perceived") ||
             Item.Kind==TEXT("LostSight") || Item.Kind==TEXT("LastSeen"));
    }

    bool PrepareCandidateIdentities(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester)
    {
        if (Request.RequestedCapabilityVersion<1 || Request.RequestedCapabilityVersion>FDeliveryNpcDecisionRequest::CapabilityVersion) return false;
        if (Request.RequestedCapabilityVersion<5) return true;
        TSet<FGuid> CandidateIds;
        for (auto& Candidate:Request.Candidates)
        {
            const bool bHomeAction=Candidate.Action==EDeliveryNpcAction::HomeAction;
            if (bHomeAction!=Candidate.HomeChoice.IsValid()) return false;
            if (bHomeAction)
            {
                if (!Request.bPlayerRequest || !IsValid(Requester) || Candidate.TargetId.IsValid()) return false;
                const FGuid ChoiceId=Candidate.HomeChoice->CandidateId;
                if (ChoiceId.IsValid())
                {
                    if (Candidate.CandidateId.IsValid() && Candidate.CandidateId!=ChoiceId) return false;
                    Candidate.CandidateId=ChoiceId;
                }
            }
            if (!Candidate.CandidateId.IsValid())
            {
                do { Candidate.CandidateId=FGuid::NewGuid(); } while (CandidateIds.Contains(Candidate.CandidateId));
            }
            if (CandidateIds.Contains(Candidate.CandidateId)) return false;
            CandidateIds.Add(Candidate.CandidateId);
            if (bHomeAction)
            {
                // 按值复制再绑定本次请求，不能改到组件缓存或另一次请求的共享快照
                Candidate.HomeChoice=MakeShared<FDeliveryCompanionHomeChoice>(*Candidate.HomeChoice);
                Candidate.HomeChoice->CandidateId=Candidate.CandidateId;
                Candidate.HomeChoice->RequestId=Request.RequestId;
                Candidate.HomeChoice->Requester=Requester;
            }
        }
        return !Request.Candidates.IsEmpty();
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
    if (IsAuthority()) { NpcId = FGuid::NewGuid(); BindNpcMessageTurns(); }
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

void UDeliveryNpcAgentComponent::BeginDecisionTrace(const FDeliveryNpcDecisionRequest& Request)
{
    if (!IsAuthority() || Request.NpcId!=NpcId || !Request.RequestId.IsValid()) return;
    const auto* Gateway=GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>();
    if (!Gateway || Gateway->GetWorldEpoch()!=Request.WorldEpoch) return;
    const int32 PreviousRevision=DecisionTrace.Revision;
    DecisionTrace={};
    DecisionTrace.Revision=PreviousRevision;
    DecisionTrace.WorldEpoch=Request.WorldEpoch;
    DecisionTrace.RequestId=Request.RequestId;
    DecisionTrace.NpcName=TraceText(Request.SelfName,64);
    DecisionTrace.Stage=TEXT("Queued");
    TraceQueuedAt=FPlatformTime::Seconds();
    TraceDispatchedAt=-1;
    DecisionTrace.InputSummary=FString::Printf(TEXT("%s；游戏请求记录含 %d 条感知、%d 个已知对象、%d 个动作候选"),
        Request.bPlayerRequest?TEXT("玩家发起交谈"):TEXT("环境事件触发"),Request.Evidence.Num(),Request.Subjects.Num(),Request.Candidates.Num());
    const auto* PlayerMessage=Request.Evidence.FindByPredicate([&Request](const auto& Item)
        { return Request.bPlayerRequest && Item.Kind==TEXT("PlayerSpeech") && Item.EventId==Request.PlayerMessageId; });
    if (PlayerMessage) DecisionTrace.PlayerText=TraceText(PlayerMessage->Description,256);
    for (int32 Index=Request.Evidence.Num()-1;Index>=0 && DecisionTrace.Observations.Num()<5;--Index)
    {
        const auto& Item=Request.Evidence[Index];
        const TCHAR* Source=Item.Source==EDeliveryNpcEvidenceSource::Seen?TEXT("看到"):
            Item.Source==EDeliveryNpcEvidenceSource::Heard?TEXT("听到"):
            Item.Source==EDeliveryNpcEvidenceSource::ToldBy?TEXT("被告知"):TEXT("实际结果");
        FString Details;
        if (Item.bHasObservedLocation && FMath::IsFinite(Item.ObservedLocation.X) &&
            FMath::IsFinite(Item.ObservedLocation.Y) && FMath::IsFinite(Item.ObservedLocation.Z))
            Details=FString::Printf(TEXT("；位置 (%.0f, %.0f, %.0f) cm"),
                Item.ObservedLocation.X,Item.ObservedLocation.Y,Item.ObservedLocation.Z);
        if (FMath::IsFinite(Item.ObservedAt)) Details+=FString::Printf(TEXT("；观察时刻 %.1f 秒（本局时间）"),Item.ObservedAt);
        if (FMath::IsFinite(Item.ObservedAt) && FMath::IsFinite(Item.ExpiresAt) && Item.ExpiresAt>=Item.ObservedAt)
            Details+=FString::Printf(TEXT("；时效 %.1f 秒"),Item.ExpiresAt-Item.ObservedAt);
        DecisionTrace.Observations.Add(TraceText(FString::Printf(TEXT("%s：%s%s"),Source,
            *TraceText(Item.Description,FMath::Max(0,156-Details.Len())),*Details),160));
    }
    for (const auto& Item:Request.Subjects)
    {
        if (DecisionTrace.Subjects.Num()>=6) break;
        DecisionTrace.Subjects.Add(TraceText(Item.Name+TEXT(" · ")+Item.Kind,160));
    }
    for (const auto& Item:Request.Candidates)
    {
        if (DecisionTrace.Candidates.Num()>=12) break;
        const FString Target=TraceSubject(Request,Item.TargetId);
        // 时长来自 EncodeRequest 同一个候选字段，未列出的动作不编造拒绝原因
        DecisionTrace.Candidates.Add(TraceText(FString::Printf(TEXT("%s%s；时长上限 %.1f 秒；引用 %d 条证据"),
            *DeliveryNpc::ActionName(Item.Action),Target.IsEmpty()?TEXT(""):*(TEXT(" → ")+Target),
            DeliveryNpc::CandidateDuration(Item.Action),Item.EvidenceIds.Num()),160));
    }
    if (Request.bHasCompanionContext)
    {
        if (!Request.CompanionRelationshipStage.IsEmpty()) DecisionTrace.Memory.Add(TraceText(TEXT("关系：")+Request.CompanionRelationshipStage,160));
        if (!Request.CompanionMode.IsEmpty()) DecisionTrace.Memory.Add(TraceText(TEXT("持续安排：")+Request.CompanionMode,160));
        if (!Request.CompanionTaskTitle.IsEmpty()) DecisionTrace.Memory.Add(TraceText(TEXT("当前任务：")+Request.CompanionTaskTitle,160));
        // 只显示本轮实际使用的经历数量，不把个人经历全文同步给旁观玩家
        DecisionTrace.Memory.Add(FString::Printf(TEXT("本轮带入 %d 条当局经历"),Request.CompanionMemories.Num()));
    }
    if (Request.RunningActionId.IsValid())
    {
        DecisionTrace.CurrentActivity=TraceText(DeliveryNpc::ActionName(Request.RunningAction),96);
        const FString Target=TraceSubject(Request,Request.RunningTargetId);
        if (!Target.IsEmpty()) DecisionTrace.CurrentActivity=TraceText(DecisionTrace.CurrentActivity+TEXT(" → ")+Target,160);
    }
    else if (Request.bRunningCompanion)
    {
        DecisionTrace.CurrentActivity=TraceText(Request.CompanionMode,64);
        if (!Request.CompanionErrandAction.IsEmpty())
            DecisionTrace.CurrentActivity+=TEXT(" · ")+TraceText(Request.CompanionErrandAction,64);
        if (!Request.CompanionErrandPhase.IsEmpty())
            DecisionTrace.CurrentActivity+=TEXT(" · ")+TraceText(Request.CompanionErrandPhase,32);
        DecisionTrace.CurrentActivity=TraceText(DecisionTrace.CurrentActivity,160);
    }
    else DecisionTrace.CurrentActivity=TEXT("当前没有执行中的动作");
    PublishDecisionTrace();
}

bool UDeliveryNpcAgentComponent::IsDecisionTraceCurrent(FGuid WorldEpoch, FGuid RequestId) const
{
    if (!IsAuthority() || !RequestId.IsValid() || DecisionTrace.RequestId!=RequestId || DecisionTrace.WorldEpoch!=WorldEpoch) return false;
    const auto* Gateway=GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>();
    return Gateway && Gateway->GetWorldEpoch()==WorldEpoch;
}

void UDeliveryNpcAgentComponent::PublishDecisionTrace()
{
    if (!IsAuthority() || !DecisionTrace.RequestId.IsValid()) return;
    DecisionTrace.Revision=DecisionTrace.Revision==MAX_int32?1:DecisionTrace.Revision+1;
    // 订阅期间即使开始新请求，本次通知仍是一份完整快照
    const FDeliveryNpcDecisionTrace Snapshot=DecisionTrace;
    OnDecisionTraceChanged.Broadcast(Snapshot);
}

void UDeliveryNpcAgentComponent::TraceRequestEvent(FGuid WorldEpoch, FGuid RequestId, FName Stage, FName Error)
{
    if (!IsDecisionTraceCurrent(WorldEpoch,RequestId)) return;
    if (Stage==TEXT("Requesting") && TraceDispatchedAt<0) TraceDispatchedAt=FPlatformTime::Seconds();
    if ((Stage==TEXT("ReplyDone") || Stage==TEXT("Failed") || Stage==TEXT("Cancelled")) &&
        TraceQueuedAt>=0 && DecisionTrace.TotalElapsedMs<0)
        DecisionTrace.TotalElapsedMs=FMath::Max(0.0,(FPlatformTime::Seconds()-TraceQueuedAt)*1000.0);
    DecisionTrace.Stage=Stage;
    if (!Error.IsNone() && DecisionTrace.RejectionReason.IsEmpty()) DecisionTrace.RejectionReason=TraceText(Error.ToString(),96);
    if (Stage==TEXT("ReplyDone")) DecisionTrace.ReplyStage=TEXT("TransferDone");
    PublishDecisionTrace();
}

void UDeliveryNpcAgentComponent::TraceDiagnostics(FGuid WorldEpoch, FGuid RequestId, const FDeliveryNpcDecisionTrace& Diagnostics)
{
    if (!IsDecisionTraceCurrent(WorldEpoch,RequestId)) return;
    // 后端可能压缩输入，游戏请求中的位置和记录数量不代表最终模型实际读取的内容
    if (!Diagnostics.InputSummary.IsEmpty())
        DecisionTrace.InputSummary=TraceText(DecisionTrace.InputSummary+TEXT("；")+Diagnostics.InputSummary,320);
    DecisionTrace.ActualModelName=Diagnostics.ActualModelName;
    DecisionTrace.ModelProvider=Diagnostics.ModelProvider;
    DecisionTrace.RequestSource=Diagnostics.RequestSource;
    DecisionTrace.Route=Diagnostics.Route;
    DecisionTrace.FallbackReason=Diagnostics.FallbackReason;
    DecisionTrace.BackendQueueMs=Diagnostics.BackendQueueMs;
    DecisionTrace.BackendDecisionMs=Diagnostics.BackendDecisionMs;
    DecisionTrace.BackendElapsedMs=Diagnostics.BackendElapsedMs;
    DecisionTrace.FirstSentenceMs=Diagnostics.FirstSentenceMs;
    DecisionTrace.InputTokens=Diagnostics.InputTokens;
    DecisionTrace.CachedInputTokens=Diagnostics.CachedInputTokens;
    DecisionTrace.OutputTokens=Diagnostics.OutputTokens;
    // 紧接着由 ModelDone 事件统一发布，避免同一回包重复通知
}

void UDeliveryNpcAgentComponent::TraceProposal(const FDeliveryNpcDecisionProposal& Proposal)
{
    if (!IsDecisionTraceCurrent(ActiveRequest.WorldEpoch,ActiveRequest.RequestId)) return;
    DecisionTrace.ProposedAction=TraceText(DeliveryNpc::ActionName(Proposal.Action),64);
    DecisionTrace.ProposedTarget=TraceSubject(ActiveRequest,Proposal.TargetId);
    DecisionTrace.ReplyPreview=TraceText(Proposal.Speech,256);
    if (!DecisionTrace.ReplyPreview.IsEmpty())
    {
        DecisionTrace.FirstReplyText=TraceText(Proposal.Speech,120);
        DecisionTrace.ReplyStage=TEXT("Received");
    }
    if (TraceDispatchedAt>=0 && DecisionTrace.DecisionElapsedMs<0)
        DecisionTrace.DecisionElapsedMs=FMath::Max(0.0,(FPlatformTime::Seconds()-TraceDispatchedAt)*1000.0);
    // 校验完成后一起发布，建议与执行许可始终分别保存
}

void UDeliveryNpcAgentComponent::TraceValidation(FGuid WorldEpoch, FGuid RequestId, bool bAccepted, FName Error)
{
    if (!IsDecisionTraceCurrent(WorldEpoch,RequestId)) return;
    if (bAccepted)
    {
        DecisionTrace.ActionId=PublicState.ActionId;
        const bool bContinued=DecisionTrace.ProposedAction==TEXT("ContinueCurrent");
        DecisionTrace.ValidatedAction=bContinued?TEXT("ContinueCurrent / 继续当前活动"):
            TraceText(DeliveryNpc::ActionName(PublicState.Action),64);
        DecisionTrace.ExecutionResult=bContinued?TEXT("保留当前活动，仅更新本次回复"):
            PublicState.Status==EDeliveryNpcActionStatus::Running?TEXT("执行中"):
            PublicState.Status==EDeliveryNpcActionStatus::Succeeded?TEXT("动作已完成"):TEXT("校验通过，等待执行");
        DecisionTrace.Stage=TEXT("Decision");
    }
    else
    {
        DecisionTrace.RejectionReason=TraceText(Error.IsNone()?TEXT("RejectedProposal"):Error.ToString(),96);
        if (!DecisionTrace.ProposedAction.IsEmpty()) DecisionTrace.ExecutionResult=TEXT("未执行，本次建议未通过校验");
        DecisionTrace.Stage=TEXT("Failed");
    }
    PublishDecisionTrace();
}

void UDeliveryNpcAgentComponent::TraceReplyText(FGuid RequestId, const FString& Text)
{
    if (!IsDecisionTraceCurrent(DecisionTrace.WorldEpoch,RequestId) || Text.IsEmpty()) return;
    const FString Preview=TraceText(Text,120);
    if (Preview.IsEmpty()) return;
    if (DecisionTrace.FirstReplyText.IsEmpty()) DecisionTrace.FirstReplyText=Preview;
    if (!DecisionTrace.ReplyPreview.Contains(Preview))
        DecisionTrace.ReplyPreview=TraceText(DecisionTrace.ReplyPreview.IsEmpty()?Preview:DecisionTrace.ReplyPreview+TEXT(" ")+Preview,256);
    DecisionTrace.ReplyStage=TEXT("Received");
    PublishDecisionTrace();
}

void UDeliveryNpcAgentComponent::TraceAction(FGuid ActionId, FName Stage)
{
    if (!ActionId.IsValid() || DecisionTrace.ActionId!=ActionId ||
        !IsDecisionTraceCurrent(DecisionTrace.WorldEpoch,DecisionTrace.RequestId)) return;
    DecisionTrace.Stage=Stage;
    const auto Status=PublicState.Status;
    DecisionTrace.ExecutionResult=Status==EDeliveryNpcActionStatus::Running?TEXT("执行中"):
        Status==EDeliveryNpcActionStatus::Succeeded?TEXT("动作已完成"):
        Status==EDeliveryNpcActionStatus::Cancelled?TEXT("动作已取消"):TEXT("动作失败");
    if (!PublicState.Reason.IsNone()) DecisionTrace.RejectionReason=TraceText(PublicState.Reason.ToString(),96);
    DecisionTrace.CurrentActivity=TraceText(DeliveryNpc::ActionName(PublicState.Action)+TEXT(" · ")+DecisionTrace.ExecutionResult,160);
    PublishDecisionTrace();
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
    Item.bNeedsReply = Observation.bNeedsReply;
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
    if (!IsAuthority() || bChangingPlan || !WorldEpoch.IsValid() || (HasActiveAction() && !CanUpdateRunningAction() && !bPlayerRequest)) return false;
    if (!bPlayerRequest && IsPlayerInputBeingRecorded()) return false;
    if (!IsAuthority() || bChangingPlan) return false;
    if (const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        // 每次请求只保存一份自己的当前战况，不把对方的私有数值发给模型
        Evidence.RemoveAll([](const auto& Item){ return Item.Kind==TEXT("SelfCombat"); });
        FDeliveryNpcObservation Body;
        Body.Kind=TEXT("SelfCombat"); Body.Subject=GetOwner(); Body.Source=EDeliveryNpcEvidenceSource::ActionResult;
        Body.Description=Npc->Combat->DescribeCombatState(); Body.Lifetime=30;
        if (CanUpdateRunningAction())
        {
            const auto* Other=Cast<ADeliveryNpcCharacter>(PublicState.Target);
            const FString TargetName=Other?Other->DisplayName.ToString():TEXT("对方");
            Body.Description+=FString::Printf(TEXT("；我正在对%s执行%s，剩余约%.1f秒。当前动作还未结束；下一步可以继续、逃跑或停手"),
                *TargetName,*DeliveryNpc::ActionName(PublicState.Action),GetActionTimeRemaining());
        }
        ObserveEvent(Body);
    }
    PruneEvidence();
    if (!NpcId.IsValid()) NpcId = FGuid::NewGuid();
    OutRequest = {};
    OutRequest.RunningActionId=HasActiveAction()?PublicState.ActionId:FGuid();
    if (OutRequest.RunningActionId.IsValid())
    {
        OutRequest.RunningAction=PublicState.Action;
        OutRequest.RunningTargetId=ActionTargetId;
        OutRequest.RunningSecondsRemaining=GetActionTimeRemaining();
        OutRequest.RunningPlanRevision=PublicState.PlanRevision;
        OutRequest.RunningTarget=PublicState.Target.Get();
        OutRequest.RunningRequester=PublicState.Requester.Get();
        OutRequest.RunningOrigin=PublicState.Origin;
        OutRequest.RunningStartTime=PublicState.ServerStartTime;
        OutRequest.RunningDuration=PublicState.Duration;
        OutRequest.RunningDeadline=ActionDeadline;
    }
    OutRequest.WorldEpoch = WorldEpoch;
    OutRequest.NpcId = NpcId;
    OutRequest.RequestId = FGuid::NewGuid();
    OutRequest.ContextRevision = ContextRevision;
    OutRequest.PlanRevision = ++NextPlanRevision;
    OutRequest.Persona = Persona.Left(1024);
    OutRequest.bPlayerRequest = bPlayerRequest;
    RequestPlayerState=bPlayerRequest && PlayerReplyController.IsValid()?PlayerReplyController->PlayerState:nullptr;
    OutRequest.RequesterPC=bPlayerRequest?PlayerReplyController.Get():nullptr;
    OutRequest.RequesterPlayerState=RequestPlayerState;
    OutRequest.PlayerMessageId=bPlayerRequest?PlayerReplyMessage:FGuid();
    const auto* Settings = GetDefault<UDeliveryAiSettings>();
    OutRequest.bStreamSentenceAudio = Settings->bStreamReplies && Settings->bStreamSentenceAudio;
    OutRequest.StreamProtocolVersion=Settings->bStreamReplies?2:0;
    OutRequest.bPendingPlayerMessage=bPlayerRequest && PlayerReplyMessage.IsValid();
    OutRequest.SelfSubjectId = NpcId;
    OutRequest.SelfName = TEXT("NPC");
    if (const auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        OutRequest.Voice = Npc->Presentation->TtsVoice.ToString();
        OutRequest.SelfName = Npc->DisplayName.ToString();
        OutRequest.ConversationDepth = Npc->GetConversationDepth();
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
    AttachNpcMessageContext(OutRequest);
    OutRequest.LastDecisionEventIds = LastDecisionEventIds;
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
            if (CanChooseAction(Action,nullptr,RequestPlayerState.Get())) AddCandidate(Action,{},{});
        for (const auto& Pair : KnownSubjects)
        {
            AActor* Target=Pair.Value.Get();
            if (!CanLookAt(Target)) continue;
            const int32 FactIndex=Evidence.FindLastByPredicate([&](const auto& Item){ return Item.SubjectId==Pair.Key; });
            if (FactIndex==INDEX_NONE) continue;
            const auto* Fact=&Evidence[FactIndex];
            for (auto Action : {EDeliveryNpcAction::LookAt,EDeliveryNpcAction::MoveTo,EDeliveryNpcAction::StrikeOnce,EDeliveryNpcAction::HoldVehicle,EDeliveryNpcAction::MoveAside,EDeliveryNpcAction::Fight,EDeliveryNpcAction::Flee})
                if (CanChooseAction(Action,Target,RequestPlayerState.Get())) AddCandidate(Action,Pair.Key,Fact->EventId);
        }
        Resident->ExtendDecisionRequest(OutRequest,RequestPlayerState.Get());
    }
    if (bPlayerRequest)
    {
        OutRequest.RequestedCapabilityVersion=6;
        if (const auto* Companion=GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>())
        {
            OutRequest.bRunningCompanion=HasCompanionActivity();
            OutRequest.bRunningErrand=Companion->HasActiveErrand();
            OutRequest.bRunningHome=Companion->HasActiveHomeArrangement();
            OutRequest.RunningCompanionMode=uint8(Companion->GetMode());
            OutRequest.RunningCompanionOwner=Companion->GetOwnerPlayerState();
            OutRequest.RunningHomeArrangement=Companion->GetHomeState().ArrangementId;
            OutRequest.RunningHomeProp=Companion->GetHomeState().CarriedProp.Get();
            if (!OutRequest.RunningActionId.IsValid() && OutRequest.bRunningCompanion)
                OutRequest.RunningAction=OutRequest.bRunningErrand?Companion->GetErrand().Action:
                    OutRequest.bRunningHome?EDeliveryNpcAction::HomeAction:
                    Companion->GetMode()==EDeliveryCompanionMode::GoHome?EDeliveryNpcAction::GoHome:
                    Companion->GetMode()==EDeliveryCompanionMode::Stay?EDeliveryNpcAction::Stay:EDeliveryNpcAction::Follow;
        }
        if (OutRequest.RunningActionId.IsValid() || OutRequest.bRunningCompanion)
        {
            FDeliveryNpcCandidate Continue;
            Continue.Action=EDeliveryNpcAction::ContinueCurrent;
            OutRequest.Candidates.Add(Continue);
            OutRequest.AllowedActions.AddUnique(Continue.Action);
        }
    }
    return PrepareCandidateIdentities(OutRequest,RequestPlayerState.Get());
}

bool UDeliveryNpcAgentComponent::RequestDecision()
{
    if (!IsAuthority() || bChangingPlan) return false;
    // 录音和等待转写期间保留真实事件与 BT 活动，不把暂缓当成后台失败
    if (!bPlayerRequest && IsPlayerInputBeingRecorded()) return false;
    if (!IsAuthority() || bChangingPlan) return false;
    if (IsDecisionPending()) { bDecisionQueued = true; return true; }
    if (HasActiveAction() && !CanUpdateRunningAction() && !bPlayerRequest) return false;
    auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>();
    if (!Gateway || !BuildDecisionRequest(Gateway->GetWorldEpoch(), ActiveRequest)) return false;
    if (!Gateway->Enqueue(this, ActiveRequest)) { ActiveRequest = {}; return false; }
    bDecisionPending=true; GetOwner()->ForceNetUpdate();
    return true;
}

bool UDeliveryNpcAgentComponent::HasGameplayRuleEvidence(AActor* Target,FGuid EvidenceId) const
{
    if (!IsValid(Target) || Target==GetOwner() || Target->GetWorld()!=GetWorld() || !EvidenceId.IsValid()) return false;
    const double Now=GetWorld()->GetTimeSeconds();
    return Evidence.ContainsByPredicate([this,Target,EvidenceId,Now](const auto& Item)
    {
        return Item.EventId==EvidenceId && Item.Source==EDeliveryNpcEvidenceSource::ActionResult &&
            Item.ExpiresAt>Now && Item.SubjectId.IsValid() && KnownSubjects.FindRef(Item.SubjectId).Get()==Target;
    });
}

bool UDeliveryNpcAgentComponent::IsGameplayRuleActionValid() const
{
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    return Npc && PublicState.Origin==EDeliveryNpcActionOrigin::GameplayRule &&
        PublicState.Action==EDeliveryNpcAction::Fight && PublicState.RuleRevision>0 &&
        FMath::IsFinite(PublicState.Duration) && PublicState.Duration>=UDeliveryNpcCombatComponent::StrikeDuration &&
        PublicState.Duration<=5.f && ActionEvidenceIds.Num()==1 &&
        KnownSubjects.FindRef(ActionTargetId).Get()==PublicState.Target &&
        HasGameplayRuleEvidence(PublicState.Target,ActionEvidenceIds[0]) &&
        Npc->CanExecuteGameplayRule(PublicState.Action,PublicState.Target,PublicState.RuleRevision);
}

bool UDeliveryNpcAgentComponent::RequestGameplayRuleAction(EDeliveryNpcAction Action,AActor* Target,int32 RuleRevision,FGuid EvidenceId,float Duration)
{
    if (!IsAuthority() || bChangingPlan || Action!=EDeliveryNpcAction::Fight || RuleRevision<=0 ||
        !IsValid(Target) || Target==GetOwner() || Target->GetWorld()!=GetWorld() ||
        !FMath::IsFinite(Duration) || Duration<UDeliveryNpcCombatComponent::StrikeDuration || Duration>5.f) return false;
    auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || !Npc->CanExecuteGameplayRule(Action,Target,RuleRevision)) return false;
    PruneEvidence();
    if (!HasGameplayRuleEvidence(Target,EvidenceId)) return false;
    // 同一轮仍在执行时不重置拳数或时间；倒地后可凭仍有效的规则重新开始
    if (HasActiveAction() && PublicState.Origin==EDeliveryNpcActionOrigin::GameplayRule &&
        PublicState.RuleRevision==RuleRevision) return false;

    TGuardValue<bool> ChangingPlan(bChangingPlan,true);
    // 规则打断模型时结束原玩家等待，随后复用原取消路径清掉声音和旧动作
    CancelDecision();
    FinishFailedPlayerReply(PlayerReplyMessage,nullptr,false);
    CancelPlan(TEXT("GameplayRule"));
    // 停止旧能力会发出通知，归属、目标或证据可能在通知中失效
    if (!IsAuthority() || !HasGameplayRuleEvidence(Target,EvidenceId) ||
        !Npc->CanExecuteGameplayRule(Action,Target,RuleRevision)) return false;
    const auto* Fact=Evidence.FindByPredicate([EvidenceId](const auto& Item){ return Item.EventId==EvidenceId; });
    if (!Fact) return false;
    PublicState={};
    PublicState.ActionId=FGuid::NewGuid();
    PublicState.PlanRevision=++NextPlanRevision;
    PublicState.Action=Action;
    PublicState.Origin=EDeliveryNpcActionOrigin::GameplayRule;
    PublicState.RuleRevision=RuleRevision;
    PublicState.Status=EDeliveryNpcActionStatus::Accepted;
    PublicState.Target=Target;
    PublicState.Duration=Duration;
    ActionEvidenceIds={EvidenceId};
    ActionTargetId=Fact->SubjectId;
    ActionConversationDepth=0;
    ActionDeadline=GetWorld()->GetTimeSeconds()+1.0;
    // 不改 LastDecisionEventIds，规则执行不能冒充模型已读过这些事件
    const FGuid AcceptedAction=PublicState.ActionId;
    UE_LOG(LogDeliveryNpcAgent,Log,TEXT("GameplayRule npc=%s action=%s revision=%d evidence=%s"),
        *NpcId.ToString(),*AcceptedAction.ToString(),RuleRevision,*EvidenceId.ToString());
    PublishState();
    return IsAuthority() && PublicState.ActionId==AcceptedAction && HasActiveAction();
}

void UDeliveryNpcAgentComponent::CancelDecision()
{
    if (!IsAuthority()) return;
    InvalidateNpcMessageTurns(TEXT("DecisionCancelled"));
    if (!IsAuthority()) return;
    TArray<APlayerController*> Players;
    if (PlayerReplyController.IsValid()) Players.AddUnique(PlayerReplyController.Get());
    if (ActiveRequest.RequesterPC.IsValid()) Players.AddUnique(ActiveRequest.RequesterPC.Get());
    for (const auto& Pair:Replies) if (Pair.Value.RequesterPC.IsValid()) Players.AddUnique(Pair.Value.RequesterPC.Get());
    for (auto* Player:Players) InterruptPlayerReply(Player);
    if (auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>()) Gateway->Cancel(this);
    ActiveRequest = {};
    RequestPlayerState.Reset();
    bDecisionPending=false;
    bDecisionQueued = false;
    Replies.Reset();
}

bool UDeliveryNpcAgentComponent::HasCompanionActivity() const
{
    const auto* Companion=GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
    // 等候仍要回位和避让，聊天继续保留主人的持续安排
    return Companion && (Companion->HasActiveErrand() || Companion->HasActiveHomeArrangement() ||
        Companion->GetMode()==EDeliveryCompanionMode::Follow || Companion->GetMode()==EDeliveryCompanionMode::Stay ||
        Companion->GetMode()==EDeliveryCompanionMode::GoHome);
}

bool UDeliveryNpcAgentComponent::ShouldPauseForConversation() const
{
    return IsListening() && !HasActiveAction() && !HasCompanionActivity();
}

bool UDeliveryNpcAgentComponent::IsPlayerReplyActive(APlayerController* Requester) const
{
    return GetReplyMessage(Requester).IsValid();
}

bool UDeliveryNpcAgentComponent::CanReplacePendingReply(FGuid ExistingReplyId,FGuid NewReplyId) const
{
    const FReply* Existing=nullptr;
    const FReply* Replacement=nullptr;
    for (const auto& Pair:Replies)
    {
        if (Pair.Value.ReplyId==ExistingReplyId) Existing=&Pair.Value;
        if (Pair.Value.ReplyId==NewReplyId) Replacement=&Pair.Value;
    }
    return Existing && Replacement && Existing->RequesterPC.HasSameIndexAndSerialNumber(Replacement->RequesterPC) &&
        Existing->RequesterPlayerState.HasSameIndexAndSerialNumber(Replacement->RequesterPlayerState);
}

bool UDeliveryNpcAgentComponent::CanStartReplySpeech(FGuid ReplyId) const
{
    for (const auto& Pair:Replies)
        if (Pair.Value.ReplyId==ReplyId)
        {
            if (!CanStartReplyMessageAudio(ReplyId)) return false;
            return Pair.Value.bActionStarted ||
                (Pair.Value.ActionId==PublicState.ActionId &&
                    (PublicState.Status==EDeliveryNpcActionStatus::Running || PublicState.Status==EDeliveryNpcActionStatus::Succeeded));
        }
    return PublicState.GetReplyId()!=ReplyId || PublicState.Status==EDeliveryNpcActionStatus::Running ||
        PublicState.Status==EDeliveryNpcActionStatus::Succeeded;
}

FGuid UDeliveryNpcAgentComponent::GetReplyMessage(APlayerController* Requester) const
{
    if (!IsValid(Requester)) return {};
    if (PlayerReplyController.Get()==Requester && PlayerReplyMessage.IsValid()) return PlayerReplyMessage;
    if (ActiveRequest.RequesterPC.Get()==Requester) return ActiveRequest.PlayerMessageId;
    const FReply* Latest=nullptr;
    for (const auto& Pair:Replies)
        if (Pair.Value.RequesterPC.Get()==Requester && (!Latest || Pair.Value.PlanRevision>Latest->PlanRevision)) Latest=&Pair.Value;
    return Latest?Latest->MessageId:FGuid();
}

void UDeliveryNpcAgentComponent::InterruptPlayerReply(APlayerController* Requester)
{
    if (!IsAuthority() || !IsValid(Requester) || Requester->GetWorld()!=GetWorld()) return;
    TGuardValue<bool> ChangingPlan(bChangingPlan,true);
    TArray<FGuid> Cancelled;
    bool bRequestHasReply=false;
    for (const auto& Pair:Replies)
        if (Pair.Value.RequesterPC.Get()==Requester)
        {
            Cancelled.Add(Pair.Value.ReplyId);
            bRequestHasReply|=Pair.Key==ActiveRequest.RequestId;
        }
    FGuid UnapprovedMessage;
    if (ActiveRequest.RequesterPC.Get()==Requester)
    {
        const FGuid RequestId=ActiveRequest.RequestId;
        TraceRequestEvent(ActiveRequest.WorldEpoch,RequestId,TEXT("Cancelled"),TEXT("PlayerInterrupted"));
        if (!bRequestHasReply) UnapprovedMessage=ActiveRequest.PlayerMessageId;
        ActiveRequest={};
        RequestPlayerState.Reset();
        bDecisionPending=false;
        bDecisionQueued=false;
        if (auto* Gateway=GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>()) Gateway->CancelRequest(this,RequestId);
    }
    if (PlayerReplyController.Get()==Requester)
    {
        if (!UnapprovedMessage.IsValid() && Cancelled.IsEmpty()) UnapprovedMessage=PlayerReplyMessage;
        PlayerReplyMessage.Invalidate(); PlayerReplyController.Reset(); bPlayerRequest=false;
        EndListening();
    }
    // 一次移除整批，停止当前句的回调不能把同一玩家的旧待播句提到队首
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->Presentation->CancelReplies(Cancelled);
    for (const FGuid& ReplyId:Cancelled) RemoveReply(ReplyId,TEXT("PlayerInterrupted"));
    if (Cancelled.Contains(PublicState.GetReplyId()))
    {
        PublicState.Speech.Reset(); PublicState.ReplyToEventId.Invalidate(); PublicState.bStreamSpeech=false;
        PublishState();
    }
    if (UnapprovedMessage.IsValid()) OnPlayerReplyEnded.Broadcast(Requester,UnapprovedMessage,TEXT("PlayerInterrupted"));
    GetOwner()->ForceNetUpdate();
}

bool UDeliveryNpcAgentComponent::BeginListening(AActor* Speaker, FGuid MessageId)
{
    if (!IsAuthority() || bChangingPlan || !IsValid(Speaker) || !MessageId.IsValid()) return false;
    const auto* PlayerPawn=Cast<APawn>(Speaker);
    auto* Requester=PlayerPawn?Cast<APlayerController>(PlayerPawn->GetController()):nullptr;
    if (!Requester || Requester->GetWorld()!=GetWorld()) return false;
    if (PlayerReplyController.IsValid() && PlayerReplyController.Get()!=Requester && PlayerReplyMessage.IsValid()) return false;
    if (ActiveRequest.bPlayerRequest && ActiveRequest.RequesterPC.Get()!=Requester) return false;
    for (const auto& Pair:Replies)
        if (Pair.Value.RequesterPC.IsValid() && Pair.Value.RequesterPC.Get()!=Requester) return false;
    // 录音开始后同一 MessageId 的最终文字到达，沿用本轮倾听而不是自我取消
    if (ListeningMessage==MessageId && PlayerReplyController.Get()==Requester) return true;
    InterruptPlayerReply(Requester);
    ListeningSpeaker = Speaker;
    ListeningMessage = MessageId;
    PlayerReplyMessage = MessageId;
    PlayerReplyController=Requester;
    ListeningStartedAt = ServerTime();
    ListeningUntil = FPlatformTime::Seconds() + 30;
    bPlayerRequest = true;
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
        Npc->Presentation->PlayReaction(EDeliveryNpcVoiceReaction::HeardPlayer,MessageId);
    GetOwner()->ForceNetUpdate();
    UE_LOG(LogDeliveryNpcAgent, Log, TEXT("NpcLatency speech=%s stage=listening"), *MessageId.ToString());
    return true;
}

void UDeliveryNpcAgentComponent::EndListening()
{
    ListeningSpeaker = nullptr;
    ListeningMessage.Invalidate();
    GetOwner()->ForceNetUpdate();
}

bool UDeliveryNpcAgentComponent::ReceiveStreamDecision(FGuid RequestId, const FDeliveryNpcDecisionProposal& Proposal)
{
    if (!IsAuthority() || bChangingPlan || !RequestId.IsValid() || RequestId != ActiveRequest.RequestId) return false;
    const FGuid TraceWorldEpoch=ActiveRequest.WorldEpoch;
    TraceProposal(Proposal);
    FName Error;
    if (!TryAcceptProposal(Proposal, Error))
    {
        ReceiveDecision(RequestId, nullptr, Error);
        return false;
    }
    TraceValidation(TraceWorldEpoch,RequestId,true,NAME_None);
    PublicState.bStreamSpeech = true;
    FReply Reply;
    Reply.ReplyId=PublicState.GetReplyId(); Reply.MessageId=PublicState.ReplyToEventId;
    Reply.ActionId=PublicState.ActionId; Reply.Action=PublicState.Action;
    Reply.Target=PublicState.Target; Reply.bHadTarget=IsValid(PublicState.Target);
    Reply.Listener=PublicState.SpeechTarget; Reply.Depth=ActionConversationDepth;
    Reply.PlanRevision=ActiveRequest.PlanRevision;
    Reply.RequesterPC=ActiveRequest.RequesterPC;
    Reply.RequesterPlayerState=ActiveRequest.RequesterPlayerState;
    Reply.bContinueCurrent=Proposal.Action==EDeliveryNpcAction::ContinueCurrent;
    Reply.bActionStarted=PublicState.Status==EDeliveryNpcActionStatus::Running || PublicState.Status==EDeliveryNpcActionStatus::Succeeded ||
        (Reply.bContinueCurrent && !ActiveRequest.RunningActionId.IsValid());
    if (!RegisterReplyMessageContext(Reply,ActiveRequest,false))
    {
        if (Reply.bOwnsAction) TryFinishAction(Reply.ActionId,EDeliveryNpcActionStatus::Cancelled,TEXT("NpcMessageParentChanged"));
        return false;
    }
    Replies.Add(RequestId,Reply);
    // 继续同一场交战且没有台词时，正在播放的声音保持原样
    if (!Proposal.Speech.TrimStartAndEnd().IsEmpty()) StartReplyPresentation(Reply);
    // 模型生成期间听到的新话仍留给下一轮，收到动作建议不能清掉接话安排
    EndListening();
    PublishState();
    UE_LOG(LogDeliveryNpcAgent, Log, TEXT("NpcLatency request=%s stage=approved"), *RequestId.ToString());
    return true;
}

bool UDeliveryNpcAgentComponent::StartReplyPresentation(FReply Reply)
{
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        if (!Npc->Presentation->HasReply(Reply.ReplyId))
        {
            // 较新的台词已开始准备后，旧请求迟到的第一句不能挤掉它
            if (Reply.PlanRevision<LatestSpeechPlanRevision) { CancelReply(Reply.ReplyId); return false; }
            Npc->Presentation->BeginReply(Reply.ReplyId,Reply.MessageId,Reply.Depth,Reply.Listener.Get());
            if (!Npc->Presentation->HasReply(Reply.ReplyId)) { CancelReply(Reply.ReplyId); return false; }
            if (Reply.bModelFinished) Npc->Presentation->SetReplySentenceCount(Reply.ReplyId,Reply.SentenceCount);
            LatestSpeechPlanRevision=Reply.PlanRevision;
        }
        return Npc->Presentation->HasReply(Reply.ReplyId);
    }
    return false;
}

void UDeliveryNpcAgentComponent::CancelReply(FGuid ReplyId)
{
    RemoveReply(ReplyId,TEXT("ReplyCancelled"));
}

void UDeliveryNpcAgentComponent::FinishReplyPlayback(FGuid ReplyId)
{
    for (const auto& Pair:Replies)
        if (Pair.Value.ReplyId==ReplyId && IsDecisionTraceCurrent(DecisionTrace.WorldEpoch,Pair.Key))
        {
            // 零台词也走完成回调，所以这里只表示回复生命周期结束
            DecisionTrace.ReplyStage=TEXT("Finished");
            PublishDecisionTrace();
            break;
        }
    RemoveReply(ReplyId,TEXT("ReplyCompleted"));
}

void UDeliveryNpcAgentComponent::RemoveReply(FGuid ReplyId,FName Reason)
{
    if (!IsAuthority() || !ReplyId.IsValid()) return;
    FGuid RequestId;
    for (const auto& Pair:Replies)
        if (Pair.Value.ReplyId==ReplyId) { RequestId=Pair.Key; break; }
    if (!RequestId.IsValid()) return;
    const FReply Removed=Replies.FindChecked(RequestId);
    if (Reason!=TEXT("ReplyCompleted")) TraceRequestEvent(DecisionTrace.WorldEpoch,RequestId,TEXT("Cancelled"),Reason);
    Replies.Remove(RequestId);
    // 先撤下自身请求，再发布终态，取消后代的同步回调不能重复处理这轮
    if (auto* Gateway=GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>()) Gateway->CancelRequest(this,RequestId);
    if (ActiveRequest.RequestId==RequestId)
    {
        ActiveRequest={}; bDecisionPending=false; bDecisionQueued=false; RequestPlayerState.Reset();
    }
    if (PlayerReplyController==Removed.RequesterPC && PlayerReplyMessage==Removed.MessageId)
    { PlayerReplyController.Reset(); PlayerReplyMessage.Invalidate(); bPlayerRequest=false; EndListening(); }
    ReportNpcReplyPresentation(ReplyId,Reason==TEXT("ReplyCompleted")?
        EDeliveryNpcReplyPresentation::Completed:EDeliveryNpcReplyPresentation::Interrupted);
    if (!IsAuthority()) return;
    if (Removed.RequesterPC.IsValid() && Removed.MessageId.IsValid())
        OnPlayerReplyEnded.Broadcast(Removed.RequesterPC.Get(),Removed.MessageId,Reason);
}

void UDeliveryNpcAgentComponent::ReceiveSentence(FGuid RequestId, const FDeliveryNpcSentence& Sentence)
{
    if (!CanReceiveReply(RequestId)) return;
    const FReply Reply=Replies.FindChecked(RequestId);
    if (!StartReplyPresentation(Reply)) return;
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        Npc->Presentation->QueueSentence(Reply.ReplyId, Reply.MessageId, Sentence);
        TraceReplyText(RequestId,Sentence.Text);
    }
}

bool UDeliveryNpcAgentComponent::CanReceiveReply(FGuid RequestId) const
{
    const auto* Reply=Replies.Find(RequestId);
    return IsAuthority() && !bChangingPlan && Reply && !Reply->bTransferFinished;
}

bool UDeliveryNpcAgentComponent::ReceiveSpeechBegin(FGuid RequestId, const FDeliveryNpcSpeechBegin& Begin)
{
    if (!CanReceiveReply(RequestId)) return false;
    const FReply Reply=Replies.FindChecked(RequestId);
    if (!StartReplyPresentation(Reply)) return false;
    auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    const bool bAccepted=Npc && Npc->Presentation->BeginSentenceAudio(Reply.ReplyId, Reply.MessageId, Begin);
    if (bAccepted) TraceReplyText(RequestId,Begin.Text);
    return bAccepted;
}

bool UDeliveryNpcAgentComponent::ReceiveAudioChunk(FGuid RequestId, const FDeliveryNpcAudioChunk& Chunk)
{
    if (!CanReceiveReply(RequestId)) return false;
    const FReply Reply=Replies.FindChecked(RequestId);
    auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    return Npc && Npc->Presentation->AppendSentenceAudio(Reply.ReplyId, Reply.MessageId, Chunk);
}

bool UDeliveryNpcAgentComponent::ReceiveAudioEnd(FGuid RequestId, const FDeliveryNpcAudioEnd& End)
{
    if (!CanReceiveReply(RequestId)) return false;
    const FReply Reply=Replies.FindChecked(RequestId);
    auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    return Npc && Npc->Presentation->EndSentenceAudio(Reply.ReplyId, Reply.MessageId, End);
}

bool UDeliveryNpcAgentComponent::FinishModelReply(FGuid RequestId,int32 SentenceCount,TOptional<FString> ApprovedText)
{
    if (!IsAuthority() || bChangingPlan || SentenceCount<0 || SentenceCount>3) return false;
    auto* Reply=Replies.Find(RequestId);
    if (!Reply || Reply->bModelFinished || ActiveRequest.RequestId!=RequestId) return false;
    if (ApprovedText.IsSet())
    {
        if (!DeliveryNpcMessages::IsValidReplyText(ApprovedText.GetValue()) ||
            ((SentenceCount==0)!=ApprovedText.GetValue().IsEmpty())) return false;
        Reply->ApprovedText=MoveTemp(ApprovedText.GetValue());
        Reply->bHasApprovedText=true;
    }
    Reply->bModelFinished=true;
    Reply->SentenceCount=SentenceCount;
    const FGuid ReplyId=Reply->ReplyId;
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->Presentation->SetReplySentenceCount(ReplyId,SentenceCount);
    FinishDecisionWait(RequestId,true,SentenceCount);
    TryDeliverReplyMessage(RequestId);
    return true;
}

void UDeliveryNpcAgentComponent::FinishReply(FGuid RequestId,bool bSucceeded,int32 SentenceCount)
{
    if (!IsAuthority() || bChangingPlan || !RequestId.IsValid()) return;
    auto* Found=Replies.Find(RequestId);
    if (!Found)
    {
        if (ActiveRequest.RequestId==RequestId) ReceiveDecision(RequestId,nullptr,TEXT("TransportFailure"));
        return;
    }
    const FReply Reply=*Found;
    if (Reply.bTransferFinished) return;
    if (SentenceCount<0 || SentenceCount>3 || (Reply.bModelFinished && SentenceCount!=Reply.SentenceCount)) bSucceeded=false;
    if (bSucceeded)
    {
        if (!Reply.bModelFinished && !FinishModelReply(RequestId,SentenceCount)) return;
        if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->Presentation->FinishSentences(Reply.ReplyId,SentenceCount);
        if (auto* Current=Replies.Find(RequestId)) Current->bTransferFinished=true;
        // 零台词没有播放结束回调；只有真实玩家的普通动作仍需等动作回执
        const bool bNeedsPlayerActionReceipt=!Reply.bContinueCurrent && Reply.RequesterPC.IsValid() && Reply.MessageId.IsValid();
        if (SentenceCount==0 && !bNeedsPlayerActionReceipt) FinishReplyPlayback(Reply.ReplyId);
        return;
    }
    // 先撤下失败声音，再处理重试；播放清理不能清掉本轮玩家身份和刚恢复的倾听
    Replies.Remove(RequestId);
    if (auto* Gateway=GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>()) Gateway->CancelRequest(this,RequestId);
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->Presentation->CancelReply(Reply.ReplyId);
    // 传输失败已真正停止这轮声音；保留已送达文本，后代仍等其它祖先播完
    ReportNpcReplyPresentation(Reply.ReplyId,Reply.bHasApprovedText?
        EDeliveryNpcReplyPresentation::Unavailable:EDeliveryNpcReplyPresentation::Interrupted);
    if (!IsAuthority()) return;
    if (ActiveRequest.RequestId==RequestId) FinishDecisionWait(RequestId,false,0);
    else if (Reply.RequesterPC.IsValid() && Reply.MessageId.IsValid())
    {
        if (PlayerReplyMessage==Reply.MessageId && PlayerReplyController==Reply.RequesterPC)
            FinishFailedPlayerReply(Reply.MessageId,nullptr,false);
        else
        {
            if (auto* Conversation=Reply.RequesterPC->FindComponentByClass<UDeliveryConversationComponent>())
                Conversation->ClientResult(TEXT("ReplyFailed"),Reply.MessageId);
            OnPlayerReplyEnded.Broadcast(Reply.RequesterPC.Get(),Reply.MessageId,TEXT("ReplyFailed"));
        }
    }
}

void UDeliveryNpcAgentComponent::FinishDecisionWait(FGuid RequestId,bool bSucceeded,int32 SentenceCount)
{
    if (ActiveRequest.RequestId!=RequestId) return;
    const bool bFinishingCurrentPlayer=!bPlayerRequest ||
        (ActiveRequest.PlayerMessageId.IsValid() && ActiveRequest.PlayerMessageId==PlayerReplyMessage);
    // 动作失败后，表现组件可能已经停止对白；是否零台词仍以本次后台结束通知为准
    const bool bSilent=bSucceeded && SentenceCount==0 && PublicState.bStreamSpeech;
    FGuid FailedMessage;
    AActor* FailedSpeaker=nullptr;
    if (!bSucceeded) GetRequestedPlayerMessage(FailedMessage,FailedSpeaker);
    if (bSucceeded) RecordActionReply();
    const bool bContinueConversation = bDecisionQueued;
    ActiveRequest = {};
    bDecisionPending = false; bDecisionQueued = false;
    if (bSucceeded && bFinishingCurrentPlayer)
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
    Reason = TEXT("StaleProposal");
    auto RejectStale = [&](const TCHAR* Cause, FGuid EventId=FGuid())
    {
        Reason=TEXT("StaleProposal");
        UE_LOG(LogDeliveryNpcAgent,Log,TEXT("Decision stale npc=%s request=%s active_request=%s cause=%s event=%s"),
            *NpcId.ToString(),*Proposal.RequestId.ToString(),*ActiveRequest.RequestId.ToString(),Cause,*EventId.ToString());
        return false;
    };
    if (!IsAuthority()) return RejectStale(TEXT("NotAuthority"));
    if (bChangingPlan) return RejectStale(TEXT("PlanChanging"));
    if (!ActiveRequest.RequestId.IsValid()) return RejectStale(TEXT("RequestEnded"));
    if (!IsNpcMessageParentCurrent(ActiveRequest.ParentNpcMessage)) return RejectStale(TEXT("NpcMessageParentChanged"));
    PruneEvidence();
    const bool bResident=GetOwner()->IsA<ADeliveryNpcCharacter>();
    const bool bUpdatingAction=ActiveRequest.RunningActionId.IsValid();
    if (bUpdatingAction)
    {
        if ((ActiveRequest.RequestedCapabilityVersion>=6?!IsActivitySnapshotCurrent(ActiveRequest):!CanUpdateRunningAction()) || ActiveRequest.RunningActionId!=PublicState.ActionId)
            return RejectStale(TEXT("RunningActionChanged"));
    }
    else if (HasActiveAction()) return RejectStale(TEXT("AnotherActionStarted"));
    // 按发送时保存的信息核对回复，普通视野更新不应让已经生成的台词被丢弃
    // 其他类型的角色仍要求上下文版本完全一致；受击时通过 CancelPlan 取消当前请求
    if (Proposal.RequestId!=ActiveRequest.RequestId) return RejectStale(TEXT("RequestMismatch"));
    if (Proposal.WorldEpoch!=ActiveRequest.WorldEpoch) return RejectStale(TEXT("WorldChanged"));
    if (Proposal.NpcId!=NpcId) return RejectStale(TEXT("NpcMismatch"));
    if (Proposal.ContextRevision!=ActiveRequest.ContextRevision || (!bResident && Proposal.ContextRevision!=ContextRevision))
        return RejectStale(TEXT("ContextChanged"));
    if (Proposal.PlanRevision!=ActiveRequest.PlanRevision) return RejectStale(TEXT("PlanChanged"));
    if (Proposal.CapabilityVersion!=ActiveRequest.RequestedCapabilityVersion)
    { Reason=TEXT("CapabilityVersionMismatch"); return false; }
    FGuid PendingPlayerMessage;
    AActor* RequestSpeaker=nullptr;
    GetRequestedPlayerMessage(PendingPlayerMessage,RequestSpeaker);
    const bool bAnsweringPlayer=PendingPlayerMessage.IsValid();
    // 先回答玩家，后来播完的 NPC 闲聊留给下一轮；新玩家发言仍会作废旧回复
    // 接话安排和原话已经由 HearSpeech 保存，这里不把后来听到的话记为已处理
    if (bResident && !bUpdatingAction)
    {
        const auto* NewMessage=Evidence.FindByPredicate([this,bAnsweringPlayer](const auto& Item)
        {
            return Item.bNeedsReply && (Item.Kind==TEXT("PlayerSpeech") || (!bAnsweringPlayer && (Item.Kind==TEXT("Speech") || Item.Kind==TEXT("NpcMessage")))) && !ActiveRequest.Evidence.ContainsByPredicate(
                [&Item](const auto& Sent){ return Sent.EventId==Item.EventId; });
        });
        if (NewMessage) return RejectStale(NewMessage->Kind==TEXT("PlayerSpeech")?TEXT("NewPlayerSpeech"):TEXT("NewNpcSpeech"),NewMessage->EventId);
    }
    Reason = TEXT("UnsupportedAction");
    if (!ActiveRequest.AllowedActions.Contains(Proposal.Action)) return false;
    const FDeliveryNpcCandidate* SelectedCandidate=nullptr;
    if (ActiveRequest.RequestedCapabilityVersion>=5)
    {
        Reason=TEXT("InvalidCandidate");
        if (!Proposal.CandidateId.IsValid()) return false;
        for (const auto& Candidate:ActiveRequest.Candidates)
        {
            if (Candidate.CandidateId!=Proposal.CandidateId) continue;
            if (SelectedCandidate) return false;
            SelectedCandidate=&Candidate;
        }
        if (!SelectedCandidate || SelectedCandidate->Action!=Proposal.Action ||
            SelectedCandidate->TargetId!=Proposal.TargetId || SelectedCandidate->EvidenceIds!=Proposal.EvidenceIds) return false;
        if (!FMath::IsFinite(Proposal.Duration) || Proposal.Duration>DeliveryNpc::CandidateDuration(SelectedCandidate->Action))
        { Reason=TEXT("InvalidDuration"); return false; }
    }
    if (Proposal.Action==EDeliveryNpcAction::ContinueCurrent)
        return AcceptContinueCurrent(Proposal,Reason);
    TSharedPtr<FDeliveryCompanionHomeChoice> AcceptedHomeChoice;
    Reason = TEXT("InvalidDuration");
    if ((Proposal.Action==EDeliveryNpcAction::Fight || Proposal.Action==EDeliveryNpcAction::Flee) &&
        (!FMath::IsFinite(Proposal.Duration) || Proposal.Duration<UDeliveryNpcCombatComponent::StrikeDuration || Proposal.Duration>DeliveryNpc::MaxDecisionDuration(Proposal.Action))) return false;
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
        if (!CanChooseAction(Proposal.Action,Target,RequestPlayerState.Get())) return false;
        if (DeliveryNpc::IsCompanionCommand(Proposal.Action))
        {
            FDeliveryNpcDecisionRequest Current;
            Resident->ExtendDecisionRequest(Current,RequestPlayerState.Get());
            Reason=TEXT("StaleCompanionCommand");
            if (Current.CompanionCommandRevision!=ActiveRequest.CompanionCommandRevision) return false;
            // 弱引用的普通相等会把两个失效对象看作相同，任务快照须保留原对象身份
            if (DeliveryNpc::IsCompanionErrand(Proposal.Action) &&
                (!Current.CompanionTask.HasSameIndexAndSerialNumber(ActiveRequest.CompanionTask) ||
                    !Current.CompanionPackage.HasSameIndexAndSerialNumber(ActiveRequest.CompanionPackage)))
            { Reason=TEXT("CompanionTaskOrPackageChanged"); return false; }
            if (Proposal.Action==EDeliveryNpcAction::HomeAction)
            {
                Reason=TEXT("StaleHomeChoice");
                const auto* Companion=GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
                if (!SelectedCandidate || !SelectedCandidate->HomeChoice.IsValid() || !Companion) return false;
                const auto& Choice=*SelectedCandidate->HomeChoice;
                if (Choice.CandidateId!=Proposal.CandidateId || Choice.RequestId!=ActiveRequest.RequestId ||
                    !RequestPlayerState.IsValid() || !Choice.Requester.HasSameIndexAndSerialNumber(RequestPlayerState) ||
                    !Companion->CanApplyHomeChoice(Choice,RequestPlayerState.Get(),ActiveRequest.CompanionCommandRevision)) return false;
                AcceptedHomeChoice=MakeShared<FDeliveryCompanionHomeChoice>(Choice);
            }
        }
        const bool bOffered=SelectedCandidate || ActiveRequest.Candidates.ContainsByPredicate([&](const auto& Item)
            { return Item.Action==Proposal.Action && Item.TargetId==Proposal.TargetId; });
        if (!bOffered) return false;
    }
    Reason = TEXT("InvalidDuration");
    if (!FMath::IsFinite(Proposal.Duration) || (DeliveryNpc::IsCompanionCommand(Proposal.Action)?Proposal.Duration!=0.f:(Proposal.Duration < .1f || Proposal.Duration > DeliveryNpc::MaxDecisionDuration(Proposal.Action)))) return false;
    FGuid PlayerMessage;
    AActor* SpeechTarget = nullptr;
    if (bResident)
    {
        for (const auto& Item : ActiveRequest.Evidence)
            if (Item.Kind==TEXT("PlayerSpeech")) { PlayerMessage=Item.EventId; SpeechTarget=KnownSubjects.FindRef(Item.SubjectId).Get(); }
        if (ActiveRequest.Evidence.ContainsByPredicate([&PlayerMessage](const auto& Item)
            { return (Item.Kind==TEXT("SelfSpeech") || Item.Kind==TEXT("ActionReply")) && Item.ReplyToEventId==PlayerMessage; })) PlayerMessage.Invalidate();
        // 没有台词时仍要做出玩家能看见的动作，空等不能算作回应
        const bool bVisibleAction=bUpdatingAction || Proposal.Action==EDeliveryNpcAction::LookAt || Proposal.Action==EDeliveryNpcAction::MoveTo ||
            Proposal.Action==EDeliveryNpcAction::StrikeOnce || Proposal.Action==EDeliveryNpcAction::Fight || Proposal.Action==EDeliveryNpcAction::HoldVehicle ||
            Proposal.Action==EDeliveryNpcAction::ReleaseVehicle || Proposal.Action==EDeliveryNpcAction::LieDown ||
            Proposal.Action==EDeliveryNpcAction::Pace || Proposal.Action==EDeliveryNpcAction::MoveAside || Proposal.Action==EDeliveryNpcAction::Flee || DeliveryNpc::IsCompanionCommand(Proposal.Action);
        if (PlayerMessage.IsValid() && Proposal.Speech.TrimStartAndEnd().IsEmpty() && !bVisibleAction)
        { Reason=TEXT("MissingPlayerReply"); return false; }
        if (!PlayerMessage.IsValid())
        {
            SpeechTarget = nullptr;
            for (int32 Index=ActiveRequest.Evidence.Num()-1;Index>=0;--Index)
            {
                const auto& Message=ActiveRequest.Evidence[Index];
                if (!Message.bNeedsReply || (Message.Kind!=TEXT("Speech") && Message.Kind!=TEXT("NpcMessage"))) continue;
                if (ActiveRequest.Evidence.ContainsByPredicate([&Message](const auto& Item)
                    { return Item.ReplyToEventId==Message.EventId && (Item.Kind==TEXT("SelfSpeech") || Item.Kind==TEXT("ActionReply") ||
                        (Message.Kind==TEXT("NpcMessage") && Item.Kind==TEXT("NpcMessageSent"))); })) continue;
                PlayerMessage=Message.EventId; SpeechTarget=KnownSubjects.FindRef(Message.SubjectId).Get();
                break;
            }
        }
        // 台词对象和动作目标可以不同，例如看着小芳，却回答正在拱火的玩家
        if (ActiveRequest.bPlayerRequest && ActiveRequest.PlayerMessageId.IsValid())
        {
            PlayerMessage=ActiveRequest.PlayerMessageId;
            SpeechTarget=ActiveRequest.RequesterPC.IsValid()?ActiveRequest.RequesterPC->GetPawn():nullptr;
        }
        if (Proposal.bHasSpeechTarget)
        {
            SpeechTarget = KnownSubjects.FindRef(Proposal.SpeechTargetId).Get();
            if (Proposal.SpeechTargetId.IsValid() && (!IsValid(SpeechTarget) || SpeechTarget==GetOwner() ||
                !Cast<APawn>(SpeechTarget) || SpeechTarget->GetWorld()!=GetWorld()))
            { return RejectStale(TEXT("SpeechTargetChanged")); }
        }
    }
    // v1-5 保留原来的交战续期语义，v6 只有明确的 ContinueCurrent 可以保留活动
    const bool bKeepAction=ActiveRequest.RequestedCapabilityVersion<6 && bUpdatingAction && PublicState.Action==Proposal.Action && PublicState.Target==Target;
    const double Now=GetWorld()->GetTimeSeconds();
    if (bKeepAction)
    {
        const double NewEnd=FMath::Min(Now+Proposal.Duration,CombatDeadline);
        const float Remaining=static_cast<float>(NewEnd-Now);
        auto* Npc=CastChecked<ADeliveryNpcCharacter>(GetOwner());
        if (Remaining<=0 || (Proposal.Action==EDeliveryNpcAction::Fight && !Npc->Combat->ContinueFight(PublicState.ActionId,Remaining)))
        { Reason=TEXT("CombatEnded"); return false; }
        ActionEndsAt=NewEnd;
        ActionDeadline=NewEnd+1;
        // 开始时间保持不变，客户端仍能接上原来的动作；本次回复使用新的编号
        PublicState.Duration=static_cast<float>(ServerTime()+Remaining-PublicState.ServerStartTime);
        PublicState.ReplyId=FGuid::NewGuid();
        ScheduleNextActionDecision();
    }
    else if (bUpdatingAction)
    {
        const FGuid PreviousAction=PublicState.ActionId;
        const FGuid UpdatingRequest=ActiveRequest.RequestId;
        TGuardValue<bool> ApplyingUpdate(bApplyingActionUpdate,true);
        TGuardValue<bool> ReplacingFight(bReplacingFight,PublicState.Action==EDeliveryNpcAction::Fight && Proposal.Action==EDeliveryNpcAction::Fight);
        // 全部校验通过后再停止旧拳，旧请求不能在停止通知中重入并换掉新安排
        if (!TryFinishAction(PreviousAction,EDeliveryNpcActionStatus::Cancelled,TEXT("ModelChangedAction")) ||
            !IsAuthority() || ActiveRequest.RequestId!=UpdatingRequest || PublicState.ActionId!=PreviousAction)
        { Reason=TEXT("ActionChangedDuringStop"); return false; }
    }
    // 只记本次模型实际收到的事件，生成和播放期间听到的新话不能算已经处理
    LastDecisionEventIds.Reset(ActiveRequest.Evidence.Num());
    for (const auto& Item : ActiveRequest.Evidence) LastDecisionEventIds.Add(Item.EventId);
    ActionConversationDepth = ActiveRequest.ConversationDepth;
    if (!bKeepAction)
    {
        PublicState = {};
        PublicState.ActionId = FGuid::NewGuid();
        PublicState.ReplyId=PublicState.ActionId;
        PublicState.Status = EDeliveryNpcActionStatus::Accepted;
        PublicState.Duration = Proposal.Duration;
        ActionEndsAt=0;
        if (Proposal.Action!=EDeliveryNpcAction::Fight && Proposal.Action!=EDeliveryNpcAction::Flee) CombatDeadline=0;
        // 行为树一直没有开始执行时也结束请求，避免人物永久停在接收状态
        ActionDeadline=Now+1;
    }
    PublicState.ReplyToEventId=PlayerMessage;
    PublicState.PlanRevision = Proposal.PlanRevision;
    PublicState.Action = Proposal.Action;
    PublicState.Origin = EDeliveryNpcActionOrigin::Model;
    PublicState.Requester=RequestPlayerState.Get();
    PublicState.CompanionCommandRevision=ActiveRequest.CompanionCommandRevision;
    PublicState.CompanionTask=ActiveRequest.CompanionTask.Get();
    PublicState.CompanionPackage=ActiveRequest.CompanionPackage.Get();
    PublicState.HomeChoice=MoveTemp(AcceptedHomeChoice);
    PublicState.Target = Target;
    PublicState.SpeechTarget = SpeechTarget;
    PublicState.Speech=Proposal.Speech.Left(120);
    // 同目标继续当前动作而没有新台词时，保留上一轮模型选择的表情
    if (!bKeepAction || !PublicState.Speech.TrimStartAndEnd().IsEmpty()) PublicState.Emotion=Proposal.Emotion;
    PublicState.bStreamSpeech=false;
    ActionTargetId = Proposal.TargetId;
    ActionEvidenceIds = Proposal.EvidenceIds;
    Reason = NAME_None;
    return true;
}

bool UDeliveryNpcAgentComponent::IsActivitySnapshotCurrent(const FDeliveryNpcDecisionRequest& Request) const
{
    if (Request.RunningActionId.IsValid())
    {
        if (!HasActiveAction() || Request.RunningActionId!=PublicState.ActionId ||
            Request.RunningPlanRevision!=PublicState.PlanRevision || Request.RunningAction!=PublicState.Action ||
            Request.RunningOrigin!=PublicState.Origin ||
            !Request.RunningTarget.HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(PublicState.Target.Get())) ||
            !Request.RunningRequester.HasSameIndexAndSerialNumber(TWeakObjectPtr<APlayerState>(PublicState.Requester.Get())) ||
            Request.RunningStartTime!=PublicState.ServerStartTime || Request.RunningDuration!=PublicState.Duration ||
            Request.RunningDeadline!=ActionDeadline) return false;
        if (!Request.RunningTarget.IsExplicitlyNull() && !Request.RunningTarget.IsValid()) return false;
        if (!Request.RunningRequester.IsExplicitlyNull() && !Request.RunningRequester.IsValid()) return false;
    }
    else if (HasActiveAction()) return false;
    if (Request.bHasCompanionContext)
    {
        const auto* Companion=GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
        if (!Companion || (Request.bRunningCompanion && !Request.RunningCompanionOwner.IsValid()) || !Request.RunningCompanionOwner.HasSameIndexAndSerialNumber(
                TWeakObjectPtr<APlayerState>(Companion->GetOwnerPlayerState())) ||
            Request.CompanionCommandRevision!=Companion->GetCommandRevision() ||
            Request.RunningCompanionMode!=uint8(Companion->GetMode()) ||
            Request.bRunningErrand!=Companion->HasActiveErrand() || Request.bRunningHome!=Companion->HasActiveHomeArrangement()) return false;
        FDeliveryNpcDecisionRequest Current;
        Companion->ExtendDecisionRequest(Current,Request.RequesterPlayerState.Get());
        if (!Request.CompanionTask.HasSameIndexAndSerialNumber(Current.CompanionTask) ||
            !Request.CompanionPackage.HasSameIndexAndSerialNumber(Current.CompanionPackage)) return false;
        if (Request.bRunningHome && (Request.RunningHomeArrangement!=Companion->GetHomeState().ArrangementId ||
            !Request.RunningHomeProp.HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(Companion->GetHomeState().CarriedProp.Get())))) return false;
        if (Request.bRunningCompanion && !HasCompanionActivity()) return false;
    }
    return Request.RunningActionId.IsValid() || Request.bRunningCompanion;
}

bool UDeliveryNpcAgentComponent::AcceptContinueCurrent(const FDeliveryNpcDecisionProposal& Proposal,FName& Reason)
{
    if (ActiveRequest.bPendingPlayerMessage && Proposal.Speech.TrimStartAndEnd().IsEmpty())
    { Reason=TEXT("MissingPlayerReply"); return false; }
    Reason=TEXT("StaleActivity");
    if (ActiveRequest.RequestedCapabilityVersion<6 || Proposal.Duration!=0.f || Proposal.TargetId.IsValid() ||
        !Proposal.EvidenceIds.IsEmpty() || !IsActivitySnapshotCurrent(ActiveRequest)) return false;
    AActor* Listener=nullptr;
    FGuid ReplyTo=ActiveRequest.PlayerMessageId;
    if (auto* Turns=BoundNpcTurns.Get(); Turns && ActiveRequest.ParentNpcMessage.IsValid())
    {
        FDeliveryNpcMessage Parent;
        if (Turns->GetNpcMessage(ActiveRequest.ParentNpcMessage,Cast<ADeliveryNpcCharacter>(GetOwner()),Parent))
        { Listener=Parent.Sender.Get(); ReplyTo=Parent.Handle.MessageId; }
    }
    if (ActiveRequest.RequesterPC.IsValid()) Listener=ActiveRequest.RequesterPC->GetPawn();
    if (Proposal.bHasSpeechTarget)
    {
        Listener=KnownSubjects.FindRef(Proposal.SpeechTargetId).Get();
        if (Proposal.SpeechTargetId.IsValid() && (!IsValid(Listener) || !Cast<APawn>(Listener) ||
            Listener==GetOwner() || Listener->GetWorld()!=GetWorld())) { Reason=TEXT("InvalidSpeechTarget"); return false; }
    }
    LastDecisionEventIds.Reset(ActiveRequest.Evidence.Num());
    for (const auto& Item:ActiveRequest.Evidence) LastDecisionEventIds.Add(Item.EventId);
    // 只更新本轮回复，不重新执行 GAS/BT，也不延长原来的活动期限
    PublicState.ReplyId=FGuid::NewGuid();
    PublicState.ReplyToEventId=ReplyTo;
    PublicState.SpeechTarget=Listener;
    PublicState.Speech=Proposal.Speech.Left(120);
    PublicState.Emotion=Proposal.Emotion;
    PublicState.bStreamSpeech=false;
    ActionConversationDepth=ActiveRequest.ConversationDepth;
    Reason=NAME_None;
    return true;
}

void UDeliveryNpcAgentComponent::ReceiveDecision(FGuid RequestId, const FDeliveryNpcDecisionProposal* Proposal, FName Error)
{
    if (!IsAuthority() || bChangingPlan || !RequestId.IsValid() || RequestId != ActiveRequest.RequestId) return;
    const FGuid TraceWorldEpoch=ActiveRequest.WorldEpoch;
    if (Proposal) TraceProposal(*Proposal);
    const FGuid RequestedPlayerMessage=ActiveRequest.PlayerMessageId;
    const auto RequestedPlayerController=ActiveRequest.RequesterPC;
    TOptional<FReply> AcceptedReply;
    bool bAccepted = Error.IsNone() && Proposal && TryAcceptProposal(*Proposal, Error);
    TraceValidation(TraceWorldEpoch,RequestId,bAccepted,Error);
    const bool bRetry = bDecisionQueued && !bAccepted;
    const bool bContinueConversation = bDecisionQueued && bAccepted;
    FGuid FailedMessage;
    AActor* FailedSpeaker=nullptr;
    if (!bAccepted) GetRequestedPlayerMessage(FailedMessage,FailedSpeaker);
    const bool bFinishingCurrentPlayer=!bPlayerRequest ||
        (ActiveRequest.PlayerMessageId.IsValid() && ActiveRequest.PlayerMessageId==PlayerReplyMessage);
    if (bAccepted)
    {
        FReply Reply;
        Reply.ReplyId=PublicState.GetReplyId(); Reply.MessageId=PublicState.ReplyToEventId;
        Reply.RequesterPC=ActiveRequest.RequesterPC; Reply.RequesterPlayerState=ActiveRequest.RequesterPlayerState;
        Reply.ActionId=PublicState.ActionId; Reply.Action=PublicState.Action;
        Reply.PlanRevision=ActiveRequest.PlanRevision;
        Reply.bModelFinished=true; Reply.bTransferFinished=true;
        Reply.bContinueCurrent=Proposal->Action==EDeliveryNpcAction::ContinueCurrent;
        Reply.ApprovedText=Proposal->Speech;
        Reply.bHasApprovedText=DeliveryNpcMessages::IsValidReplyText(Reply.ApprovedText);
        Reply.SentenceCount=Reply.ApprovedText.IsEmpty()?0:1;
        if (RegisterReplyMessageContext(Reply,ActiveRequest,true))
        { Replies.Add(RequestId,Reply); AcceptedReply=Reply; }
        else
        {
            bAccepted=false; Error=TEXT("NpcMessageParentChanged");
            if (Reply.bOwnsAction) TryFinishAction(Reply.ActionId,EDeliveryNpcActionStatus::Cancelled,Error);
        }
    }
    ActiveRequest = {};
    bDecisionPending=false;
    bDecisionQueued = false;
    UE_LOG(LogDeliveryNpcAgent, Display, TEXT("Decision npc=%s request=%s accepted=%d reason=%s"),
        *NpcId.ToString(), *RequestId.ToString(), bAccepted, *Error.ToString());
    if (bAccepted)
    {
        TryDeliverReplyMessage(RequestId);
        if (!IsAuthority()) return;
        PublishState();
        // 普通 HTTP 回复也可能续接同一段行动，使用本次已保存的回复，回调后的新 PublicState 不能冒充旧台词
        if (!IsAuthority()) return;
        if (AcceptedReply.IsSet() && Replies.Contains(RequestId))
        {
            const FReply Reply=AcceptedReply.GetValue();
            if ((Reply.bActionStarted || Reply.bContinueCurrent) && !Reply.ApprovedText.IsEmpty())
                if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()))
                    if (!Npc->Presentation->PublishSpeech(Reply.ApprovedText,Reply.Emotion,Reply.Depth,Reply.MessageId,Reply.ReplyId))
                        CancelReply(Reply.ReplyId);
        }
    }
    if (bAccepted && bFinishingCurrentPlayer && (!bPlayerRequest ||
        (PlayerReplyMessage==RequestedPlayerMessage && PlayerReplyController==RequestedPlayerController)))
    {
        bPlayerRequest = false;
        const bool bAwaitingAction=HasActiveAction() && PlayerReplyMessage.IsValid() &&
            PlayerReplyMessage==PublicState.ReplyToEventId && PublicState.Speech.TrimStartAndEnd().IsEmpty();
        if (!bAwaitingAction) { PlayerReplyMessage.Invalidate(); PlayerReplyController.Reset(); }
        EndListening();
    }
    if (bAccepted && AcceptedReply.IsSet() && AcceptedReply->bContinueCurrent && AcceptedReply->ApprovedText.TrimStartAndEnd().IsEmpty())
        FinishReplyPlayback(AcceptedReply->ReplyId);
    // 终态通知只能处理原请求，新请求已由同步回调开始时不重置它的唤醒与重试状态
    if (!IsAuthority() || (ActiveRequest.RequestId.IsValid() && ActiveRequest.RequestId!=RequestId)) return;
    OnDecisionFinished.Broadcast(Error);
    if (!IsAuthority()) return;
    auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    // 完成通知会结束本轮等待，新话需要在当前动作和台词结束后继续处理
    if (bContinueConversation && Npc) Npc->WakeDecision();
    const bool bRetryAllowed=!FailedMessage.IsValid() || FailedMessage!=PlayerReplyMessage || (Npc && Npc->IsDecisionRetryScheduled());
    const bool bRetryStarted=bRetry && bRetryAllowed && !IsDecisionPending() && (!HasActiveAction() || CanUpdateRunningAction() || bPlayerRequest) && RequestDecision();
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
    TArray<FGuid> FailedReplies;
    for (const auto& Pair:Replies)
        if (Pair.Value.MessageId==MessageId && Pair.Value.RequesterPC.Get()==Controller) FailedReplies.Add(Pair.Value.ReplyId);
    for (const FGuid& ReplyId:FailedReplies) RemoveReply(ReplyId,TEXT("ReplyFailed"));
    if (Controller && FailedReplies.IsEmpty()) OnPlayerReplyEnded.Broadcast(Controller,MessageId,TEXT("ReplyFailed"));
}

bool UDeliveryNpcAgentComponent::HasActiveAction() const
{
    return PublicState.Status == EDeliveryNpcActionStatus::Accepted || PublicState.Status == EDeliveryNpcActionStatus::Running;
}

bool UDeliveryNpcAgentComponent::CanUpdateRunningAction() const
{
    if (!IsAuthority() || bChangingPlan || PublicState.Status!=EDeliveryNpcActionStatus::Running ||
        PublicState.Origin!=EDeliveryNpcActionOrigin::Model ||
        (PublicState.Action!=EDeliveryNpcAction::Fight && PublicState.Action!=EDeliveryNpcAction::Flee)) return false;
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || Npc->IsKnockedDown() || !IsValid(PublicState.Target) || PublicState.Target->GetWorld()!=GetWorld() ||
        GetWorld()->GetTimeSeconds()>=ActionEndsAt || GetWorld()->GetTimeSeconds()>=CombatDeadline) return false;
    return PublicState.Action!=EDeliveryNpcAction::Fight || Npc->Combat->GetFightStopReason(PublicState.ActionId).IsNone();
}

bool UDeliveryNpcAgentComponent::CanChooseAction(EDeliveryNpcAction Action,AActor* Target,APlayerState* Requester) const
{
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc) return false;
    // 执行中的重新判断先开放交战、逃跑和停手，其他玩法仍使用原来的请求入口
    if (!bPlayerRequest && CanUpdateRunningAction() && Action!=EDeliveryNpcAction::Fight && Action!=EDeliveryNpcAction::Flee &&
        Action!=EDeliveryNpcAction::Wait && Action!=EDeliveryNpcAction::LookAt) return false;
    return Npc->CanExecuteForRequester(Action,Target,Requester);
}

double UDeliveryNpcAgentComponent::GetActionTimeRemaining() const
{
    return GetWorld()?FMath::Max(0.0,ActionEndsAt-GetWorld()->GetTimeSeconds()):0;
}

void UDeliveryNpcAgentComponent::ScheduleNextActionDecision()
{
    NextActionDecisionAt=GetWorld()->GetTimeSeconds()+FMath::Max(.25,GetActionTimeRemaining()-DeliveryNpc::CombatDecisionLeadSeconds);
}

bool UDeliveryNpcAgentComponent::StartAction(FDeliveryNpcPublicState& OutAction)
{
    if (!IsAuthority() || bChangingPlan || PublicState.Status != EDeliveryNpcActionStatus::Accepted) return false;
    if (PublicState.Origin==EDeliveryNpcActionOrigin::Model && PublicState.ActionId==NpcMessageActionId &&
        PublicState.PlanRevision==NpcMessageActionPlan && !IsNpcMessageParentCurrent(NpcMessageActionParent))
    { TryFinishAction(PublicState.ActionId,EDeliveryNpcActionStatus::Cancelled,TEXT("NpcMessageParentChanged")); return false; }
    PruneEvidence();
    const bool bEvidenceValid = Algo::AllOf(ActionEvidenceIds, [this](const FGuid& Id)
        { return Evidence.ContainsByPredicate([&Id](const auto& Item) { return Item.EventId == Id; }); });
    if (GetWorld()->GetTimeSeconds() >= ActionDeadline || !bEvidenceValid ||
        (PublicState.Action == EDeliveryNpcAction::LookAt && (!KnownSubjects.Contains(ActionTargetId) || !CanLookAt(PublicState.Target))))
    {
        TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Failed, TEXT("PreconditionChanged"));
        return false;
    }
    if (PublicState.Origin==EDeliveryNpcActionOrigin::GameplayRule)
    {
        if (!IsGameplayRuleActionValid())
        { TryFinishAction(PublicState.ActionId,EDeliveryNpcActionStatus::Failed,TEXT("GameplayRuleExpired")); return false; }
    }
    else if (auto* Resident=Cast<ADeliveryNpcCharacter>(GetOwner()))
        if (!Resident->CanExecuteForRequester(PublicState.Action,PublicState.Target,PublicState.Requester))
        { TryFinishAction(PublicState.ActionId,EDeliveryNpcActionStatus::Failed,TEXT("PreconditionChanged")); return false; }
    PublicState.Status = EDeliveryNpcActionStatus::Running;
    PublicState.ServerStartTime = ServerTime();
    const double Now=GetWorld()->GetTimeSeconds();
    ActionEndsAt=Now+PublicState.Duration;
    if (PublicState.Origin==EDeliveryNpcActionOrigin::Model &&
        (PublicState.Action==EDeliveryNpcAction::Fight || PublicState.Action==EDeliveryNpcAction::Flee))
    {
        if (CombatDeadline<=0) CombatDeadline=Now+DeliveryNpc::MaxContinuousCombatSeconds;
        ActionEndsAt=FMath::Min(ActionEndsAt,CombatDeadline);
        PublicState.Duration=static_cast<float>(ActionEndsAt-Now);
        if (PublicState.Duration<=0)
        { TryFinishAction(PublicState.ActionId,EDeliveryNpcActionStatus::Failed,TEXT("CombatTimeLimit")); return false; }
        ScheduleNextActionDecision();
    }
    ActionDeadline = ActionEndsAt + 1.0;
    for (auto& Pair:Replies)
        if (Pair.Value.ActionId==PublicState.ActionId) Pair.Value.bActionStarted=true;
    OutAction = PublicState;
    TraceAction(OutAction.ActionId,TEXT("ActionStarted"));
    PublishState();
    // 状态通知可能同步取消动作，行为树不能接着执行已经取消的那一份状态
    return IsAuthority() && PublicState.ActionId==OutAction.ActionId && PublicState.Status==EDeliveryNpcActionStatus::Running;
}

bool UDeliveryNpcAgentComponent::TryFinishAction(FGuid ActionId, EDeliveryNpcActionStatus Result, FName Reason)
{
    if (!IsAuthority() || !HasActiveAction() || ActionId != PublicState.ActionId || !DeliveryNpc::IsTerminal(Result) ||
        (Result == EDeliveryNpcActionStatus::Succeeded && PublicState.Status != EDeliveryNpcActionStatus::Running)) return false;
    TGuardValue<bool> ChangingPlan(bChangingPlan,true);
    const bool bGameplayRule=PublicState.Origin==EDeliveryNpcActionOrigin::GameplayRule;
    const bool bNeverStarted=PublicState.Status==EDeliveryNpcActionStatus::Accepted;
    FGuid RetryPlayerMessage;
    TWeakObjectPtr<APlayerController> RetryPlayerController;
    if (!bApplyingActionUpdate)
    {
        // 已结束行动的后台回复失效，倒地恢复或换目标后也不能让旧决定重新生效
        if (ActiveRequest.RunningActionId==ActionId && !Replies.Contains(ActiveRequest.RequestId))
        {
            const auto Expired=ActiveRequest;
            // 原动作正常做完后，用最新现场继续回答同一句玩家发言
            // 旧回复仍取消，避免迟到的 ContinueCurrent 重新开始已经结束的动作
            const bool bKeepPlayerMessage=Result==EDeliveryNpcActionStatus::Succeeded &&
                Expired.bPlayerRequest && Expired.bPendingPlayerMessage && bPlayerRequest &&
                Expired.PlayerMessageId.IsValid() && Expired.PlayerMessageId==PlayerReplyMessage &&
                Expired.RequesterPC.IsValid() && Expired.RequesterPC==PlayerReplyController;
            if (bKeepPlayerMessage)
            { RetryPlayerMessage=Expired.PlayerMessageId; RetryPlayerController=Expired.RequesterPC; }
            ActiveRequest={}; bDecisionPending=false; bDecisionQueued=false;
            if (auto* Gateway=GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>()) Gateway->CancelRequest(this,Expired.RequestId);
            if (!bKeepPlayerMessage && Expired.PlayerMessageId==PlayerReplyMessage)
            { PlayerReplyMessage.Invalidate(); PlayerReplyController.Reset(); bPlayerRequest=false; EndListening(); }
            if (!bKeepPlayerMessage && Expired.RequesterPC.IsValid())
                OnPlayerReplyEnded.Broadcast(Expired.RequesterPC.Get(),Expired.PlayerMessageId,TEXT("ActivityEnded"));
            UE_LOG(LogDeliveryNpcAgent,Log,TEXT("NpcLatency request=%s speech=%s stage=activity_request_ended retry_player=%d result=%s reason=%s"),
                *Expired.RequestId.ToString(),*Expired.PlayerMessageId.ToString(),bKeepPlayerMessage,
                *StaticEnum<EDeliveryNpcActionStatus>()->GetNameStringByValue(static_cast<int64>(Result)),*Reason.ToString());
        }
        CombatDeadline=0;
    }
    ActionEndsAt=0;
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
    // 停止能力期间可能换主人或销毁，旧动作不能把结果写回已清空的上下文
    if (!IsAuthority() || PublicState.ActionId!=ActionId) return true;
    UE_LOG(LogDeliveryNpcAgent, Display, TEXT("Action npc=%s action=%s revision=%d result=%s reason=%s"),
        *NpcId.ToString(), *ActionId.ToString(), PublicState.PlanRevision,
        *StaticEnum<EDeliveryNpcActionStatus>()->GetNameStringByValue(static_cast<int64>(Result)), *Reason.ToString());
    TraceAction(ActionId,TEXT("ActionFinished"));
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
    // 动作失败时仍等后台确认是否有台词，不能先删掉零台词回执的请求归属
    if (bNeverStarted && Result!=EDeliveryNpcActionStatus::Succeeded)
    {
        TArray<FGuid> UnstartedReplies;
        for (const auto& Pair:Replies)
            if (Pair.Value.ActionId==ActionId && !Pair.Value.bActionStarted)
                UnstartedReplies.Add(Pair.Key);
        // 活动已经终止，尚未兑现的行动台词按真实失败结束或重试，不能一直等开口
        TGuardValue<bool> EndingReply(bChangingPlan,false);
        for (const FGuid& RequestId:UnstartedReplies) FinishReply(RequestId,false);
        if (!IsAuthority() || PublicState.ActionId!=ActionId) return true;
    }
    // 回复已结束时处理动作回执；等待重试的玩家仍要收到下一次请求的结果
    if (!bDecisionPending && !bPlayerRequest && PlayerReplyMessage==PublicState.ReplyToEventId)
    {
        const bool bSilent=PublicState.bStreamSpeech
            ? FightingNpc && FightingNpc->Presentation->IsReplyWithoutSpeech(PublicState.GetReplyId())
            : PublicState.Speech.TrimStartAndEnd().IsEmpty();
        if (Result!=EDeliveryNpcActionStatus::Succeeded && bSilent)
            FinishFailedPlayerReply(PublicState.ReplyToEventId,nullptr,false);
        PlayerReplyMessage.Invalidate(); PlayerReplyController.Reset();
    }
    PublishState();
    const bool bRetryPlayer=RetryPlayerMessage.IsValid() && RetryPlayerMessage==PlayerReplyMessage &&
        RetryPlayerController.IsValid() && RetryPlayerController==PlayerReplyController && bPlayerRequest;
    if ((bGameplayRule || bRetryPlayer) && IsAuthority() && PublicState.ActionId==ActionId)
        if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->WakeDecision();
    return true;
}

void UDeliveryNpcAgentComponent::RecordActionReply()
{
    if (PublicState.Status!=EDeliveryNpcActionStatus::Succeeded || !PublicState.ReplyToEventId.IsValid()) return;
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    const bool bSilent=PublicState.bStreamSpeech
        ? Npc && Npc->Presentation->IsReplyWithoutSpeech(PublicState.GetReplyId())
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
    FinishReplyPlayback(PublicState.GetReplyId());
}

void UDeliveryNpcAgentComponent::CancelPlan(FName Reason)
{
    if (!IsAuthority()) return;
    TGuardValue<bool> ChangingPlan(bChangingPlan,true);
    CancelDecision();
    TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Cancelled, Reason);
    EndListening();
    bPlayerRequest = false;
    PlayerReplyMessage.Invalidate();
    PlayerReplyController.Reset();
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->Presentation->CancelSpeech();
}

void UDeliveryNpcAgentComponent::ResetContext(FName Reason)
{
    if (!IsAuthority()) return;
    CancelPlan(Reason);
    Evidence.Reset(); KnownSubjects.Reset(); RecentEventIds.Reset(); LastDecisionEventIds.Reset();
    ActionEvidenceIds.Reset(); ActionTargetId.Invalidate(); ActionConversationDepth=0;
    ++ContextRevision;
    PublicState={};
    PublishState();
}

void UDeliveryNpcAgentComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!IsAuthority()) return;
    TArray<FGuid> OrphanedReplies;
    for (const auto& Pair:Replies)
        if (!Pair.Value.RequesterPC.IsExplicitlyNull() && !Pair.Value.RequesterPC.IsValid()) OrphanedReplies.Add(Pair.Value.ReplyId);
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()); Npc && !OrphanedReplies.IsEmpty()) Npc->Presentation->CancelReplies(OrphanedReplies);
    for (const FGuid& ReplyId:OrphanedReplies) RemoveReply(ReplyId,TEXT("RequesterLost"));
    if (ActiveRequest.bPlayerRequest && !ActiveRequest.RequesterPC.IsValid())
    {
        const FGuid RequestId=ActiveRequest.RequestId;
        ActiveRequest={}; bDecisionPending=false; bDecisionQueued=false;
        if (auto* Gateway=GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>()) Gateway->CancelRequest(this,RequestId);
        PlayerReplyMessage.Invalidate(); PlayerReplyController.Reset(); bPlayerRequest=false; EndListening();
    }
    if (ListeningMessage.IsValid() && (!IsValid(ListeningSpeaker) || FPlatformTime::Seconds() > ListeningUntil || ListeningSpeaker->GetWorld() != GetWorld() ||
        (PlayerReplyController.IsValid() && PlayerReplyController->GetPawn()!=ListeningSpeaker) ||
        FVector::DistSquared(GetOwner()->GetActorLocation(), ListeningSpeaker->GetActorLocation()) > FMath::Square(900.f)))
    {
        if (auto* Requester=PlayerReplyController.Get()) InterruptPlayerReply(Requester);
        else EndListening();
    }
    if (HasActiveAction() && PublicState.Origin==EDeliveryNpcActionOrigin::Model && PublicState.ActionId==NpcMessageActionId &&
        PublicState.PlanRevision==NpcMessageActionPlan && !IsNpcMessageParentCurrent(NpcMessageActionParent))
        TryFinishAction(PublicState.ActionId,EDeliveryNpcActionStatus::Cancelled,TEXT("NpcMessageParentChanged"));
    else if (HasActiveAction() && PublicState.Origin==EDeliveryNpcActionOrigin::GameplayRule && !IsGameplayRuleActionValid())
        TryFinishAction(PublicState.ActionId,EDeliveryNpcActionStatus::Cancelled,TEXT("GameplayRuleExpired"));
    else if (HasActiveAction() && GetWorld()->GetTimeSeconds() >= ActionDeadline)
        TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Failed, TEXT("ExecutorTimeout"));
    else if (PublicState.Status == EDeliveryNpcActionStatus::Running && PublicState.Action == EDeliveryNpcAction::LookAt && !CanLookAt(PublicState.Target))
        TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Cancelled, TEXT("TargetUnavailable"));
    // 模型完成上一轮选择后开始考虑下一步，当前动作和对白继续进行
    if (CanUpdateRunningAction() && !IsDecisionPending() && GetWorld()->GetTimeSeconds()>=NextActionDecisionAt)
    {
        NextActionDecisionAt=GetWorld()->GetTimeSeconds()+1;
        RequestDecision();
    }
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
    InvalidateNpcMessageTurns(TEXT("EndPlay"),true);
    if (auto* Turns=BoundNpcTurns.Get()) Turns->OnNpcMessagesInvalidated.RemoveAll(this);
    BoundNpcTurns.Reset();
    PendingNpcMessageCancellations.Reset();
    TSet<FGuid> EndedMessages;
    TArray<TPair<TWeakObjectPtr<APlayerController>,FGuid>> EndedReplies;
    if (PlayerReplyController.IsValid() && PlayerReplyMessage.IsValid())
    {
        EndedMessages.Add(PlayerReplyMessage);
        EndedReplies.Emplace(PlayerReplyController,PlayerReplyMessage);
    }
    for (const auto& Pair:Replies)
        if (Pair.Value.RequesterPC.IsValid() && Pair.Value.MessageId.IsValid() && !EndedMessages.Contains(Pair.Value.MessageId))
        {
            EndedMessages.Add(Pair.Value.MessageId);
            EndedReplies.Emplace(Pair.Value.RequesterPC,Pair.Value.MessageId);
        }
    Replies.Reset();
    ListeningSpeaker=nullptr;
    ListeningMessage.Invalidate();
    PlayerReplyMessage.Invalidate();
    PlayerReplyController.Reset();
    bPlayerRequest=false;
    // 先清空自身状态再通知，外部结束回调不能修改正在遍历的回复表
    for (const auto& Ended:EndedReplies)
        if (Ended.Key.IsValid()) OnPlayerReplyEnded.Broadcast(Ended.Key.Get(),Ended.Value,TEXT("EndPlay"));
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
    LastDecisionEventIds.Reset();
    ActionConversationDepth = 0;
    KnownSubjects.Reset();
    OnPublicStateChanged.Clear();
    OnDecisionFinished.Clear();
    OnPlayerReplyEnded.Clear();
    DecisionTrace={}; TraceQueuedAt=-1; TraceDispatchedAt=-1;
    OnDecisionTraceChanged.Clear();
    Super::EndPlay(EndPlayReason);
}
