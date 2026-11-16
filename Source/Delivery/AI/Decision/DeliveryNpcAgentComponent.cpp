#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "World/Location/DeliveryLocationRegistry.h"
#include "Inventory/Items/DeliveryHandheldItem.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "AI/Conversation/DeliveryConversationTurnSubsystem.h"
#include "AI/Conversation/DeliveryNpcConversationTrace.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Character/DeliveryCharacter.h"
#include "Algo/AllOf.h"
#include "AIController.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "Vehicle/Summon/DeliveryVehicleSummonComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Inventory/Items/DeliveryPackageItem.h"
#include "Net/UnrealNetwork.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeliveryNpcAgent, Log, All);

namespace
{
// 截断文字并把控制字符换成空格，用于诊断显示
FString TraceText(const FString& Text, int32 Limit)
{
    FString Result = Text.Left(Limit);
    for (TCHAR& Character : Result)
        if (FChar::IsControl(Character))
            Character = TEXT(' ');
    return Result.TrimStartAndEnd();
}

// 按编号取对象名称，用于诊断显示
FString TraceSubject(const FDeliveryNpcDecisionRequest& Request, FGuid Id)
{
    const auto* Subject = Request.Subjects.FindByPredicate(
        [Id](const auto& Item)
        {
            return Item.SubjectId == Id;
        });
    return Subject ? TraceText(Subject->Name, 64) : FString();
}

// 对话类事件，保存时间比现场事件长
bool IsConversationMemory(FName Kind)
{
    return Kind == TEXT("NpcMessage") || Kind == TEXT("NpcMessageSent") || Kind == TEXT("PlayerSpeech") ||
           Kind == TEXT("Speech") || Kind == TEXT("OverheardSpeech") || Kind == TEXT("SelfSpeech") || Kind == TEXT("ActionReply");
}

// 看见、看不见类事件
bool IsSightMemory(const FDeliveryNpcEvidence& Item)
{
    return Item.Source == EDeliveryNpcEvidenceSource::Seen &&
           (Item.Kind == TEXT("VisibleNow") || Item.Kind == TEXT("Perceived") || Item.Kind == TEXT("LostSight") ||
            Item.Kind == TEXT("LastSeen"));
}

// 给每个候选分配编号，模型回复里的候选序号按这个编号还原，能力版本低于 5 时不需要
bool PrepareCandidateIdentities(FDeliveryNpcDecisionRequest& Request, APlayerState* Requester)
{
    if (Request.RequestedCapabilityVersion < 1 ||
        Request.RequestedCapabilityVersion > FDeliveryNpcDecisionRequest::CapabilityVersion)
        return false;
    if (Request.RequestedCapabilityVersion < 5)
        return true;
    auto& IndexById = Request.CandidateIndexById;
    IndexById.Reset();
    IndexById.Reserve(Request.Candidates.Num());
    for (int32 Index = 0; Index < Request.Candidates.Num(); ++Index)
    {
        auto& Candidate = Request.Candidates[Index];
        const bool bHomeAction = Candidate.Action == EDeliveryNpcAction::HomeAction;
        if (bHomeAction != Candidate.HomeChoice.IsValid())
            return false;
        const bool bItemAction = DeliveryNpc::IsCompanionItemErrand(Candidate.Action);
        if (bItemAction != Candidate.ItemChoice.IsValid())
            return false;
        if (bItemAction)
        {
            const auto& Choice = *Candidate.ItemChoice;
            if (!Request.bPlayerRequest || !IsValid(Requester) || Choice.SubjectId != Candidate.TargetId ||
                Candidate.EvidenceIds.Num() != 1 || Candidate.EvidenceIds[0] != Choice.EvidenceId ||
                Choice.GetAction() != Candidate.Action)
                return false;
            if (Candidate.Action == EDeliveryNpcAction::SearchNearOwner)
            {
                if (Candidate.TargetId.IsValid() || Choice.Item.IsValid() || !Choice.SourceCarrier.IsValid() ||
                    !Request.bPendingPlayerMessage || !Request.bRequesterIsCompanionOwner ||
                    Choice.EvidenceId != Request.PlayerMessageId)
                    return false;
            }
            else if (!Candidate.TargetId.IsValid() || !Choice.Item.IsValid())
                return false;
        }
        if (bHomeAction)
        {
            if (!Request.bPlayerRequest || !IsValid(Requester) || Candidate.TargetId.IsValid())
                return false;
            const FGuid ChoiceId = Candidate.HomeChoice->CandidateId;
            if (ChoiceId.IsValid())
            {
                if (Candidate.CandidateId.IsValid() && Candidate.CandidateId != ChoiceId)
                    return false;
                Candidate.CandidateId = ChoiceId;
            }
        }
        if (!Candidate.CandidateId.IsValid())
        {
            do
            {
                Candidate.CandidateId = FGuid::NewGuid();
            } while (IndexById.Contains(Candidate.CandidateId));
        }
        if (IndexById.Contains(Candidate.CandidateId))
            return false;
        IndexById.Add(Candidate.CandidateId, Index);
        if (bItemAction)
        {
            Candidate.ItemChoice = MakeShared<FDeliveryCompanionItemChoice>(*Candidate.ItemChoice);
            Candidate.ItemChoice->CandidateId = Candidate.CandidateId;
            Candidate.ItemChoice->RequestId = Request.RequestId;
            Candidate.ItemChoice->Requester = Requester;
        }
        if (bHomeAction)
        {
            // 按值复制再绑定本次请求，不能改到组件缓存或另一次请求的共享快照
            Candidate.HomeChoice = MakeShared<FDeliveryCompanionHomeChoice>(*Candidate.HomeChoice);
            Candidate.HomeChoice->CandidateId = Candidate.CandidateId;
            Candidate.HomeChoice->RequestId = Request.RequestId;
            Candidate.HomeChoice->Requester = Requester;
        }
    }
    return !Request.Candidates.IsEmpty();
}
} // namespace

// 组件需要复制，每 0.1 秒 Tick 一次
UDeliveryNpcAgentComponent::UDeliveryNpcAgentComponent()
{
    SetIsReplicatedByDefault(true);
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickInterval = .1f;
}

// 服务器上生成 NPC 编号，并订阅 NPC 之间的接话事件
void UDeliveryNpcAgentComponent::BeginPlay()
{
    Super::BeginPlay();
    if (IsAuthority())
    {
        NpcId = FGuid::NewGuid();
        BindNpcMessageTurns();
    }
    else
        SetComponentTickEnabled(false);
}

// 是否在服务器上
bool UDeliveryNpcAgentComponent::IsAuthority() const
{
    return !bEndingPlay && GetOwner() && GetOwner()->HasAuthority() && GetWorld() && GetWorld()->GetNetMode() != NM_Client;
}

// 服务器时间，客户端按它对齐动画和声音
double UDeliveryNpcAgentComponent::ServerTime() const
{
    if (const auto* State = GetWorld()->GetGameState())
        return State->GetServerWorldTimeSeconds();
    return GetWorld()->GetTimeSeconds();
}

// 请求入队时建立诊断记录：事件、候选、记忆和当前活动
void UDeliveryNpcAgentComponent::BeginDecisionTrace(const FDeliveryNpcDecisionRequest& Request)
{
    if (!IsAuthority() || Request.NpcId != NpcId || !Request.RequestId.IsValid())
        return;
    const auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>();
    if (!Gateway || Gateway->GetWorldEpoch() != Request.WorldEpoch)
        return;
    const int32 PreviousRevision = DecisionTrace.Revision;
    DecisionTrace = {};
    DecisionTrace.Revision = PreviousRevision;
    DecisionTrace.WorldEpoch = Request.WorldEpoch;
    DecisionTrace.RequestId = Request.RequestId;
    DecisionTrace.NpcName = TraceText(Request.SelfName, 64);
    DecisionTrace.Stage = TEXT("Queued");
    TraceQueuedAt = FPlatformTime::Seconds();
    TraceDispatchedAt = -1;
    DecisionTrace.InputSummary = FString::Printf(TEXT("%s；游戏请求记录含 %d 条感知、%d 个已知对象、%d 个动作候选"),
                                                 Request.bPlayerRequest ? TEXT("玩家发起交谈") : TEXT("环境事件触发"),
                                                 Request.Evidence.Num(), Request.Subjects.Num(), Request.Candidates.Num());
    const auto* PlayerMessage = Request.Evidence.FindByPredicate(
        [&Request](const auto& Item)
        {
            return Request.bPlayerRequest && Item.Kind == TEXT("PlayerSpeech") && Item.EventId == Request.PlayerMessageId;
        });
    if (PlayerMessage)
        DecisionTrace.PlayerText = TraceText(PlayerMessage->Description, 256);
    for (int32 Index = Request.Evidence.Num() - 1; Index >= 0 && DecisionTrace.Observations.Num() < 5; --Index)
    {
        const auto& Item = Request.Evidence[Index];
        const TCHAR* Source = Item.Source == EDeliveryNpcEvidenceSource::Seen     ? TEXT("看到")
                              : Item.Source == EDeliveryNpcEvidenceSource::Heard  ? TEXT("听到")
                              : Item.Source == EDeliveryNpcEvidenceSource::ToldBy ? TEXT("被告知")
                                                                                  : TEXT("实际结果");
        FString Details;
        if (Item.bHasObservedLocation && FMath::IsFinite(Item.ObservedLocation.X) && FMath::IsFinite(Item.ObservedLocation.Y) &&
            FMath::IsFinite(Item.ObservedLocation.Z))
            Details = FString::Printf(TEXT("；位置 (%.0f, %.0f, %.0f) cm"), Item.ObservedLocation.X, Item.ObservedLocation.Y,
                                      Item.ObservedLocation.Z);
        if (FMath::IsFinite(Item.ObservedAt))
            Details += FString::Printf(TEXT("；观察时刻 %.1f 秒（本局时间）"), Item.ObservedAt);
        if (FMath::IsFinite(Item.ObservedAt) && FMath::IsFinite(Item.ExpiresAt) && Item.ExpiresAt >= Item.ObservedAt)
            Details += FString::Printf(TEXT("；时效 %.1f 秒"), Item.ExpiresAt - Item.ObservedAt);
        DecisionTrace.Observations.Add(TraceText(
            FString::Printf(TEXT("%s：%s%s"), Source, *TraceText(Item.Description, FMath::Max(0, 156 - Details.Len())), *Details),
            160));
    }
    for (const auto& Item : Request.Subjects)
    {
        if (DecisionTrace.Subjects.Num() >= 6)
            break;
        DecisionTrace.Subjects.Add(TraceText(Item.Name + TEXT(" · ") + Item.Kind, 160));
    }
    for (const auto& Item : Request.Candidates)
    {
        if (DecisionTrace.Candidates.Num() >= 12)
            break;
        const FString Target = TraceSubject(Request, Item.TargetId);
        // 时长来自 EncodeRequest 同一个候选字段，未列出的动作不编造拒绝原因
        DecisionTrace.Candidates.Add(
            TraceText(FString::Printf(TEXT("%s%s；时长上限 %.1f 秒；引用 %d 条证据"), *DeliveryNpc::ActionName(Item.Action),
                                      Target.IsEmpty() ? TEXT("") : *(TEXT(" → ") + Target),
                                      DeliveryNpc::CandidateDuration(Item.Action), Item.EvidenceIds.Num()),
                      160));
    }
    if (Request.bHasCompanionContext)
    {
        if (!Request.CompanionRelationshipStage.IsEmpty())
            DecisionTrace.Memory.Add(TraceText(TEXT("关系：") + Request.CompanionRelationshipStage, 160));
        if (!Request.CompanionMode.IsEmpty())
            DecisionTrace.Memory.Add(TraceText(TEXT("持续安排：") + Request.CompanionMode, 160));
        if (!Request.CompanionTaskTitle.IsEmpty())
            DecisionTrace.Memory.Add(TraceText(TEXT("当前任务：") + Request.CompanionTaskTitle, 160));
        // 只显示本轮实际使用的经历数量，不把个人经历全文同步给旁观玩家
        DecisionTrace.Memory.Add(FString::Printf(TEXT("本轮带入 %d 条当局经历"), Request.CompanionMemories.Num()));
    }
    if (Request.RunningActionId.IsValid())
    {
        DecisionTrace.CurrentActivity = TraceText(DeliveryNpc::ActionName(Request.RunningAction), 96);
        const FString Target = TraceSubject(Request, Request.RunningTargetId);
        if (!Target.IsEmpty())
            DecisionTrace.CurrentActivity = TraceText(DecisionTrace.CurrentActivity + TEXT(" → ") + Target, 160);
    }
    else if (Request.bRunningCompanion)
    {
        DecisionTrace.CurrentActivity = TraceText(Request.CompanionMode, 64);
        if (!Request.CompanionErrandAction.IsEmpty())
            DecisionTrace.CurrentActivity += TEXT(" · ") + TraceText(Request.CompanionErrandAction, 64);
        if (!Request.CompanionErrandPhase.IsEmpty())
            DecisionTrace.CurrentActivity += TEXT(" · ") + TraceText(Request.CompanionErrandPhase, 32);
        DecisionTrace.CurrentActivity = TraceText(DecisionTrace.CurrentActivity, 160);
    }
    else
        DecisionTrace.CurrentActivity = TEXT("当前没有执行中的动作");
    PublishDecisionTrace();
}

// 诊断记录是否属于这次请求和当前世界
bool UDeliveryNpcAgentComponent::IsDecisionTraceCurrent(FGuid WorldEpoch, FGuid RequestId) const
{
    if (!IsAuthority() || !RequestId.IsValid() || DecisionTrace.RequestId != RequestId || DecisionTrace.WorldEpoch != WorldEpoch)
        return false;
    const auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>();
    return Gateway && Gateway->GetWorldEpoch() == WorldEpoch;
}

// 增加修订号并通知订阅者
void UDeliveryNpcAgentComponent::PublishDecisionTrace()
{
    if (!IsAuthority() || !DecisionTrace.RequestId.IsValid())
        return;
    DecisionTrace.Revision = DecisionTrace.Revision == MAX_int32 ? 1 : DecisionTrace.Revision + 1;
    // 订阅期间即使开始新请求，本次通知仍是一份完整快照
    const FDeliveryNpcDecisionTrace Snapshot = DecisionTrace;
    OnDecisionTraceChanged.Broadcast(Snapshot);
}

// 记录请求阶段：发出、完成、失败、取消，并计算耗时
void UDeliveryNpcAgentComponent::TraceRequestEvent(FGuid WorldEpoch, FGuid RequestId, FName Stage, FName Error)
{
    if (!IsDecisionTraceCurrent(WorldEpoch, RequestId))
        return;
    if (Stage == TEXT("Requesting") && TraceDispatchedAt < 0)
        TraceDispatchedAt = FPlatformTime::Seconds();
    if ((Stage == TEXT("ReplyDone") || Stage == TEXT("Failed") || Stage == TEXT("Cancelled")) && TraceQueuedAt >= 0 &&
        DecisionTrace.TotalElapsedMs < 0)
        DecisionTrace.TotalElapsedMs = FMath::Max(0.0, (FPlatformTime::Seconds() - TraceQueuedAt) * 1000.0);
    DecisionTrace.Stage = Stage;
    if (!Error.IsNone() && DecisionTrace.RejectionReason.IsEmpty())
        DecisionTrace.RejectionReason = TraceText(Error.ToString(), 96);
    if (Stage == TEXT("ReplyDone"))
        DecisionTrace.ReplyStage = TEXT("TransferDone");
    PublishDecisionTrace();
}

// 记录后端返回的诊断：实际模型、路由、各段耗时和 token 数
void UDeliveryNpcAgentComponent::TraceDiagnostics(FGuid WorldEpoch, FGuid RequestId, const FDeliveryNpcDecisionTrace& Diagnostics)
{
    if (!IsDecisionTraceCurrent(WorldEpoch, RequestId))
        return;
    // 后端可能压缩输入，游戏请求中的位置和记录数量不代表最终模型实际读取的内容
    if (!Diagnostics.InputSummary.IsEmpty())
        DecisionTrace.InputSummary = TraceText(DecisionTrace.InputSummary + TEXT("；") + Diagnostics.InputSummary, 320);
    DecisionTrace.ActualModelName = Diagnostics.ActualModelName;
    DecisionTrace.ModelProvider = Diagnostics.ModelProvider;
    DecisionTrace.RequestSource = Diagnostics.RequestSource;
    DecisionTrace.Route = Diagnostics.Route;
    DecisionTrace.FallbackReason = Diagnostics.FallbackReason;
    DecisionTrace.BackendQueueMs = Diagnostics.BackendQueueMs;
    DecisionTrace.BackendDecisionMs = Diagnostics.BackendDecisionMs;
    DecisionTrace.BackendElapsedMs = Diagnostics.BackendElapsedMs;
    DecisionTrace.FirstSentenceMs = Diagnostics.FirstSentenceMs;
    DecisionTrace.InputTokens = Diagnostics.InputTokens;
    DecisionTrace.CachedInputTokens = Diagnostics.CachedInputTokens;
    DecisionTrace.OutputTokens = Diagnostics.OutputTokens;
    // 紧接着由 ModelDone 事件统一发布，避免同一回包重复通知
}

// 记录模型建议的动作和目标
void UDeliveryNpcAgentComponent::TraceProposal(const FDeliveryNpcDecisionProposal& Proposal)
{
    if (!IsDecisionTraceCurrent(ActiveRequest.WorldEpoch, ActiveRequest.RequestId))
        return;
    DecisionTrace.ProposedAction = TraceText(DeliveryNpc::ActionName(Proposal.Action), 64);
    DecisionTrace.ProposedTarget = TraceSubject(ActiveRequest, Proposal.TargetId);
    DecisionTrace.ReplyPreview = TraceText(Proposal.Speech, 256);
    if (!DecisionTrace.ReplyPreview.IsEmpty())
    {
        DecisionTrace.FirstReplyText = TraceText(Proposal.Speech, 120);
        DecisionTrace.ReplyStage = TEXT("Received");
    }
    if (TraceDispatchedAt >= 0 && DecisionTrace.DecisionElapsedMs < 0)
        DecisionTrace.DecisionElapsedMs = FMath::Max(0.0, (FPlatformTime::Seconds() - TraceDispatchedAt) * 1000.0);
    // 校验完成后一起发布，建议与执行许可始终分别保存
}

// 记录校验结果和拒绝原因
void UDeliveryNpcAgentComponent::TraceValidation(FGuid WorldEpoch, FGuid RequestId, bool bAccepted, FName Error)
{
    if (!IsDecisionTraceCurrent(WorldEpoch, RequestId))
        return;
    if (bAccepted)
    {
        DecisionTrace.ActionId = PublicState.ActionId;
        const bool bContinued = DecisionTrace.ProposedAction == TEXT("ContinueCurrent");
        DecisionTrace.ValidatedAction =
            bContinued ? TEXT("ContinueCurrent / 继续当前活动") : TraceText(DeliveryNpc::ActionName(PublicState.Action), 64);
        DecisionTrace.ExecutionResult = bContinued ? TEXT("保留当前活动，仅更新本次回复")
                                        : PublicState.Reason == TEXT("CompanionErrandContinued") ? TEXT("原委托继续执行")
                                        : PublicState.Reason == TEXT("CompanionErrandAccepted") ? TEXT("已接下委托，等待执行结果")
                                        : PublicState.Status == EDeliveryNpcActionStatus::Running   ? TEXT("执行中")
                                        : PublicState.Status == EDeliveryNpcActionStatus::Succeeded ? TEXT("动作已完成")
                                                                                                    : TEXT("校验通过，等待执行");
        DecisionTrace.Stage = TEXT("Decision");
    }
    else
    {
        DecisionTrace.RejectionReason = TraceText(Error.IsNone() ? TEXT("RejectedProposal") : Error.ToString(), 96);
        if (!DecisionTrace.ProposedAction.IsEmpty())
            DecisionTrace.ExecutionResult = TEXT("未执行，本次建议未通过校验");
        DecisionTrace.Stage = TEXT("Failed");
    }
    PublishDecisionTrace();
}

// 记录第一句台词
void UDeliveryNpcAgentComponent::TraceReplyText(FGuid RequestId, const FString& Text)
{
    if (!IsDecisionTraceCurrent(DecisionTrace.WorldEpoch, RequestId) || Text.IsEmpty())
        return;
    const FString Preview = TraceText(Text, 120);
    if (Preview.IsEmpty())
        return;
    if (DecisionTrace.FirstReplyText.IsEmpty())
        DecisionTrace.FirstReplyText = Preview;
    if (!DecisionTrace.ReplyPreview.Contains(Preview))
        DecisionTrace.ReplyPreview =
            TraceText(DecisionTrace.ReplyPreview.IsEmpty() ? Preview : DecisionTrace.ReplyPreview + TEXT(" ") + Preview, 256);
    DecisionTrace.ReplyStage = TEXT("Received");
    PublishDecisionTrace();
}

// 记录动作开始和结束
void UDeliveryNpcAgentComponent::TraceAction(FGuid ActionId, FName Stage)
{
    if (!ActionId.IsValid() || DecisionTrace.ActionId != ActionId ||
        !IsDecisionTraceCurrent(DecisionTrace.WorldEpoch, DecisionTrace.RequestId))
        return;
    DecisionTrace.Stage = Stage;
    const auto Status = PublicState.Status;
    const bool bFollowAccepted = Status == EDeliveryNpcActionStatus::Succeeded &&
                                 PublicState.Action == EDeliveryNpcAction::Follow &&
                                 PublicState.Reason == TEXT("CompanionModeChanged");
    DecisionTrace.ExecutionResult = Status == EDeliveryNpcActionStatus::Running ? TEXT("执行中")
                                    : bFollowAccepted                           ? TEXT("已接下指令，持续跟随")
                                    : PublicState.Reason == TEXT("CompanionErrandContinued") ? TEXT("原委托继续执行")
                                    : PublicState.Reason == TEXT("CompanionErrandAccepted") ? TEXT("已接下委托，等待执行结果")
                                    : Status == EDeliveryNpcActionStatus::Succeeded ? TEXT("动作已完成")
                                    : Status == EDeliveryNpcActionStatus::Cancelled ? TEXT("动作已取消")
                                                                                    : TEXT("动作失败");
    if (!PublicState.Reason.IsNone())
        DecisionTrace.RejectionReason = TraceText(PublicState.Reason.ToString(), 96);
    DecisionTrace.CurrentActivity =
        TraceText(DeliveryNpc::ActionName(PublicState.Action) + TEXT(" · ") + DecisionTrace.ExecutionResult, 160);
    PublishDecisionTrace();
}

// 删除过期事件和失效对象，正在被动作或回复引用的事件保留
void UDeliveryNpcAgentComponent::PruneEvidence()
{
    const double Now = GetWorld()->GetTimeSeconds();
    const auto* Companion = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
    const FGuid PendingAssistProof = Companion && !Companion->HasCommittedAssist() ? Companion->GetAssistEvidenceId() : FGuid();
    auto IsReferenced = [this](const FDeliveryNpcEvidence& Item)
    {
        return (Item.EventId == PendingJoinedEvent && IsJoinedResponseOwnerCurrent()) ||
               (ActiveRequest.RequestId.IsValid() && ActiveRequest.Evidence.ContainsByPredicate(
                                                         [&Item](const auto& Sent)
                                                         {
                                                             return Sent.EventId == Item.EventId;
                                                         })) ||
               (HasActiveAction() && ActionEvidenceIds.Contains(Item.EventId));
    };
    // 原定时限照常生效，引用保护只防止重复观察和容量淘汰删掉正在使用的记录
    bool bChanged = Evidence.RemoveAll(
                        [Now](const auto& Item)
                        {
                            return Item.ExpiresAt <= Now;
                        }) > 0;
    for (auto& Item : Evidence)
    {
        // 转头后仍记得刚才见过谁，但不再向模型声称对方仍在原地
        // 当前请求保留发送时的原描述，等这一轮结束后再标为历史观察
        if (IsSightMemory(Item) && Item.Kind != TEXT("LostSight") && Item.Kind != TEXT("LastSeen") &&
            Now - Item.ObservedAt >= 45.0 && !IsReferenced(Item))
        {
            Item.Kind = TEXT("LastSeen");
            Item.Description = TEXT("此前见过这个对象；记录的位置是当时所见，当前位置和行为未知");
            bChanged = true;
        }
    }
    TSet<FGuid> SeenSubjects;
    for (int32 Index = Evidence.Num() - 1; Index >= 0; --Index)
    {
        const auto& Item = Evidence[Index];
        if (!IsSightMemory(Item) || !Item.SubjectId.IsValid())
            continue;
        // 同一个人的定时视野更新只留最新一条，正在被请求或动作引用的编号暂时保留
        if (SeenSubjects.Contains(Item.SubjectId) && !IsReferenced(Item))
        {
            Evidence.RemoveAt(Index);
            bChanged = true;
        }
        else
            SeenSubjects.Add(Item.SubjectId);
    }
    while (Evidence.Num() > 32)
    {
        int32 RemoveIndex = INDEX_NONE;
        int32 OldestUnprotectedIndex = INDEX_NONE;
        int32 LowestPriority = 3;
        // 保留刚收到的新事件和当前待确认协战的唯一真实命中，避免旧请求占满证据后吞掉候选
        for (int32 Index = 0; Index < Evidence.Num() - 1; ++Index)
        {
            const auto& Item = Evidence[Index];
            if (PendingAssistProof.IsValid() && Item.EventId == PendingAssistProof)
                continue;
            if (OldestUnprotectedIndex == INDEX_NONE)
                OldestUnprotectedIndex = Index;
            if (IsReferenced(Item))
                continue;
            const int32 Priority = IsSightMemory(Item) ? 0 : IsConversationMemory(Item.Kind) ? 2 : 1;
            if (Priority < LowestPriority)
            {
                RemoveIndex = Index;
                LowestPriority = Priority;
            }
        }
        // 旧请求引用已经有发送副本；满额时从仍可清理的最早记录腾出一格
        Evidence.RemoveAt(RemoveIndex == INDEX_NONE ? OldestUnprotectedIndex : RemoveIndex);
        bChanged = true;
    }
    if (bChanged)
        ++ContextRevision;
    for (auto It = KnownSubjects.CreateIterator(); It; ++It)
    {
        if (!It.Value().IsValid() || !Evidence.ContainsByPredicate(
                                         [&It](const auto& Item)
                                         {
                                             return Item.SubjectId == It.Key() || Item.AddresseeId == It.Key();
                                         }))
            It.RemoveCurrent();
    }
}

// 服务器：把一条观察写成事件，相同编号只记一次
bool UDeliveryNpcAgentComponent::ObserveEvent(const FDeliveryNpcObservation& Observation)
{
    if (!IsAuthority() || Observation.Kind.IsNone() || !FMath::IsFinite(Observation.Lifetime) || Observation.Lifetime <= 0 ||
        (Observation.bHasObservedLocation && Observation.ObservedLocation.ContainsNaN()) ||
        (Observation.Subject && (!IsValid(Observation.Subject) || Observation.Subject->GetWorld() != GetWorld())) ||
        (Observation.Addressee && (!IsValid(Observation.Addressee) || Observation.Addressee->GetWorld() != GetWorld())))
        return false;
    const FGuid EventId = Observation.EventId.IsValid() ? Observation.EventId : FGuid::NewGuid();
    if (RecentEventIds.Contains(EventId))
        return false;
    PruneEvidence();

    // 填写事件：类型、来源、描述、有效期和相关对象
    FDeliveryNpcEvidence Item;
    Item.EventId = EventId;
    Item.ReplyToEventId = Observation.ReplyToEventId;
    Item.Kind = Observation.Kind;
    Item.Source = Observation.Source;
    Item.Description = Observation.Description.Left(512);
    Item.bNeedsReply = Observation.bNeedsReply;
    Item.ObservedAt = GetWorld()->GetTimeSeconds();
    // 刚说过的话和见过的人记住五分钟，碰撞、受击等现场事件仍使用各自的短时限
    const float Lifetime =
        IsConversationMemory(Item.Kind) || IsSightMemory(Item) ? 300.f : FMath::Clamp(Observation.Lifetime, .1f, 60.f);
    Item.ExpiresAt = Item.ObservedAt + Lifetime;
    Item.bHasObservedLocation = Observation.bHasObservedLocation;
    Item.ObservedLocation = Observation.bHasObservedLocation ? Observation.ObservedLocation : FVector::ZeroVector;
    auto RememberSubject = [this](AActor* Actor)
    {
        if (!Actor)
            return FGuid();
        if (Actor == GetOwner() && !NpcId.IsValid())
            NpcId = FGuid::NewGuid();
        for (const auto& Pair : KnownSubjects)
            if (Pair.Value == Actor)
                return Pair.Key;
        const FGuid SubjectId = Actor == GetOwner() ? NpcId : FGuid::NewGuid();
        KnownSubjects.Add(SubjectId, Actor);
        return SubjectId;
    };
    Item.SubjectId = RememberSubject(Observation.Subject);
    Item.AddresseeId = RememberSubject(Observation.Addressee);
    // 只有服务器已完成认主的真实结果才能安排主动回应
    if (Item.Kind == TEXT("CompanionJoined") && Item.Source == EDeliveryNpcEvidenceSource::ActionResult)
    {
        const auto* Npc = Cast<ADeliveryCompanionCharacter>(GetOwner());
        auto* OwnerState = Npc ? Npc->Companion->GetOwnerPlayerState() : nullptr;
        const auto* PC = IsValid(OwnerState) ? OwnerState->GetPlayerController() : nullptr;
        if (Npc && Npc->GetEncounterState().Phase == EDeliveryCompanionEncounterPhase::Joined && Observation.bNeedsReply &&
            IsValid(Observation.Subject) && Npc->Companion->CanCommand(OwnerState) && PC && PC->PlayerState == OwnerState &&
            PC->GetPawn() == Observation.Subject)
        {
            PendingJoinedEvent = EventId;
            PendingJoinedOwner = OwnerState;
            PendingJoinedSubject = PC->GetPawn();
            JoinedResponseExpiresAt = Item.ExpiresAt;
            JoinedResponseAttempts = 0;
            NextJoinedResponseRetryAt = GetWorld()->GetTimeSeconds() + 4;
            UE_LOG(LogDeliveryNpcAgent, Log, TEXT("CompanionJoined response pending npc=%s event=%s"), *GetNameSafe(Npc),
                   *EventId.ToString());
        }
    }

    // 保存事件，增加上下文版本号
    Evidence.Add(MoveTemp(Item));
    PruneEvidence();
    RecentEventIds.Add(EventId);
    if (RecentEventIds.Num() > 128)
        RecentEventIds.RemoveAt(0);
    ++ContextRevision;
    // 对手打中我或开始逃跑时，提早请模型判断，当前行动照常执行
    const bool bCombatChanged = Observation.Kind == TEXT("LightMeleeHit") || Observation.Kind == TEXT("FleeStarted");
    if (bCombatChanged && Observation.Source == EDeliveryNpcEvidenceSource::Seen && Observation.Subject == PublicState.Target &&
        CanUpdateRunningAction() && CombatEventDecisionAt < 0)
        CombatEventDecisionAt = FMath::Max(GetWorld()->GetTimeSeconds() + .25, LastCombatDecisionAt + 1.5);
    return true;
}

// 交谈流程第 10 步，服务器：把事件、对象和候选动作整理成一份请求
bool UDeliveryNpcAgentComponent::BuildDecisionRequest(FGuid WorldEpoch, FDeliveryNpcDecisionRequest& OutRequest)
{
    if (!IsAuthority() || bChangingPlan || !WorldEpoch.IsValid() ||
        (HasActiveAction() && !CanUpdateRunningAction() && !bPlayerRequest))
        return false;

    // 同伴安静期间不主动插话；玩家正在录音时暂缓
    auto* CompanionContext = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
    const int32 QuietRemaining = CompanionContext ? CompanionContext->GetQuietSecondsRemaining() : 0;
    // 主动插话暂停，已有交战的决策及直接提问仍走原入口
    if (QuietRemaining > 0 && !bPlayerRequest && !CanUpdateRunningAction() &&
        !(CompanionContext && CompanionContext->CanJoinAssist(nullptr)))
        return false;
    if (!bPlayerRequest && IsPlayerInputBeingRecorded())
        return false;
    if (!IsAuthority() || bChangingPlan)
        return false;

    // 把自己当前的战况写成一条 SelfCombat 事件
    if (const auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        // 每次请求只保存一份自己的当前战况，不把对方的私有数值发给模型
        Evidence.RemoveAll(
            [](const auto& Item)
            {
                return Item.Kind == TEXT("SelfCombat");
            });
        FDeliveryNpcObservation Body;
        Body.Kind = TEXT("SelfCombat");
        Body.Subject = GetOwner();
        Body.Source = EDeliveryNpcEvidenceSource::ActionResult;
        Body.Description = Npc->Combat->DescribeCombatState();
        Body.Lifetime = 30;
        if (CanUpdateRunningAction())
        {
            const auto* Other = Cast<ADeliveryNpcCharacter>(PublicState.Target);
            const FString TargetName = Other ? Other->DisplayName.ToString() : TEXT("对方");
            Body.Description +=
                FString::Printf(TEXT("；我正在对%s执行%s，剩余约%.1f秒。当前动作还未结束；下一步可以继续、逃跑或停手"),
                                *TargetName, *DeliveryNpc::ActionName(PublicState.Action), GetActionTimeRemaining());
        }
        ObserveEvent(Body);
    }
    if (CompanionContext)
    {
        CompanionContext->RefreshItemObservations();
        if (bPlayerRequest && PlayerReplyMessage.IsValid() && PlayerReplyController.IsValid())
            CompanionContext->RefreshCaterpillarObservationForOwnerRequest(PlayerReplyController->PlayerState);
    }
    PruneEvidence();
    if (!NpcId.IsValid())
        NpcId = FGuid::NewGuid();
    OutRequest = {};

    // 正在执行的动作：模型可以选择继续或更换
    OutRequest.RunningActionId = HasActiveAction() ? PublicState.ActionId : FGuid();
    if (OutRequest.RunningActionId.IsValid())
    {
        OutRequest.RunningAction = PublicState.Action;
        OutRequest.RunningTargetId = ActionTargetId;
        OutRequest.RunningSecondsRemaining = GetActionTimeRemaining();
        OutRequest.RunningPlanRevision = PublicState.PlanRevision;
        OutRequest.RunningTarget = PublicState.Target.Get();
        OutRequest.RunningRequester = PublicState.Requester.Get();
        OutRequest.RunningOrigin = PublicState.Origin;
        OutRequest.RunningStartTime = PublicState.ServerStartTime;
        OutRequest.RunningDuration = PublicState.Duration;
        OutRequest.RunningDeadline = ActionDeadline;
    }

    // 请求身份：世界编号、NPC 编号、请求编号、计划修订号，回复按这些编号核对
    OutRequest.WorldEpoch = WorldEpoch;
    OutRequest.NpcId = NpcId;
    OutRequest.RequestId = FGuid::NewGuid();
    OutRequest.ContextRevision = ContextRevision;
    OutRequest.PlanRevision = ++NextPlanRevision;
    OutRequest.Persona = Persona.Left(1024);
    OutRequest.bPlayerRequest = bPlayerRequest;
    if (bPlayerRequest)
        if (const auto* Locations = UDeliveryLocationRegistry::Get(this))
            Locations->CollectPublicNpcPlaces(OutRequest.FamiliarPlaces);
    RequestPlayerState = bPlayerRequest && PlayerReplyController.IsValid() ? PlayerReplyController->PlayerState : nullptr;
    OutRequest.RequesterPC = bPlayerRequest ? PlayerReplyController.Get() : nullptr;
    OutRequest.RequesterPlayerState = RequestPlayerState;
    OutRequest.PlayerMessageId = bPlayerRequest ? PlayerReplyMessage : FGuid();
    const auto* Settings = GetDefault<UDeliveryAiSettings>();
    OutRequest.bStreamSentenceAudio = Settings->bStreamReplies && Settings->bStreamSentenceAudio;
    OutRequest.StreamProtocolVersion = Settings->bStreamReplies ? 2 : 0;
    OutRequest.bPendingPlayerMessage = bPlayerRequest && PlayerReplyMessage.IsValid();
    OutRequest.CompanionQuietSecondsRemaining = QuietRemaining;
    OutRequest.SelfSubjectId = NpcId;
    OutRequest.SelfName = TEXT("NPC");
    if (const auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        OutRequest.Voice = Npc->Presentation->TtsVoice.ToString();
        OutRequest.SelfName = Npc->DisplayName.ToString();
        OutRequest.ConversationDepth = Npc->GetConversationDepth();
        if (!Npc->Profile.RowName.IsNone())
            OutRequest.ProfileId = Npc->Profile.RowName.ToString();
    }

    // 对象名单：自己和已感知的对象，模型只能用这些编号指代对象
    OutRequest.Subjects.Add({NpcId, OutRequest.SelfName.Left(64), TEXT("npc")});
    for (const auto& Pair : KnownSubjects)
    {
        const AActor* Actor = Pair.Value.Get();
        if (!Actor || Actor == GetOwner())
            continue;
        FDeliveryNpcSubject Subject;
        Subject.SubjectId = Pair.Key;
        if (const auto* Npc = Cast<ADeliveryNpcCharacter>(Actor))
        {
            Subject.Name = Npc->DisplayName.ToString();
            Subject.Kind = TEXT("npc");
        }
        else if (const auto* Pawn = Cast<APawn>(Actor); Pawn && Pawn->IsPlayerControlled())
        {
            const auto* Player = Pawn->GetPlayerState();
            Subject.Name = Player ? FString::Printf(TEXT("快递员%d"), Player->GetPlayerId()) : TEXT("快递员");
            Subject.Kind = TEXT("player");
        }
        else if (Actor->IsA<ADeliveryMotorbike>())
        {
            Subject.Name = TEXT("电瓶车");
            Subject.Kind = TEXT("vehicle");
        }
        else if (const auto* Item = Cast<ADeliveryHandheldItem>(Actor); Item && Item->GetItemComponent())
        {
            Subject.Name = Item->GetItemComponent()->DisplayName.ToString();
            Subject.Kind = TEXT("other");
            for (TCHAR& Character : Subject.Name)
                if (Character < 32)
                    Character = TEXT(' ');
            Subject.Name = Subject.Name.TrimStartAndEnd();
            if (Subject.Name.IsEmpty())
                Subject.Name = TEXT("道具");
        }
        else
        {
            Subject.Name = TEXT("附近的对象");
            Subject.Kind = TEXT("other");
        }
        Subject.Name = Subject.Name.Left(64);
        OutRequest.Subjects.Add(MoveTemp(Subject));
    }

    // 事件：见闻、听到的话、动作结果
    OutRequest.Evidence = Evidence;
    AttachNpcMessageContext(OutRequest);
    OutRequest.LastDecisionEventIds = LastDecisionEventIds;
    // 模型读过事件还不等于已经回应，下一次请求仍保留这条未说完的真实经历
    const FGuid JoinedEvent = GetJoinedResponseEvent(OutRequest);
    OutRequest.LastDecisionEventIds.Remove(JoinedEvent);
    auto* JoinedListener = JoinedEvent.IsValid() && !bPlayerRequest ? GetJoinedResponseListener() : nullptr;
    if (JoinedEvent.IsValid() && !bPlayerRequest)
    {
        if (JoinedResponseAttempts >= 3 || !CanHearJoinedResponse(JoinedListener))
        {
            // 远处保留服务端原事件，这轮自主决定只处理当前其它观察
            OutRequest.Evidence.RemoveAll(
                [JoinedEvent](const auto& Fact)
                {
                    return Fact.EventId == JoinedEvent;
                });
        }
        else
        {
            OutRequest.JoinedResponseEvent = JoinedEvent;
            for (auto& Subject : OutRequest.Subjects)
                if (KnownSubjects.FindRef(Subject.SubjectId).Get() == JoinedListener)
                {
                    OutRequest.JoinedOwnerSubjectId = Subject.SubjectId;
                    OutRequest.JoinedOwnerPawn = JoinedListener;
                    // 对话名单描述正在驾驶的主人，仍用真实车辆 Pawn 保存这轮收话对象
                    if (const auto* Bike = Cast<ADeliveryMotorbike>(JoinedListener))
                    {
                        auto* OwnerState = PendingJoinedOwner.Get();
                        const auto* Driver = Bike->GetDriver();
                        if (OwnerState && IsValid(Driver) && Bike->GetRegisteredPlayer() == OwnerState &&
                            Driver->GetCurrentVehicle() == Bike && Driver->GetTaskPlayerState() == OwnerState)
                        {
                            Subject.Kind = TEXT("player");
                            Subject.Name = FString::Printf(TEXT("快递员%d"), OwnerState->GetPlayerId());
                        }
                    }
                    break;
                }
        }
    }

    // 候选动作：先加不需要对象的，再对每个已感知对象逐个询问 CanChooseAction
    OutRequest.AllowedActions.Add(EDeliveryNpcAction::Wait);
    if (bPlayerRequest || !CanUpdateRunningAction())
    {
        for (const auto& Pair : KnownSubjects)
            if (CanLookAt(Pair.Value.Get()))
            {
                OutRequest.AllowedActions.Add(EDeliveryNpcAction::LookAt);
                break;
            }
    }
    if (auto* Resident = Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        auto AddCandidate = [&](EDeliveryNpcAction Action, FGuid TargetId, FGuid EvidenceId)
        {
            OutRequest.AllowedActions.AddUnique(Action);
            FDeliveryNpcCandidate Candidate;
            Candidate.Action = Action;
            Candidate.TargetId = TargetId;
            if (EvidenceId.IsValid())
                Candidate.EvidenceIds.Add(EvidenceId);
            OutRequest.Candidates.Add(Candidate);
        };
        for (auto Action : {EDeliveryNpcAction::Wait, EDeliveryNpcAction::LieDown, EDeliveryNpcAction::ReleaseVehicle,
                            EDeliveryNpcAction::Pace})
            if (CanChooseAction(Action, nullptr, RequestPlayerState.Get()))
                AddCandidate(Action, {}, {});
        if (bPlayerRequest && CanChooseAction(EDeliveryNpcAction::GetUp, nullptr, RequestPlayerState.Get()))
            AddCandidate(EDeliveryNpcAction::GetUp, {}, {});
        for (const auto& Pair : KnownSubjects)
        {
            AActor* Target = Pair.Value.Get();
            if (!CanLookAt(Target))
                continue;
            const int32 FactIndex = Evidence.FindLastByPredicate(
                [&](const auto& Item)
                {
                    return Item.SubjectId == Pair.Key;
                });
            if (FactIndex == INDEX_NONE)
                continue;
            const auto* Fact = &Evidence[FactIndex];
            for (auto Action : {EDeliveryNpcAction::LookAt, EDeliveryNpcAction::MoveTo, EDeliveryNpcAction::StrikeOnce,
                                EDeliveryNpcAction::HoldVehicle, EDeliveryNpcAction::MoveAside, EDeliveryNpcAction::Fight,
                                EDeliveryNpcAction::Flee})
                if (CanChooseAction(Action, Target, RequestPlayerState.Get()))
                    AddCandidate(Action, Pair.Key, Fact->EventId);
        }
        Resident->ExtendDecisionRequest(OutRequest, RequestPlayerState.Get());
    }
    if (bPlayerRequest)
    {
        // 玩家请求：附上同伴的持续安排，加入 ContinueCurrent 以保持原安排
        OutRequest.RequestedCapabilityVersion = FMath::Max(OutRequest.RequestedCapabilityVersion, 6);
        if (const auto* Companion = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>())
        {
            OutRequest.bRunningCompanion = HasCompanionActivity();
            OutRequest.bRunningErrand = Companion->HasActiveErrand();
            OutRequest.bRunningHome = Companion->HasActiveHomeArrangement();
            OutRequest.RunningCompanionMode = uint8(Companion->GetMode());
            OutRequest.RunningCompanionOwner = Companion->GetOwnerPlayerState();
            OutRequest.RunningHomeArrangement = Companion->GetHomeState().ArrangementId;
            OutRequest.RunningHomeProp = Companion->GetHomeState().CarriedProp.Get();
            OutRequest.RunningHeldItem = Companion->GetCarriedItem();
            OutRequest.RunningErrandItem = Companion->GetErrand().Item.Get();
            if (!OutRequest.RunningActionId.IsValid() && OutRequest.bRunningCompanion)
                OutRequest.RunningAction = OutRequest.bRunningErrand ? Companion->GetErrand().Action
                                           : OutRequest.bRunningHome ? EDeliveryNpcAction::HomeAction
                                           : Companion->GetMode() == EDeliveryCompanionMode::GoHome ? EDeliveryNpcAction::GoHome
                                           : Companion->GetMode() == EDeliveryCompanionMode::Stay   ? EDeliveryNpcAction::Stay
                                                                                                    : EDeliveryNpcAction::Follow;
            if (OutRequest.RunningAction == EDeliveryNpcAction::Pace && !OutRequest.bRequesterIsCompanionOwner)
            {
                // 访客仍可通过 ContinueCurrent 交谈，不能用 Wait 或注视换掉主人的踱步
                OutRequest.AllowedActions.Reset();
                OutRequest.Candidates.Reset();
            }
        }
        if (OutRequest.RunningActionId.IsValid() || OutRequest.bRunningCompanion)
        {
            FDeliveryNpcCandidate Continue;
            Continue.Action = EDeliveryNpcAction::ContinueCurrent;
            OutRequest.Candidates.Add(Continue);
            OutRequest.AllowedActions.AddUnique(Continue.Action);
            if (OutRequest.RequestedCapabilityVersion >= 8 && OutRequest.bPendingPlayerMessage && CompanionContext &&
                CompanionContext->CanChangeSpeechPreference(RequestPlayerState.Get()))
            {
                auto Quiet = Continue;
                Quiet.SpeechPreference = EDeliveryCompanionSpeechPreference::Quiet;
                OutRequest.Candidates.Add(MoveTemp(Quiet));
                if (QuietRemaining > 0)
                {
                    auto Normal = Continue;
                    Normal.SpeechPreference = EDeliveryCompanionSpeechPreference::Normal;
                    OutRequest.Candidates.Add(MoveTemp(Normal));
                }
            }
        }
    }

    // 按共享行为表过滤这个人物不能用的动作
    if (const auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>())
    {
        const auto* Resident = Cast<ADeliveryNpcCharacter>(GetOwner());
        Gateway->GetBehaviorCatalog().FilterModelOptions(OutRequest, Resident ? Resident->Profile.RowName : NAME_None);
    }
    // 配置移掉取物后，本轮也不发送悬空线索或仅因线索登记的主体
    TSet<FGuid> RemovedLeadSubjects;
    OutRequest.Evidence.RemoveAll(
        [&](const FDeliveryNpcEvidence& Fact)
        {
            if ((Fact.Kind != DeliveryCompanionItemSearch::EvidenceKind && Fact.Kind != TEXT("CompanionOwnerItemAvailable")) ||
                OutRequest.Candidates.ContainsByPredicate(
                    [&Fact](const FDeliveryNpcCandidate& Candidate)
                    {
                        return Candidate.EvidenceIds.Contains(Fact.EventId);
                    }))
                return false;
            RemovedLeadSubjects.Add(Fact.SubjectId);
            return true;
        });
    if (!RemovedLeadSubjects.IsEmpty())
        OutRequest.Subjects.RemoveAll(
            [&](const FDeliveryNpcSubject& Subject)
            {
                return RemovedLeadSubjects.Contains(Subject.SubjectId) && !KnownSubjects.Contains(Subject.SubjectId);
            });

    // 给每个候选分配编号
    return PrepareCandidateIdentities(OutRequest, RequestPlayerState.Get());
}

// 交谈流程第 9 步，服务器：整理请求并交给网关排队
bool UDeliveryNpcAgentComponent::RequestDecision()
{
    if (!IsAuthority() || bChangingPlan)
        return false;
    // 录音和等待转写期间保留真实事件与 BT 活动，不把暂缓当成后台失败
    if (!bPlayerRequest && IsPlayerInputBeingRecorded())
        return false;
    if (!IsAuthority() || bChangingPlan)
        return false;

    // 已有请求在等回复：记下有新情况，当前请求结束后再处理
    if (IsDecisionPending())
    {
        bDecisionQueued = true;
        return true;
    }
    if (HasActiveAction() && !CanUpdateRunningAction() && !bPlayerRequest)
        return false;

    // 整理请求后入队
    auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>();
    const bool bCombatRequest = CanUpdateRunningAction();
    const TWeakObjectPtr<ADeliveryNpcCharacter> Resident = Cast<ADeliveryNpcCharacter>(GetOwner());
    const uint64 WakeSerial = Resident.IsValid() ? Resident->DecisionWakeSerial : 0;
    if (!Gateway || !BuildDecisionRequest(Gateway->GetWorldEpoch(), ActiveRequest))
        return false;
    const FGuid QueuedRequest = ActiveRequest.RequestId;
    // 入队诊断会同步通知监听者，期间可继续保存事实或替换这次请求
    bDecisionPending = true;
    if (!Gateway->Enqueue(this, ActiveRequest))
    {
        ActiveRequest = {};
        bDecisionPending = false;
        return false;
    }
    if (!IsAuthority() || ActiveRequest.RequestId != QueuedRequest)
        return IsDecisionPending();
    if (auto* Npc = Resident.Get())
        Npc->ConfirmDecisionRequestQueued(WakeSerial);
    if (bCombatRequest)
    {
        LastCombatDecisionAt = GetWorld()->GetTimeSeconds();
        CombatEventDecisionAt = -1;
    }
    if (!ActiveRequest.bPlayerRequest && GetJoinedResponseEvent(ActiveRequest).IsValid())
    {
        ++JoinedResponseAttempts;
        NextJoinedResponseRetryAt = GetWorld()->GetTimeSeconds() + 4 * JoinedResponseAttempts;
    }
    GetOwner()->ForceNetUpdate();
    return true;
}

// 规则动作引用的受击事件是否仍然有效
bool UDeliveryNpcAgentComponent::HasGameplayRuleEvidence(AActor* Target, FGuid EvidenceId) const
{
    if (!IsValid(Target) || Target == GetOwner() || Target->GetWorld() != GetWorld() || !EvidenceId.IsValid())
        return false;
    // 已确认协战沿用服务器保存的真实命中编号，不延长或伪造感知事件
    if (const auto* Companion = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
        Companion && Companion->HasCommittedAssist())
        return Companion->IsAssistEvidenceCurrent(Target, EvidenceId);
    const double Now = GetWorld()->GetTimeSeconds();
    return Evidence.ContainsByPredicate(
        [this, Target, EvidenceId, Now](const auto& Item)
        {
            return Item.EventId == EvidenceId && Item.Source == EDeliveryNpcEvidenceSource::ActionResult &&
                   Item.ExpiresAt > Now && Item.SubjectId.IsValid() && KnownSubjects.FindRef(Item.SubjectId).Get() == Target;
        });
}

// 当前规则动作的目标和证据是否仍然有效
bool UDeliveryNpcAgentComponent::IsGameplayRuleActionValid() const
{
    const auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    const auto* Companion = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
    const bool bCommittedTarget = Companion && Companion->HasCommittedAssist() && ActionEvidenceIds.Num() == 1 &&
                                  Companion->IsAssistEvidenceCurrent(PublicState.Target, ActionEvidenceIds[0]);
    return Npc && PublicState.Origin == EDeliveryNpcActionOrigin::GameplayRule &&
           PublicState.Action == EDeliveryNpcAction::Fight && PublicState.RuleRevision > 0 &&
           FMath::IsFinite(PublicState.Duration) && PublicState.Duration >= UDeliveryNpcCombatComponent::StrikeDuration &&
           PublicState.Duration <= 5.f && ActionEvidenceIds.Num() == 1 &&
           (bCommittedTarget || KnownSubjects.FindRef(ActionTargetId).Get() == PublicState.Target) &&
           HasGameplayRuleEvidence(PublicState.Target, ActionEvidenceIds[0]) &&
           Npc->CanExecuteGameplayRule(PublicState.Action, PublicState.Target, PublicState.RuleRevision);
}

// 服务器规则安排一轮短时交战，例如同伴护主，不经过模型
bool UDeliveryNpcAgentComponent::RequestGameplayRuleAction(EDeliveryNpcAction Action, AActor* Target, int32 RuleRevision,
                                                           FGuid EvidenceId, float Duration)
{
    if (!IsAuthority() || bChangingPlan || Action != EDeliveryNpcAction::Fight || RuleRevision <= 0 || !IsValid(Target) ||
        Target == GetOwner() || Target->GetWorld() != GetWorld() || !FMath::IsFinite(Duration) ||
        Duration < UDeliveryNpcCombatComponent::StrikeDuration || Duration > 5.f)
        return false;
    auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || !Npc->CanExecuteGameplayRule(Action, Target, RuleRevision))
        return false;
    PruneEvidence();
    if (!HasGameplayRuleEvidence(Target, EvidenceId))
        return false;
    // 同一轮仍在执行时不重置拳数或时间；倒地后可凭仍有效的规则重新开始
    if (HasActiveAction() && PublicState.Origin == EDeliveryNpcActionOrigin::GameplayRule &&
        PublicState.RuleRevision == RuleRevision)
        return false;

    TGuardValue<bool> ChangingPlan(bChangingPlan, true);
    // 规则打断模型时结束原玩家等待，随后复用原取消路径清掉声音和旧动作
    CancelDecision();
    FinishFailedPlayerReply(PlayerReplyMessage, nullptr, false);
    CancelPlan(TEXT("GameplayRule"));
    // 停止旧能力会发出通知，归属、目标或证据可能在通知中失效
    if (!IsAuthority() || !HasGameplayRuleEvidence(Target, EvidenceId) ||
        !Npc->CanExecuteGameplayRule(Action, Target, RuleRevision))
        return false;
    const auto* Fact = Evidence.FindByPredicate(
        [EvidenceId](const auto& Item)
        {
            return Item.EventId == EvidenceId;
        });
    FGuid SubjectId = Fact ? Fact->SubjectId : FindKnownSubjectId(Target);
    if (!SubjectId.IsValid())
    {
        // 感知记录可以正常过期，持续协战仍引用同一个服务器目标
        SubjectId = FGuid::NewGuid();
        KnownSubjects.Add(SubjectId, Target);
    }
    PublicState = {};
    PublicState.ActionId = FGuid::NewGuid();
    PublicState.PlanRevision = ++NextPlanRevision;
    PublicState.Action = Action;
    PublicState.Origin = EDeliveryNpcActionOrigin::GameplayRule;
    PublicState.RuleRevision = RuleRevision;
    PublicState.Status = EDeliveryNpcActionStatus::Accepted;
    PublicState.Target = Target;
    PublicState.Duration = Duration;
    ActionEvidenceIds = {EvidenceId};
    ActionTargetId = SubjectId;
    ActionConversationDepth = 0;
    ActionDeadline = GetWorld()->GetTimeSeconds() + 1.0;
    // 不改 LastDecisionEventIds，规则执行不能冒充模型已读过这些事件
    const FGuid AcceptedAction = PublicState.ActionId;
    UE_LOG(LogDeliveryNpcAgent, Log, TEXT("GameplayRule npc=%s action=%s revision=%d evidence=%s"), *NpcId.ToString(),
           *AcceptedAction.ToString(), RuleRevision, *EvidenceId.ToString());
    PublishState();
    return IsAuthority() && PublicState.ActionId == AcceptedAction && HasActiveAction();
}

// 居民被玩家或玩家的同伴打中后，不等模型立即还手
bool UDeliveryNpcAgentComponent::RequestRetaliation(AActor* Attacker)
{
    auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    // 只对玩家和玩家的同伴立即还手；居民之间互殴仍由各自的模型决定，否则两人会一来一往打个不停
    // 被车撞倒时不对车辆挥拳
    if (!IsAuthority() || bChangingPlan || !Npc || !IsValid(Attacker) || Attacker == GetOwner() ||
        Attacker->GetWorld() != GetWorld() ||
        (!Attacker->IsA<ADeliveryCharacter>() && !Attacker->IsA<ADeliveryCompanionCharacter>()))
        return false;
    PruneEvidence();
    const double Now = GetWorld()->GetTimeSeconds();
    // 取这个人最近一次真实打中我的记录，过期或没有记录时不还手
    const auto FindHitFrom = [this, Now](const AActor* Actor)
    {
        const FDeliveryNpcEvidence* Latest = nullptr;
        for (const auto& Item : Evidence)
            if ((Item.Kind == TEXT("LightMeleeHit") || Item.Kind == TEXT("Hit")) &&
                Item.Source == EDeliveryNpcEvidenceSource::Seen && Item.ExpiresAt > Now && Item.SubjectId.IsValid() &&
                KnownSubjects.FindRef(Item.SubjectId).Get() == Actor)
                Latest = &Item;
        return Latest;
    };

    // 必须找到这个人真实打中我的记录
    const auto* Fact = FindHitFrom(Attacker);
    if (!Fact)
        return false;
    if (HasActiveAction())
    {
        if (PublicState.Origin == EDeliveryNpcActionOrigin::GameplayRule || PublicState.Action == EDeliveryNpcAction::Flee)
            return false;
        // 已经在追打动过手的人时不来回换目标
        if (PublicState.Action == EDeliveryNpcAction::Fight && IsValid(PublicState.Target) &&
            (PublicState.Target == Attacker || FindHitFrom(PublicState.Target)))
            return false;
    }
    const FGuid EvidenceId = Fact->EventId;
    const FGuid SubjectId = Fact->SubjectId;

    TGuardValue<bool> ChangingPlan(bChangingPlan, true);
    // 换目标时省去上一轮交战的冷却，随后复用原取消路径停掉旧对白和旧动作
    TGuardValue<bool> ReplacingFight(bReplacingFight, true);
    CancelDecision();
    FinishFailedPlayerReply(PlayerReplyMessage, nullptr, false);
    CancelPlan(TEXT("Retaliation"));
    if (!IsAuthority() || !IsValid(Npc) || !IsValid(Attacker) || HasActiveAction())
        return false;
    Npc->Combat->AllowImmediateFight();
    // 停止旧动作会发出通知，记录或目标可能在通知中失效；对方太远或被挡住时留给模型判断
    if (!Evidence.ContainsByPredicate(
            [EvidenceId](const auto& Item)
            {
                return Item.EventId == EvidenceId;
            }) ||
        KnownSubjects.FindRef(SubjectId).Get() != Attacker ||
        !Npc->CanExecuteForRequester(EDeliveryNpcAction::Fight, Attacker, nullptr))
        return false;

    // 写入一轮交战，来源记为模型，开打后模型照常接手
    PublicState = {};
    PublicState.ActionId = FGuid::NewGuid();
    PublicState.PlanRevision = ++NextPlanRevision;
    PublicState.Action = EDeliveryNpcAction::Fight;
    // 沿用模型交战的执行与续判流程，开打后模型照常收到这次受击并决定下一步
    PublicState.Origin = EDeliveryNpcActionOrigin::Model;
    PublicState.Status = EDeliveryNpcActionStatus::Accepted;
    PublicState.Target = Attacker;
    PublicState.Duration = DeliveryNpc::FightDecisionSeconds;
    PublicState.Emotion = TEXT("Angry");
    ActionEvidenceIds = {EvidenceId};
    ActionTargetId = SubjectId;
    ActionConversationDepth = 0;
    ActionEndsAt = 0;
    CombatDeadline = 0;
    ActionDeadline = Now + 1.0;
    // 动手的同时马上请模型接话，质问、继续追打或收手的台词仍由模型给出
    bRetaliationNeedsReply = true;
    // 不改 LastDecisionEventIds，还手不能冒充模型已读过这次受击
    const FGuid AcceptedAction = PublicState.ActionId;
    UE_LOG(LogDeliveryNpcAgent, Log, TEXT("Retaliation npc=%s action=%s target=%s evidence=%s"), *NpcId.ToString(),
           *AcceptedAction.ToString(), *GetNameSafe(Attacker), *EvidenceId.ToString());
    PublishState();
    return IsAuthority() && PublicState.ActionId == AcceptedAction && HasActiveAction();
}

// 停止等待当前回复，取消网关中的请求
void UDeliveryNpcAgentComponent::CancelDecision()
{
    if (!IsAuthority())
        return;
    InvalidateNpcMessageTurns(TEXT("DecisionCancelled"));
    if (!IsAuthority())
        return;
    TArray<APlayerController*> Players;
    if (PlayerReplyController.IsValid())
        Players.AddUnique(PlayerReplyController.Get());
    if (ActiveRequest.RequesterPC.IsValid())
        Players.AddUnique(ActiveRequest.RequesterPC.Get());
    for (const auto& Pair : Replies)
        if (Pair.Value.RequesterPC.IsValid())
            Players.AddUnique(Pair.Value.RequesterPC.Get());
    for (auto* Player : Players)
        InterruptPlayerReply(Player);
    if (auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>())
        Gateway->Cancel(this);
    ActiveRequest = {};
    RequestPlayerState.Reset();
    bDecisionPending = false;
    bDecisionQueued = false;
    Replies.Reset();
}

// 同伴的初遇回应是否仍属于当前主人
bool UDeliveryNpcAgentComponent::IsJoinedResponseOwnerCurrent() const
{
    const auto* Npc = Cast<ADeliveryCompanionCharacter>(GetOwner());
    return IsAuthority() && PendingJoinedEvent.IsValid() && PendingJoinedOwner.IsValid() && Npc &&
           !Npc->IsActorBeingDestroyed() && Npc->GetEncounterState().Phase == EDeliveryCompanionEncounterPhase::Joined &&
           Npc->Companion->GetOwnerPlayerState() == PendingJoinedOwner.Get() &&
           Npc->Companion->CanCommand(PendingJoinedOwner.Get());
}

// 本次请求是否需要回应初遇事件
FGuid UDeliveryNpcAgentComponent::GetJoinedResponseEvent(const FDeliveryNpcDecisionRequest& Request) const
{
    if (!IsJoinedResponseOwnerCurrent() || Request.ParentNpcMessage.IsValid() ||
        GetWorld()->GetTimeSeconds() >= JoinedResponseExpiresAt)
        return {};
    // 主人插话时把救助经历交给同一回复，旁人的交谈不结清主人的初遇回应
    if (Request.bPlayerRequest &&
        (!Request.bPendingPlayerMessage || !Request.PlayerMessageId.IsValid() || !Request.RequesterPC.IsValid() ||
         !Request.RequesterPlayerState.HasSameIndexAndSerialNumber(PendingJoinedOwner) ||
         Request.RequesterPC->PlayerState != PendingJoinedOwner.Get()))
        return {};
    return Request.Evidence.ContainsByPredicate(
               [this](const auto& Fact)
               {
                   return Fact.EventId == PendingJoinedEvent && Fact.Kind == TEXT("CompanionJoined") &&
                          Fact.Source == EDeliveryNpcEvidenceSource::ActionResult;
               })
               ? PendingJoinedEvent
               : FGuid();
}

// 初遇回应的对象：主人当前控制的角色
APawn* UDeliveryNpcAgentComponent::GetJoinedResponseListener() const
{
    if (!IsJoinedResponseOwnerCurrent())
        return nullptr;
    auto* PC = PendingJoinedOwner->GetPlayerController();
    auto* Listener = IsValid(PC) && PC->PlayerState == PendingJoinedOwner.Get() ? PC->GetPawn<APawn>() : nullptr;
    return IsValid(Listener) && !Listener->IsActorBeingDestroyed() && Listener->GetWorld() == GetWorld() &&
                   Listener->GetController() == PC
               ? Listener
               : nullptr;
}

// 主人是否在能听到回应的范围内
bool UDeliveryNpcAgentComponent::CanHearJoinedResponse(const APawn* Listener) const
{
    const auto* Npc = GetOwner();
    return IsValid(Npc) && IsValid(Listener) && Npc->GetWorld() == Listener->GetWorld() &&
           FVector::DistSquared(Npc->GetActorLocation(), Listener->GetActorLocation()) <=
               FMath::Square(DeliveryNpcConversationTrace::LocalConversationRangeCm) &&
           DeliveryNpcConversationTrace::HasClearSpeechPath(Listener, Npc);
}

// 模型选择的说话对象是否就是主人
bool UDeliveryNpcAgentComponent::IsJoinedReplyTarget(const FDeliveryNpcDecisionRequest& Request, FGuid SubjectId,
                                                     const APawn& Listener) const
{
    const auto* Selected = KnownSubjects.FindRef(SubjectId).Get();
    if (!IsValid(Selected) || Selected->IsActorBeingDestroyed() || Selected->GetWorld() != GetWorld() ||
        !Request.Subjects.ContainsByPredicate(
            [SubjectId](const auto& Subject)
            {
                return Subject.SubjectId == SubjectId;
            }))
        return false;
    if (Selected == &Listener)
        return true;

    // 旧编号必须属于本轮封存的主人身体，或原来亲手救起二狗的那个主体
    const bool bWasListener = SubjectId == Request.JoinedOwnerSubjectId && Selected == Request.JoinedOwnerPawn.Get();
    const bool bOriginalRescuer = Selected == PendingJoinedSubject.Get() && Request.Evidence.ContainsByPredicate(
                                                                                [&](const auto& Fact)
                                                                                {
                                                                                    return Fact.EventId == PendingJoinedEvent &&
                                                                                           Fact.SubjectId == SubjectId;
                                                                                });
    if (!bWasListener && !bOriginalRescuer)
        return false;

    const auto* OwnerState = PendingJoinedOwner.Get();
    if (const auto* Bike = Cast<ADeliveryMotorbike>(&Listener))
    {
        // 当前车必须仍是旧步行角色实际正在驾驶的登记车辆
        const auto* Driver = Cast<ADeliveryCharacter>(Selected);
        return OwnerState && Driver && Bike->GetDriver() == Driver && Driver->GetCurrentVehicle() == Bike &&
               Driver->GetTaskPlayerState() == OwnerState && Bike->GetRegisteredPlayer() == OwnerState;
    }
    const auto* Character = Cast<ADeliveryCharacter>(&Listener);
    const auto* Bike = Cast<ADeliveryMotorbike>(Selected);
    const auto* Summon = Character ? Character->FindComponentByClass<UDeliveryVehicleSummonComponent>() : nullptr;
    return OwnerState && Character && Character->GetTaskPlayerState() == OwnerState && Bike && !Bike->GetDriver() &&
           !Bike->GetController() && Bike->GetRegisteredPlayer() == OwnerState && Summon &&
           Summon->GetRegisteredVehicle() == Bike;
}

// 把初遇事件关联到这一轮回复
void UDeliveryNpcAgentComponent::BindJoinedResponse(FReply& Reply, const FDeliveryNpcDecisionRequest& Request) const
{
    const FGuid Event = GetJoinedResponseEvent(Request);
    if (!Event.IsValid())
        return;
    // 仍由模型选说话对象，给其他人或自言自语的台词不能代替对主人的回应
    const auto* Listener = GetJoinedResponseListener();
    if (!Listener || PublicState.SpeechTarget != Listener)
        return;
    Reply.JoinedEventId = Event;
    Reply.JoinedOwner = PendingJoinedOwner;
}

// 主人插话时推迟初遇回应，6 秒后再试
void UDeliveryNpcAgentComponent::DeferJoinedResponse()
{
    if (!IsAuthority() || !PendingJoinedEvent.IsValid())
        return;
    const TWeakObjectPtr<UDeliveryNpcAgentComponent> WeakThis(this);
    NextJoinedResponseRetryAt = GetWorld()->GetTimeSeconds() + 6;
    const bool bJoinedRequest = !ActiveRequest.bPlayerRequest && !ActiveRequest.ParentNpcMessage.IsValid() &&
                                ActiveRequest.Evidence.ContainsByPredicate(
                                    [this](const auto& Fact)
                                    {
                                        return Fact.EventId == PendingJoinedEvent;
                                    });
    const FGuid DeferredRequest = bJoinedRequest ? ActiveRequest.RequestId : FGuid();
    TArray<FGuid> DeferredReplies;
    for (const auto& Pair : Replies)
        if (Pair.Value.JoinedEventId.IsValid())
            DeferredReplies.Add(Pair.Value.ReplyId);
    // 先撤下旧请求，再取消声音，取消回调不能恢复旧初遇回复
    if (DeferredRequest.IsValid())
    {
        ActiveRequest = {};
        bDecisionPending = false;
        bDecisionQueued = false;
        RequestPlayerState.Reset();
        if (auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>())
            Gateway->CancelRequest(this, DeferredRequest);
    }
    if (!WeakThis.IsValid() || !IsAuthority())
        return;
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
        Npc->Presentation->CancelReplies(DeferredReplies);
    if (!WeakThis.IsValid() || !IsAuthority())
        return;
    for (const FGuid& Reply : DeferredReplies)
    {
        RemoveReply(Reply, TEXT("PlayerInterruptedJoining"));
        if (!WeakThis.IsValid() || !IsAuthority())
            return;
    }
    if (DeferredReplies.Contains(PublicState.GetReplyId()))
    {
        PublicState.Speech.Reset();
        PublicState.ReplyToEventId.Invalidate();
        PublicState.bStreamSpeech = false;
        PublishState();
    }
}

// 同伴是否有持续安排：跟随、跑腿、家居等
bool UDeliveryNpcAgentComponent::HasCompanionActivity() const
{
    const auto* Companion = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
    // 等候仍要回位和避让，聊天继续保留主人的持续安排
    return Companion &&
           (Companion->HasActiveErrand() || Companion->HasActiveHomeArrangement() ||
            Companion->GetMode() == EDeliveryCompanionMode::Follow || Companion->GetMode() == EDeliveryCompanionMode::Stay ||
            Companion->GetMode() == EDeliveryCompanionMode::GoHome);
}

// 聊天类动作期间，行为树能否继续执行同伴的持续安排
bool UDeliveryNpcAgentComponent::CanContinueCompanionMovement() const
{
    if (!IsAuthority() || !HasActiveAction() || PublicState.Origin != EDeliveryNpcActionOrigin::Model)
        return false;
    const auto* Npc = Cast<ADeliveryCompanionCharacter>(GetOwner());
    const auto* Companion = Npc ? Npc->Companion.Get() : nullptr;
    if (!Companion || Npc->IsKnockedDown())
        return false;
    if (DeliveryNpc::IsCompanionErrand(PublicState.Action))
        return Companion->CanKeepErrandCommand(PublicState);
    if (PublicState.Action == EDeliveryNpcAction::Follow)
        return Companion->CanKeepFollowCommand(PublicState.Requester, PublicState.CompanionCommandRevision,
                                               PublicState.CompanionTravelMode);
    // 跟随或等候自己处理轻击暂停，已经接受的被动对白不接管走路或下车
    const bool bFollowHandlesHit =
        !Companion->HasActiveErrand() && !Companion->HasActiveHomeArrangement() &&
        (Companion->GetMode() == EDeliveryCompanionMode::Follow || Companion->GetMode() == EDeliveryCompanionMode::Stay);
    return (PublicState.Action == EDeliveryNpcAction::Wait || PublicState.Action == EDeliveryNpcAction::LookAt ||
            PublicState.Action == EDeliveryNpcAction::SoundFollowHorn) &&
           (!Npc->Combat->IsReactingToHit() || bFollowHandlesHit) && Companion->CanCommand(Companion->GetOwnerPlayerState()) &&
           (Companion->HasActiveErrand() || Companion->HasActiveHomeArrangement() ||
            Companion->GetMode() == EDeliveryCompanionMode::Follow || Companion->GetMode() == EDeliveryCompanionMode::Stay ||
            Companion->GetMode() == EDeliveryCompanionMode::GoHome);
}

// 是否需要停下来听玩家说话
bool UDeliveryNpcAgentComponent::ShouldPauseForConversation() const
{
    return IsListening() && !HasActiveAction() && !HasCompanionActivity();
}

// 是否正在回复这位玩家
bool UDeliveryNpcAgentComponent::IsPlayerReplyActive(APlayerController* Requester) const
{
    return GetReplyMessage(Requester).IsValid();
}

// 新回复能否替换还没开始播放的旧回复
bool UDeliveryNpcAgentComponent::CanReplacePendingReply(FGuid ExistingReplyId, FGuid NewReplyId) const
{
    const FReply* Existing = nullptr;
    const FReply* Replacement = nullptr;
    for (const auto& Pair : Replies)
    {
        if (Pair.Value.ReplyId == ExistingReplyId)
            Existing = &Pair.Value;
        if (Pair.Value.ReplyId == NewReplyId)
            Replacement = &Pair.Value;
    }
    return Existing && Replacement && Existing->RequesterPC.HasSameIndexAndSerialNumber(Replacement->RequesterPC) &&
           Existing->RequesterPlayerState.HasSameIndexAndSerialNumber(Replacement->RequesterPlayerState);
}

// 这一轮回复能否开始播放
bool UDeliveryNpcAgentComponent::CanStartReplySpeech(FGuid ReplyId) const
{
    for (const auto& Pair : Replies)
        if (Pair.Value.ReplyId == ReplyId)
        {
            if (!Pair.Value.bPlayerReply && Pair.Value.JoinedEventId.IsValid() &&
                (!CanHearJoinedResponse(GetJoinedResponseListener()) || GetWorld()->GetTimeSeconds() >= JoinedResponseExpiresAt ||
                 Pair.Value.JoinedEventId != PendingJoinedEvent ||
                 !Pair.Value.JoinedOwner.HasSameIndexAndSerialNumber(PendingJoinedOwner)))
                return false;
            if (!CanStartReplyMessageAudio(ReplyId))
                return false;
            // 可以提前收流。拥有同伴指令的回复等服务器确认接受安排后才开口
            if (Pair.Value.RequiresCompanionCommandReceipt())
                return Pair.Value.bActionStarted;
            return Pair.Value.bActionStarted ||
                   (Pair.Value.ActionId == PublicState.ActionId && (PublicState.Status == EDeliveryNpcActionStatus::Running ||
                                                                    PublicState.Status == EDeliveryNpcActionStatus::Succeeded));
        }
    if (PublicState.GetReplyId() == ReplyId && PublicState.ActionId == ReplyId &&
        DeliveryNpc::IsCompanionCommand(PublicState.Action))
        return PublicState.Status == EDeliveryNpcActionStatus::Succeeded;
    return PublicState.GetReplyId() != ReplyId || PublicState.Status == EDeliveryNpcActionStatus::Running ||
           PublicState.Status == EDeliveryNpcActionStatus::Succeeded;
}

// 播放时的收听对象，初遇回应固定为主人
AActor* UDeliveryNpcAgentComponent::GetReplyListenerForPlayback(FGuid ReplyId, AActor* Fallback) const
{
    for (const auto& Pair : Replies)
    {
        const auto& Reply = Pair.Value;
        if (Reply.ReplyId != ReplyId || Reply.bPlayerReply || !Reply.JoinedEventId.IsValid())
            continue;
        // 已排队的台词沿原声音播放，送达记录使用当前仍受原主人控制的身体或车辆
        return Reply.JoinedEventId == PendingJoinedEvent && Reply.JoinedOwner.HasSameIndexAndSerialNumber(PendingJoinedOwner)
                   ? GetJoinedResponseListener()
                   : nullptr;
    }
    return Fallback;
}

// 正在回复这位玩家的哪一句话
FGuid UDeliveryNpcAgentComponent::GetReplyMessage(APlayerController* Requester) const
{
    if (!IsValid(Requester))
        return {};
    if (PlayerReplyController.Get() == Requester && PlayerReplyMessage.IsValid())
        return PlayerReplyMessage;
    if (ActiveRequest.RequesterPC.Get() == Requester)
        return ActiveRequest.PlayerMessageId;
    const FReply* Latest = nullptr;
    for (const auto& Pair : Replies)
        if (Pair.Value.RequesterPC.Get() == Requester && (!Latest || Pair.Value.PlanRevision > Latest->PlanRevision))
            Latest = &Pair.Value;
    return Latest ? Latest->MessageId : FGuid();
}

// 玩家又开口时，取消这位玩家之前的请求和回复
void UDeliveryNpcAgentComponent::InterruptPlayerReply(APlayerController* Requester)
{
    if (!IsAuthority() || !IsValid(Requester) || Requester->GetWorld() != GetWorld())
        return;
    TGuardValue<bool> ChangingPlan(bChangingPlan, true);
    TArray<FGuid> Cancelled;
    bool bRequestHasReply = false;
    for (const auto& Pair : Replies)
        if (Pair.Value.RequesterPC.Get() == Requester)
        {
            Cancelled.Add(Pair.Value.ReplyId);
            bRequestHasReply |= Pair.Key == ActiveRequest.RequestId;
        }
    FGuid UnapprovedMessage;
    if (ActiveRequest.RequesterPC.Get() == Requester)
    {
        const FGuid RequestId = ActiveRequest.RequestId;
        TraceRequestEvent(ActiveRequest.WorldEpoch, RequestId, TEXT("Cancelled"), TEXT("PlayerInterrupted"));
        if (!bRequestHasReply)
            UnapprovedMessage = ActiveRequest.PlayerMessageId;
        ActiveRequest = {};
        RequestPlayerState.Reset();
        bDecisionPending = false;
        bDecisionQueued = false;
        if (auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>())
            Gateway->CancelRequest(this, RequestId);
    }
    if (PlayerReplyController.Get() == Requester)
    {
        if (!UnapprovedMessage.IsValid() && Cancelled.IsEmpty())
            UnapprovedMessage = PlayerReplyMessage;
        PlayerReplyMessage.Invalidate();
        PlayerReplyController.Reset();
        bPlayerRequest = false;
        EndListening();
    }
    // 一次移除整批，停止当前句的回调不能把同一玩家的旧待播句提到队首
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
        Npc->Presentation->CancelReplies(Cancelled);
    for (const FGuid& ReplyId : Cancelled)
        RemoveReply(ReplyId, TEXT("PlayerInterrupted"));
    if (Cancelled.Contains(PublicState.GetReplyId()))
    {
        PublicState.Speech.Reset();
        PublicState.ReplyToEventId.Invalidate();
        PublicState.bStreamSpeech = false;
        PublishState();
    }
    if (UnapprovedMessage.IsValid())
        OnPlayerReplyEnded.Broadcast(Requester, UnapprovedMessage, TEXT("PlayerInterrupted"));
    GetOwner()->ForceNetUpdate();
}

// 交谈流程第 9 步，服务器：玩家对我说话，开始倾听
bool UDeliveryNpcAgentComponent::BeginListening(AActor* Speaker, FGuid MessageId)
{
    if (!IsAuthority() || bChangingPlan || !IsValid(Speaker) || !MessageId.IsValid())
        return false;
    const auto* PlayerPawn = Cast<APawn>(Speaker);
    auto* Requester = PlayerPawn ? Cast<APlayerController>(PlayerPawn->GetController()) : nullptr;
    if (!Requester || Requester->GetWorld() != GetWorld())
        return false;
    const auto* Pacer = HasActiveAction() && PublicState.Action == EDeliveryNpcAction::Pace
                            ? Cast<ADeliveryCompanionCharacter>(GetOwner())
                            : nullptr;
    if (Pacer && Pacer->Companion->CanCommand(Requester->PlayerState))
    {
        // 访客可以接话，主人开始新一轮时可接过对话再决定是否改变踱步
        auto* Previous = PlayerReplyController.Get();
        if (!Previous)
            Previous = ActiveRequest.RequesterPC.Get();
        if (!Previous)
            for (const auto& Pair : Replies)
                if ((Previous = Pair.Value.RequesterPC.Get()))
                    break;
        if (Previous && Previous != Requester)
        {
            InterruptPlayerReply(Previous);
            if (!IsAuthority() || !IsValid(Requester) || !IsValid(Pacer) || !Pacer->Companion->CanCommand(Requester->PlayerState))
                return false;
        }
    }
    if (PlayerReplyController.IsValid() && PlayerReplyController.Get() != Requester && PlayerReplyMessage.IsValid())
        return false;
    if (ActiveRequest.bPlayerRequest && ActiveRequest.RequesterPC.Get() != Requester)
        return false;
    for (const auto& Pair : Replies)
        if (Pair.Value.RequesterPC.IsValid() && Pair.Value.RequesterPC.Get() != Requester)
            return false;
    // 录音开始后同一 MessageId 的最终文字到达，沿用本轮倾听而不是自我取消
    if (ListeningMessage == MessageId && PlayerReplyController.Get() == Requester)
        return true;

    // 打断这位玩家之前的回复
    InterruptPlayerReply(Requester);
    if (auto* Companion = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>())
    {
        // 先暂停这一轮出拳，再保存主人新消息，结束旧回合不会撤掉收手请求
        Companion->PauseAssistForOwnerConversation(Requester->PlayerState);
        if (!IsAuthority() || !IsValid(Requester))
            return false;
    }

    // 记下说话人和发言编号，最长倾听 30 秒
    ListeningSpeaker = Speaker;
    ListeningMessage = MessageId;
    PlayerReplyMessage = MessageId;
    PlayerReplyController = Requester;
    ListeningStartedAt = ServerTime();
    ListeningUntil = FPlatformTime::Seconds() + 30;
    bPlayerRequest = true;
    DeferJoinedResponse();
    if (!IsAuthority() || PlayerReplyMessage != MessageId || PlayerReplyController.Get() != Requester)
        return false;
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
        Npc->Presentation->PlayReaction(EDeliveryNpcVoiceReaction::HeardPlayer, MessageId);
    GetOwner()->ForceNetUpdate();

    // 播放“听见了”的短反应，日志 listening
    UE_LOG(LogDeliveryNpcAgent, Log, TEXT("NpcLatency speech=%s stage=listening"), *MessageId.ToString());
    return true;
}

// 结束倾听并通知客户端
void UDeliveryNpcAgentComponent::EndListening()
{
    ListeningSpeaker = nullptr;
    ListeningMessage.Invalidate();
    GetOwner()->ForceNetUpdate();
}

// 交谈流程第 15 步，服务器：收到模型建议，校验后登记这一轮回复
bool UDeliveryNpcAgentComponent::ReceiveStreamDecision(FGuid RequestId, const FDeliveryNpcDecisionProposal& Proposal)
{
    if (!IsAuthority() || bChangingPlan || !RequestId.IsValid() || RequestId != ActiveRequest.RequestId)
        return false;
    const FGuid TraceWorldEpoch = ActiveRequest.WorldEpoch;

    // 交给 TryAcceptProposal 校验，拒绝时日志 Decision ... accepted=0
    TraceProposal(Proposal);
    FName Error;
    if (!TryAcceptProposal(Proposal, Error))
    {
        ReceiveDecision(RequestId, nullptr, Error);
        return false;
    }
    TraceValidation(TraceWorldEpoch, RequestId, true, NAME_None);
    PublicState.bStreamSpeech = true;

    // 登记这一轮回复，之后的句子和声音按请求编号对上
    FReply Reply;
    BindJoinedResponse(Reply, ActiveRequest);
    Reply.ReplyId = PublicState.GetReplyId();
    Reply.MessageId = PublicState.ReplyToEventId;
    Reply.ActionId = PublicState.ActionId;
    Reply.Action = PublicState.Action;
    Reply.Target = PublicState.Target;
    Reply.bHadTarget = IsValid(PublicState.Target);
    Reply.Listener = PublicState.SpeechTarget;
    Reply.Depth = ActionConversationDepth;
    Reply.PlanRevision = ActiveRequest.PlanRevision;
    Reply.RequesterPC = ActiveRequest.RequesterPC;
    Reply.RequesterPlayerState = ActiveRequest.RequesterPlayerState;
    Reply.bContinueCurrent = Proposal.Action == EDeliveryNpcAction::ContinueCurrent;
    if (!RegisterReplyMessageContext(Reply, ActiveRequest, false))
    {
        if (Reply.bOwnsAction)
            TryFinishAction(Reply.ActionId, EDeliveryNpcActionStatus::Cancelled, TEXT("NpcMessageParentChanged"));
        return false;
    }
    Replies.Add(RequestId, Reply);
    // 继续同一场交战且没有台词时，正在播放的声音保持原样
    if (!Proposal.Speech.TrimStartAndEnd().IsEmpty())
        StartReplyPresentation(Reply);

    // 结束倾听，通知客户端，日志 approved
    // 模型生成期间听到的新话仍留给下一轮，收到动作建议不能清掉接话安排
    EndListening();
    PublishState();
    UE_LOG(LogDeliveryNpcAgent, Log, TEXT("NpcLatency request=%s stage=approved"), *RequestId.ToString());
    return true;
}

// 把回复交给表现组件开始播放，初遇回应过期时不播
bool UDeliveryNpcAgentComponent::StartReplyPresentation(FReply Reply)
{
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        if (!Npc->Presentation->HasReply(Reply.ReplyId))
        {
            if (!Reply.bPlayerReply && Reply.JoinedEventId.IsValid())
            {
                auto* Listener = GetJoinedResponseListener();
                if (!IsJoinedResponseOwnerCurrent() || GetWorld()->GetTimeSeconds() >= JoinedResponseExpiresAt ||
                    Reply.JoinedEventId != PendingJoinedEvent ||
                    !Reply.JoinedOwner.HasSameIndexAndSerialNumber(PendingJoinedOwner))
                {
                    CancelReply(Reply.ReplyId);
                    return false;
                }
                Reply.Listener = Listener;
            }
            // 较新的台词已开始准备后，旧请求迟到的第一句不能挤掉它
            if (Reply.PlanRevision < LatestSpeechPlanRevision)
            {
                CancelReply(Reply.ReplyId);
                return false;
            }
            Npc->Presentation->BeginReply(Reply.ReplyId, Reply.MessageId, Reply.Depth, Reply.Listener.Get());
            if (!Npc->Presentation->HasReply(Reply.ReplyId))
            {
                CancelReply(Reply.ReplyId);
                return false;
            }
            if (Reply.bModelFinished)
                Npc->Presentation->SetReplySentenceCount(Reply.ReplyId, Reply.SentenceCount);
            LatestSpeechPlanRevision = Reply.PlanRevision;
        }
        return Npc->Presentation->HasReply(Reply.ReplyId);
    }
    return false;
}

// 取消一轮回复
void UDeliveryNpcAgentComponent::CancelReply(FGuid ReplyId)
{
    RemoveReply(ReplyId, TEXT("ReplyCancelled"));
}

// 回复播完后能否关联到初遇事件
bool UDeliveryNpcAgentComponent::CanLinkReplyPlayback(FGuid ReplyId) const
{
    for (const auto& Pair : Replies)
    {
        const auto& Reply = Pair.Value;
        if (Reply.ReplyId != ReplyId || Reply.bPlayerReply || !Reply.JoinedEventId.IsValid())
            continue;
        return Reply.JoinedEventId == PendingJoinedEvent && Reply.JoinedOwner.HasSameIndexAndSerialNumber(PendingJoinedOwner) &&
               GetWorld()->GetTimeSeconds() < JoinedResponseExpiresAt && !Reply.bJoinedPlaybackMissed &&
               CanHearJoinedResponse(GetJoinedResponseListener());
    }
    return true;
}

// 表现组件报告整轮播放完成，释放交谈占用并结清初遇回应
void UDeliveryNpcAgentComponent::FinishReplyPlayback(FGuid ReplyId, FGuid CompletedSpeechEventId)
{
    if (CompletedSpeechEventId.IsValid() && IsJoinedResponseOwnerCurrent() &&
        GetWorld()->GetTimeSeconds() < JoinedResponseExpiresAt)
        for (const auto& Pair : Replies)
        {
            const auto& Reply = Pair.Value;
            // 沿本轮主 MessageId 验证整轮 SelfSpeech，获救关联不替换玩家发言或语音轮次
            // 结清的是对主人的回应机会，是否说得自然仍需实际听感检查
            if (Reply.ReplyId == ReplyId && Reply.JoinedEventId == PendingJoinedEvent &&
                Reply.JoinedOwner.HasSameIndexAndSerialNumber(PendingJoinedOwner) && Reply.SentenceCount > 0 &&
                CanLinkReplyPlayback(ReplyId) &&
                Evidence.ContainsByPredicate(
                    [this, &Reply, CompletedSpeechEventId](const auto& Fact)
                    {
                        return Fact.EventId == CompletedSpeechEventId && Fact.SubjectId == NpcId &&
                               Fact.Kind == TEXT("SelfSpeech") && Fact.Source == EDeliveryNpcEvidenceSource::ActionResult &&
                               Reply.MessageId.IsValid() && Fact.ReplyToEventId == Reply.MessageId &&
                               !Fact.Description.TrimStartAndEnd().IsEmpty();
                    }))
            {
                UE_LOG(LogDeliveryNpcAgent, Log, TEXT("CompanionJoined response played npc=%s event=%s reply=%s"),
                       *GetNameSafe(GetOwner()), *PendingJoinedEvent.ToString(), *ReplyId.ToString());
                PendingJoinedEvent.Invalidate();
                PendingJoinedOwner.Reset();
                PendingJoinedSubject.Reset();
                break;
            }
        }
    for (const auto& Pair : Replies)
        if (Pair.Value.ReplyId == ReplyId && IsDecisionTraceCurrent(DecisionTrace.WorldEpoch, Pair.Key))
        {
            // 零台词也走完成回调，所以这里只表示回复生命周期结束
            DecisionTrace.ReplyStage = TEXT("Finished");
            PublishDecisionTrace();
            break;
        }
    RemoveReply(ReplyId, TEXT("ReplyCompleted"));
}

// 删除一轮回复，取消后台合成并释放交谈占用
void UDeliveryNpcAgentComponent::RemoveReply(FGuid ReplyId, FName Reason)
{
    if (!IsAuthority() || !ReplyId.IsValid())
        return;
    FGuid RequestId;
    for (const auto& Pair : Replies)
        if (Pair.Value.ReplyId == ReplyId)
        {
            RequestId = Pair.Key;
            break;
        }
    if (!RequestId.IsValid())
        return;
    const FReply Removed = Replies.FindChecked(RequestId);
    if (Reason != TEXT("ReplyCompleted"))
        TraceRequestEvent(DecisionTrace.WorldEpoch, RequestId, TEXT("Cancelled"), Reason);
    Replies.Remove(RequestId);
    // 先撤下自身请求，再发布终态，取消后代的同步回调不能重复处理这轮
    if (auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>())
        Gateway->CancelRequest(this, RequestId);
    if (ActiveRequest.RequestId == RequestId)
    {
        ActiveRequest = {};
        bDecisionPending = false;
        bDecisionQueued = false;
        RequestPlayerState.Reset();
    }
    if (PlayerReplyController == Removed.RequesterPC && PlayerReplyMessage == Removed.MessageId)
    {
        PlayerReplyController.Reset();
        PlayerReplyMessage.Invalidate();
        bPlayerRequest = false;
        EndListening();
    }
    auto State = EDeliveryNpcReplyPresentation::Interrupted;
    if (Reason == TEXT("ReplyCompleted"))
        State = EDeliveryNpcReplyPresentation::Completed;
    // NPC 已收到的台词仍是事实，换动作只会停止对应的声音
    else if (Reason == TEXT("ModelChangedAction") &&
             WasReplyTextDelivered(ReplyId, Cast<ADeliveryNpcCharacter>(Removed.Listener.Get())))
        State = EDeliveryNpcReplyPresentation::Unavailable;
    ReportNpcReplyPresentation(ReplyId, State);
    if (!IsAuthority())
        return;
    if (Removed.RequesterPC.IsValid() && Removed.MessageId.IsValid())
        OnPlayerReplyEnded.Broadcast(Removed.RequesterPC.Get(), Removed.MessageId, Reason);
}

// 网关收到整句，交给表现组件
void UDeliveryNpcAgentComponent::ReceiveSentence(FGuid RequestId, const FDeliveryNpcSentence& Sentence)
{
    if (!CanReceiveReply(RequestId))
        return;
    const FReply Reply = Replies.FindChecked(RequestId);
    if (!StartReplyPresentation(Reply))
        return;
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        Npc->Presentation->QueueSentence(Reply.ReplyId, Reply.MessageId, Sentence);
        TraceReplyText(RequestId, Sentence.Text);
    }
}

// 这个请求编号的回复是否仍在等待数据
bool UDeliveryNpcAgentComponent::CanReceiveReply(FGuid RequestId) const
{
    const auto* Reply = Replies.Find(RequestId);
    if (Reply && !Reply->bPlayerReply && Reply->JoinedEventId.IsValid() &&
        (!IsJoinedResponseOwnerCurrent() || Reply->JoinedEventId != PendingJoinedEvent ||
         !Reply->JoinedOwner.HasSameIndexAndSerialNumber(PendingJoinedOwner)))
        return false;
    return IsAuthority() && !bChangingPlan && Reply && !Reply->bTransferFinished;
}

// 网关收到一句的开头，交给表现组件
bool UDeliveryNpcAgentComponent::ReceiveSpeechBegin(FGuid RequestId, const FDeliveryNpcSpeechBegin& Begin)
{
    if (!CanReceiveReply(RequestId))
        return false;
    const FReply Reply = Replies.FindChecked(RequestId);
    if (!StartReplyPresentation(Reply))
        return false;
    auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    const bool bAccepted = Npc && Npc->Presentation->BeginSentenceAudio(Reply.ReplyId, Reply.MessageId, Begin);
    if (bAccepted)
        TraceReplyText(RequestId, Begin.Text);
    return bAccepted;
}

// 网关收到一段声音，交给表现组件
bool UDeliveryNpcAgentComponent::ReceiveAudioChunk(FGuid RequestId, const FDeliveryNpcAudioChunk& Chunk)
{
    if (!CanReceiveReply(RequestId))
        return false;
    const FReply Reply = Replies.FindChecked(RequestId);
    auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    return Npc && Npc->Presentation->AppendSentenceAudio(Reply.ReplyId, Reply.MessageId, Chunk);
}

// 网关收到一句的结束，交给表现组件
bool UDeliveryNpcAgentComponent::ReceiveAudioEnd(FGuid RequestId, const FDeliveryNpcAudioEnd& End)
{
    if (!CanReceiveReply(RequestId))
        return false;
    const FReply Reply = Replies.FindChecked(RequestId);
    auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    return Npc && Npc->Presentation->EndSentenceAudio(Reply.ReplyId, Reply.MessageId, End);
}

// 模型输出结束，记下共有几句，声音继续按原请求接收
bool UDeliveryNpcAgentComponent::FinishModelReply(FGuid RequestId, int32 SentenceCount, TOptional<FString> ApprovedText)
{
    if (!IsAuthority() || bChangingPlan || SentenceCount < 0 || SentenceCount > 3)
        return false;
    auto* Reply = Replies.Find(RequestId);
    if (!Reply || Reply->bModelFinished || ActiveRequest.RequestId != RequestId)
        return false;
    if (ApprovedText.IsSet())
    {
        if (!DeliveryNpcMessages::IsValidReplyText(ApprovedText.GetValue()) ||
            ((SentenceCount == 0) != ApprovedText.GetValue().IsEmpty()))
            return false;
        Reply->ApprovedText = MoveTemp(ApprovedText.GetValue());
        Reply->bHasApprovedText = true;
    }
    Reply->bModelFinished = true;
    Reply->SentenceCount = SentenceCount;
    const FGuid ReplyId = Reply->ReplyId;
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
        Npc->Presentation->SetReplySentenceCount(ReplyId, SentenceCount);
    FinishDecisionWait(RequestId, true, SentenceCount);
    TryDeliverReplyMessage(RequestId);
    return true;
}

// 整轮回复的数据接收结束：成功时等表现组件播完，失败时清理
void UDeliveryNpcAgentComponent::FinishReply(FGuid RequestId, bool bSucceeded, int32 SentenceCount)
{
    if (!IsAuthority() || bChangingPlan || !RequestId.IsValid())
        return;
    auto* Found = Replies.Find(RequestId);
    if (!Found)
    {
        if (ActiveRequest.RequestId == RequestId)
            ReceiveDecision(RequestId, nullptr, TEXT("TransportFailure"));
        return;
    }
    const FReply Reply = *Found;
    if (Reply.bTransferFinished)
        return;
    if (SentenceCount < 0 || SentenceCount > 3 || (Reply.bModelFinished && SentenceCount != Reply.SentenceCount))
        bSucceeded = false;
    if (bSucceeded)
    {
        if (!Reply.bModelFinished && !FinishModelReply(RequestId, SentenceCount))
            return;
        if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
            Npc->Presentation->FinishSentences(Reply.ReplyId, SentenceCount);
        if (auto* Current = Replies.Find(RequestId))
            Current->bTransferFinished = true;
        // 零台词没有播放结束回调；只有真实玩家的普通动作仍需等动作回执
        const bool bNeedsPlayerActionReceipt =
            !Reply.bContinueCurrent && Reply.RequesterPC.IsValid() && Reply.MessageId.IsValid();
        if (SentenceCount == 0 && !bNeedsPlayerActionReceipt)
            FinishReplyPlayback(Reply.ReplyId);
        return;
    }
    FailReply(RequestId);
}

// 回复失败：清理队列，播放失败反馈
void UDeliveryNpcAgentComponent::FailReply(FGuid RequestId)
{
    // 已收到完整流的指令也可能在执行时被拒绝，按原回复编号清队列
    FReply Reply;
    if (!Replies.RemoveAndCopyValue(RequestId, Reply))
        return;
    if (auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>())
        Gateway->CancelRequest(this, RequestId);
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
        Npc->Presentation->CancelReply(Reply.ReplyId);
    // 传输失败已真正停止这轮声音；保留已送达文本，后代仍等其它祖先播完
    ReportNpcReplyPresentation(Reply.ReplyId, Reply.bHasApprovedText ? EDeliveryNpcReplyPresentation::Unavailable
                                                                     : EDeliveryNpcReplyPresentation::Interrupted);
    if (!IsAuthority())
        return;
    if (ActiveRequest.RequestId == RequestId)
        FinishDecisionWait(RequestId, false, 0);
    else if (Reply.RequesterPC.IsValid() && Reply.MessageId.IsValid())
    {
        if (PlayerReplyMessage == Reply.MessageId && PlayerReplyController == Reply.RequesterPC)
            FinishFailedPlayerReply(Reply.MessageId, nullptr, false);
        else
        {
            if (auto* Conversation = Reply.RequesterPC->FindComponentByClass<UDeliveryConversationComponent>())
                Conversation->ClientResult(TEXT("ReplyFailed"), Reply.MessageId);
            OnPlayerReplyEnded.Broadcast(Reply.RequesterPC.Get(), Reply.MessageId, TEXT("ReplyFailed"));
        }
    }
}

// 流式请求结束等待：失败时按需重试，成功时安排下一次决策
void UDeliveryNpcAgentComponent::FinishDecisionWait(FGuid RequestId, bool bSucceeded, int32 SentenceCount)
{
    if (ActiveRequest.RequestId != RequestId)
        return;
    const bool bJoinedRetryLimit =
        !ActiveRequest.bPlayerRequest && GetJoinedResponseEvent(ActiveRequest).IsValid() && JoinedResponseAttempts >= 3;
    const bool bFinishingCurrentPlayer =
        !bPlayerRequest || (ActiveRequest.PlayerMessageId.IsValid() && ActiveRequest.PlayerMessageId == PlayerReplyMessage);
    // 动作失败后，表现组件可能已经停止对白；是否零台词仍以本次后台结束通知为准
    const bool bSilent = bSucceeded && SentenceCount == 0 && PublicState.bStreamSpeech;
    FGuid FailedMessage;
    AActor* FailedSpeaker = nullptr;
    if (!bSucceeded)
        GetRequestedPlayerMessage(FailedMessage, FailedSpeaker);
    if (bSucceeded)
        RecordActionReply();
    const bool bContinueConversation = bDecisionQueued;
    ActiveRequest = {};
    bDecisionPending = false;
    bDecisionQueued = false;
    if (bSucceeded && bFinishingCurrentPlayer)
    {
        // 动作先失败、后端随后确认没有台词时，也结束这次界面等待
        if (bSilent && DeliveryNpc::IsTerminal(PublicState.Status) && PublicState.Status != EDeliveryNpcActionStatus::Succeeded)
            FinishFailedPlayerReply(PublicState.ReplyToEventId, nullptr, false);
        bPlayerRequest = false;
        // 没有台词时等动作真正完成，再通知原来发言的玩家
        const bool bAwaitingAction =
            HasActiveAction() && PlayerReplyMessage.IsValid() && PlayerReplyMessage == PublicState.ReplyToEventId && bSilent;
        if (!bAwaitingAction)
        {
            PlayerReplyMessage.Invalidate();
            PlayerReplyController.Reset();
        }
        EndListening();
    }
    OnDecisionFinished.Broadcast(
        bSucceeded ? NAME_None : FName(bJoinedRetryLimit ? TEXT("JoinedResponseDeferred") : TEXT("TransportFailure")));
    // 播放上一句期间有人接话，等这句结束后继续处理，不让完成通知清掉新话
    if (bContinueConversation && bSucceeded)
        if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
            Npc->WakeDecision();
    if (!bSucceeded)
    {
        const auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
        FinishFailedPlayerReply(FailedMessage, FailedSpeaker, Npc && Npc->IsDecisionRetryScheduled());
    }
    GetOwner()->ForceNetUpdate();
}

// 当前请求是否已经包含这个人的攻击
bool UDeliveryNpcAgentComponent::IsRespondingToAttackFrom(const AActor* Attacker) const
{
    if (!bDecisionPending || !IsValid(Attacker))
        return false;
    return ActiveRequest.Evidence.ContainsByPredicate(
        [this, Attacker](const auto& Item)
        {
            return (Item.Kind == TEXT("FightThreat") || Item.Kind == TEXT("LightMeleeHit") || Item.Kind == TEXT("Hit")) &&
                   KnownSubjects.FindRef(Item.SubjectId).Get() == Attacker;
        });
}

// 能否看向这个对象：在视线内、距离不超过上限
bool UDeliveryNpcAgentComponent::CanLookAt(AActor* Target) const
{
    // 已知名称不授权跟踪实物当前位置，二狗的普通道具候选沿用当前视觉守门
    if (const auto* Companion = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>())
        if (const auto* Item = Cast<ADeliveryHandheldItem>(Target);
            DeliveryHandheldTransfer::IsOrdinaryItem(Item) && !Companion->CanUseObservedItemTarget(Item))
            return false;
    const auto* Pawn = Cast<APawn>(GetOwner());
    if (!Pawn || !Cast<AAIController>(Pawn->GetController()) || !IsValid(Target) || Target == GetOwner() ||
        Target->GetWorld() != GetWorld() ||
        FVector::DistSquared(Target->GetActorLocation(), GetOwner()->GetActorLocation()) >
            FMath::Square(FMath::Clamp(MaxLookAtDistance, 100.f, 3000.f)))
        return false;
    // 居民视线取躯干高度，车辆根点可能位于地面，不能把地面误判成目标前的墙
    if (Cast<ADeliveryNpcCharacter>(GetOwner()))
        return DeliveryNpcConversationTrace::HasClearPath(GetOwner(), Target, FVector(0, 0, 40));
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(DeliveryNpcLookAt), false, GetOwner());
    return !GetWorld()->LineTraceSingleByChannel(Hit, GetOwner()->GetActorLocation(), Target->GetActorLocation(), ECC_Visibility,
                                                 Params) ||
           Hit.GetActor() == Target;
}

// 交谈流程第 15 步，服务器：逐项校验模型建议，全部通过才写入 PublicState
bool UDeliveryNpcAgentComponent::TryAcceptProposal(const FDeliveryNpcDecisionProposal& Proposal, FName& Reason)
{
    Reason = TEXT("StaleProposal");
    auto RejectStale = [&](const TCHAR* Cause, FGuid EventId = FGuid())
    {
        Reason = TEXT("StaleProposal");
        UE_LOG(LogDeliveryNpcAgent, Log, TEXT("Decision stale npc=%s request=%s active_request=%s cause=%s event=%s"),
               *NpcId.ToString(), *Proposal.RequestId.ToString(), *ActiveRequest.RequestId.ToString(), Cause,
               *EventId.ToString());
        return false;
    };

    // 1. 请求是否过期：编号、世界、NPC、上下文和计划修订号必须与发出时一致
    if (!IsAuthority())
        return RejectStale(TEXT("NotAuthority"));
    if (bChangingPlan)
        return RejectStale(TEXT("PlanChanging"));
    if (!ActiveRequest.RequestId.IsValid())
        return RejectStale(TEXT("RequestEnded"));
    if (!IsNpcMessageParentCurrent(ActiveRequest.ParentNpcMessage))
        return RejectStale(TEXT("NpcMessageParentChanged"));
    PruneEvidence();
    const bool bResident = GetOwner()->IsA<ADeliveryNpcCharacter>();
    const bool bUpdatingAction = ActiveRequest.RunningActionId.IsValid();
    if (bUpdatingAction)
    {
        if ((ActiveRequest.RequestedCapabilityVersion >= 6 ? !IsActivitySnapshotCurrent(ActiveRequest)
                                                           : !CanUpdateRunningAction()) ||
            ActiveRequest.RunningActionId != PublicState.ActionId)
            return RejectStale(TEXT("RunningActionChanged"));
    }
    else if (HasActiveAction())
        return RejectStale(TEXT("AnotherActionStarted"));
    // 按发送时保存的信息核对回复，普通视野更新不应让已经生成的台词被丢弃
    // 其他类型的角色仍要求上下文版本完全一致；受击时通过 CancelPlan 取消当前请求
    if (Proposal.RequestId != ActiveRequest.RequestId)
        return RejectStale(TEXT("RequestMismatch"));
    if (Proposal.WorldEpoch != ActiveRequest.WorldEpoch)
        return RejectStale(TEXT("WorldChanged"));
    if (Proposal.NpcId != NpcId)
        return RejectStale(TEXT("NpcMismatch"));
    if (Proposal.ContextRevision != ActiveRequest.ContextRevision || (!bResident && Proposal.ContextRevision != ContextRevision))
        return RejectStale(TEXT("ContextChanged"));
    if (Proposal.PlanRevision != ActiveRequest.PlanRevision)
        return RejectStale(TEXT("PlanChanged"));
    if (Proposal.CapabilityVersion != ActiveRequest.RequestedCapabilityVersion)
    {
        Reason = TEXT("CapabilityVersionMismatch");
        return false;
    }
    FGuid PendingPlayerMessage;
    AActor* RequestSpeaker = nullptr;
    GetRequestedPlayerMessage(PendingPlayerMessage, RequestSpeaker);
    const bool bAnsweringPlayer = PendingPlayerMessage.IsValid();
    // 先回答玩家，后来播完的 NPC 闲聊留给下一轮；新玩家发言仍会作废旧回复
    // 接话安排和原话已经由 HearSpeech 保存，这里不把后来听到的话记为已处理
    if (bResident && !bUpdatingAction)
    {
        const auto* NewMessage = Evidence.FindByPredicate(
            [this, bAnsweringPlayer](const auto& Item)
            {
                return Item.bNeedsReply &&
                       (Item.Kind == TEXT("PlayerSpeech") ||
                        (!bAnsweringPlayer && (Item.Kind == TEXT("Speech") || Item.Kind == TEXT("NpcMessage")))) &&
                       !ActiveRequest.Evidence.ContainsByPredicate(
                           [&Item](const auto& Sent)
                           {
                               return Sent.EventId == Item.EventId;
                           });
            });
        if (NewMessage)
            return RejectStale(NewMessage->Kind == TEXT("PlayerSpeech") ? TEXT("NewPlayerSpeech") : TEXT("NewNpcSpeech"),
                               NewMessage->EventId);
    }
    if (const FName Refresh = GetRequestedCompanionErrandRefreshReason(Proposal.Action); !Refresh.IsNone())
    {
        Reason = Refresh;
        return false;
    }

    // 2. 动作必须在本次允许列表里，候选编号必须是当时发出的那一个
    Reason = TEXT("UnsupportedAction");
    if (!ActiveRequest.AllowedActions.Contains(Proposal.Action))
        return false;
    const FDeliveryNpcCandidate* SelectedCandidate = nullptr;
    if (ActiveRequest.RequestedCapabilityVersion >= 5)
    {
        Reason = TEXT("InvalidCandidate");
        if (!Proposal.CandidateId.IsValid())
            return false;
        SelectedCandidate = ActiveRequest.FindCandidate(Proposal.CandidateId);
        if (!SelectedCandidate || SelectedCandidate->Action != Proposal.Action ||
            SelectedCandidate->TargetId != Proposal.TargetId || SelectedCandidate->EvidenceIds != Proposal.EvidenceIds)
            return false;
        if (!FMath::IsFinite(Proposal.Duration) || Proposal.Duration > DeliveryNpc::CandidateDuration(SelectedCandidate->Action))
        {
            Reason = TEXT("InvalidDuration");
            return false;
        }
    }
    const FGuid JoinedEvent = GetJoinedResponseEvent(ActiveRequest);
    if (ActiveRequest.JoinedResponseEvent.IsValid() && ActiveRequest.JoinedResponseEvent != JoinedEvent)
        return RejectStale(TEXT("JoinedResponseChanged"));
    APawn* JoinedListener = nullptr;
    if (JoinedEvent.IsValid() && !ActiveRequest.bPlayerRequest)
    {
        JoinedListener = GetJoinedResponseListener();
        if (!JoinedListener)
        {
            Reason = TEXT("JoinedOwnerUnavailable");
            return false;
        }
        if (Proposal.Speech.TrimStartAndEnd().IsEmpty())
        {
            Reason = TEXT("MissingJoinedReply");
            return false;
        }
        if (Proposal.bHasSpeechTarget && Proposal.SpeechTargetId.IsValid() &&
            !IsJoinedReplyTarget(ActiveRequest, Proposal.SpeechTargetId, *JoinedListener))
        {
            Reason = TEXT("JoinedReplyTargetChanged");
            return false;
        }
    }
    if (Proposal.Action == EDeliveryNpcAction::ContinueCurrent)
    {
        const auto Preference =
            SelectedCandidate ? SelectedCandidate->SpeechPreference : EDeliveryCompanionSpeechPreference::Unspecified;
        if (Preference != EDeliveryCompanionSpeechPreference::Unspecified &&
            (!bAnsweringPlayer || PendingPlayerMessage != ActiveRequest.PlayerMessageId))
        {
            Reason = TEXT("SpeechPreferenceRequiresOwnerMessage");
            return false;
        }
        return AcceptContinueCurrent(Proposal, Preference, Reason);
    }
    if (Proposal.Action == EDeliveryNpcAction::StorePackageInBackpack &&
        (!ActiveRequest.bPlayerRequest || !ActiveRequest.bRequesterIsCompanionOwner))
    {
        Reason = TEXT("BackpackHandoffRequiresOwnerRequest");
        return false;
    }
    TSharedPtr<FDeliveryCompanionHomeChoice> AcceptedHomeChoice;
    TSharedPtr<FDeliveryCompanionItemChoice> AcceptedItemChoice;
    Reason = TEXT("InvalidDuration");
    if ((Proposal.Action == EDeliveryNpcAction::Fight || Proposal.Action == EDeliveryNpcAction::Flee) &&
        (!FMath::IsFinite(Proposal.Duration) || Proposal.Duration < UDeliveryNpcCombatComponent::StrikeDuration ||
         Proposal.Duration > DeliveryNpc::MaxDecisionDuration(Proposal.Action)))
        return false;

    // 3. 引用的事件必须仍然记得
    Reason = TEXT("InvalidEvidence");
    const auto* SelectedItem = SelectedCandidate ? SelectedCandidate->ItemChoice.Get() : nullptr;
    const bool bSearchLead = SelectedItem && SelectedItem->UsesRequestEvidence();
    for (const auto& Id : Proposal.EvidenceIds)
    {
        const auto* Fact = ActiveRequest.Evidence.FindByPredicate(
            [&Id](const auto& Item)
            {
                return Item.EventId == Id;
            });
        if (!Fact)
            return false;
        if (bSearchLead)
        {
            const bool bCaterpillar = SelectedItem->GetAction() == EDeliveryNpcAction::CollectCaterpillar ||
                                      SelectedItem->GetAction() == EDeliveryNpcAction::InsertCaterpillar;
            const bool bExpectedFact =
                SelectedItem->IsOwnerInventorySource() ? Fact->Kind == TEXT("CompanionOwnerItemAvailable") &&
                                                             Fact->Source == EDeliveryNpcEvidenceSource::ActionResult
                : bCaterpillar
                    ? ((Fact->Kind == TEXT("WorldItemSeen") && Fact->Source == EDeliveryNpcEvidenceSource::Seen &&
                        SelectedItem->Source == TEXT("World")) ||
                       (Fact->Kind == DeliveryCompanionItemSearch::EvidenceKind &&
                        Fact->Source == EDeliveryNpcEvidenceSource::ActionResult && SelectedItem->Source == TEXT("World")) ||
                       (Fact->Kind == TEXT("CompanionHoldingItem") && Fact->Source == EDeliveryNpcEvidenceSource::ActionResult &&
                        SelectedItem->GetAction() == EDeliveryNpcAction::InsertCaterpillar &&
                        SelectedItem->Source == TEXT("CompanionHand")))
                    : Fact->Kind == DeliveryCompanionItemSearch::EvidenceKind &&
                          Fact->Source == EDeliveryNpcEvidenceSource::ActionResult;
            if (Id != SelectedItem->EvidenceId || !bExpectedFact || Fact->bNeedsReply || Fact->SubjectId != Proposal.TargetId ||
                Fact->ObservedAt != SelectedItem->ObservedAt || Fact->ExpiresAt <= GetWorld()->GetTimeSeconds())
                return false;
        }
        else if (!Evidence.ContainsByPredicate(
                     [&Id](const auto& Item)
                     {
                         return Item.EventId == Id;
                     }))
            return false;
    }
    AActor* Target = nullptr;
    const bool bCompanionPace = Proposal.Action == EDeliveryNpcAction::Pace && ActiveRequest.bHasCompanionContext;

    // 4. 目标必须有效，并且现在仍能执行这个动作
    Reason = TEXT("InvalidTarget");
    if (DeliveryNpc::IsCompanionItemErrand(Proposal.Action))
    {
        if (!SelectedItem)
            return false;
        if (Proposal.Action == EDeliveryNpcAction::SearchNearOwner)
        {
            Reason = TEXT("AreaSearchRequiresCurrentOwnerSpeech");
            if (!bAnsweringPlayer || PendingPlayerMessage != ActiveRequest.PlayerMessageId ||
                !ActiveRequest.bRequesterIsCompanionOwner || Proposal.TargetId.IsValid() || Proposal.EvidenceIds.Num() != 1 ||
                Proposal.EvidenceIds[0] != ActiveRequest.PlayerMessageId)
                return false;
        }
        else
        {
            Target = bSearchLead ? SelectedItem->Item.Get() : KnownSubjects.FindRef(Proposal.TargetId).Get();
            if (!IsValid(Target) || SelectedItem->Item.Get() != Target)
                return false;
        }
    }
    else if (Proposal.Action == EDeliveryNpcAction::JoinAssist)
    {
        const auto* Companion = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
        Target = KnownSubjects.FindRef(Proposal.TargetId).Get();
        if (!Companion || Target != Companion->GetAssistTarget() || Proposal.EvidenceIds.Num() != 1 ||
            Proposal.EvidenceIds[0] != Companion->GetAssistEvidenceId() ||
            !Companion->IsAssistEvidenceCurrent(Target, Proposal.EvidenceIds[0]))
            return false;
    }
    else if (Proposal.Action == EDeliveryNpcAction::LookAt || Proposal.Action == EDeliveryNpcAction::MoveTo ||
             Proposal.Action == EDeliveryNpcAction::StrikeOnce || Proposal.Action == EDeliveryNpcAction::Fight ||
             Proposal.Action == EDeliveryNpcAction::Flee || Proposal.Action == EDeliveryNpcAction::HoldVehicle ||
             Proposal.Action == EDeliveryNpcAction::MoveAside)
    {
        Target = KnownSubjects.FindRef(Proposal.TargetId).Get();
        if (!CanLookAt(Target) || !Evidence.ContainsByPredicate(
                                      [&Proposal](const auto& Item)
                                      {
                                          return Item.SubjectId == Proposal.TargetId &&
                                                 Proposal.EvidenceIds.Contains(Item.EventId);
                                      }))
            return false;
    }
    else if (Proposal.TargetId.IsValid())
        return false;
    if (auto* Resident = Cast<ADeliveryNpcCharacter>(GetOwner()))
    {
        if (!bSearchLead && !CanChooseAction(Proposal.Action, Target, RequestPlayerState.Get()))
            return false;
        if (bCompanionPace)
        {
            const auto* Companion = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
            Reason = TEXT("StaleCompanionCommand");
            if (!Companion || Companion->GetCommandRevision() != ActiveRequest.CompanionCommandRevision)
                return false;
        }
        if (DeliveryNpc::IsCompanionCommand(Proposal.Action))
        {
            FDeliveryNpcDecisionRequest Current;
            Resident->ExtendDecisionRequest(Current, RequestPlayerState.Get());
            Reason = TEXT("StaleCompanionCommand");
            if (Current.CompanionCommandRevision != ActiveRequest.CompanionCommandRevision)
                return false;
            if (Proposal.Action == EDeliveryNpcAction::JoinAssist || Proposal.Action == EDeliveryNpcAction::StopAssist)
            {
                if (Current.CompanionAssistRevision != ActiveRequest.CompanionAssistRevision)
                    return false;
                if (Proposal.Action == EDeliveryNpcAction::StopAssist &&
                    (!ActiveRequest.bPlayerRequest || !ActiveRequest.bPendingPlayerMessage ||
                     !ActiveRequest.bRequesterIsCompanionOwner || !bAnsweringPlayer ||
                     PendingPlayerMessage != ActiveRequest.PlayerMessageId))
                    return false;
            }
            // 弱引用的普通相等会把两个失效对象看作相同，任务快照须保留原对象身份
            if (DeliveryNpc::IsCompanionErrand(Proposal.Action) && !DeliveryNpc::IsCompanionItemErrand(Proposal.Action) &&
                (!Current.CompanionTask.HasSameIndexAndSerialNumber(ActiveRequest.CompanionTask) ||
                 !Current.CompanionPackage.HasSameIndexAndSerialNumber(ActiveRequest.CompanionPackage)))
            {
                Reason = TEXT("CompanionTaskOrPackageChanged");
                return false;
            }
            if (DeliveryNpc::IsCompanionItemErrand(Proposal.Action))
            {
                const auto* Companion = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
                const auto& Choice = *SelectedCandidate->ItemChoice;
                Reason = TEXT("StaleItemChoice");
                if (!Companion || Choice.CandidateId != Proposal.CandidateId || Choice.RequestId != ActiveRequest.RequestId ||
                    !Choice.Requester.HasSameIndexAndSerialNumber(RequestPlayerState) ||
                    !Companion->CanApplyItemChoice(Choice, RequestPlayerState.Get(), ActiveRequest.CompanionCommandRevision))
                    return false;
                AcceptedItemChoice = MakeShared<FDeliveryCompanionItemChoice>(Choice);
            }
            if (Proposal.Action == EDeliveryNpcAction::HomeAction)
            {
                Reason = TEXT("StaleHomeChoice");
                const auto* Companion = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
                if (!SelectedCandidate || !SelectedCandidate->HomeChoice.IsValid() || !Companion)
                    return false;
                const auto& Choice = *SelectedCandidate->HomeChoice;
                if (Choice.CandidateId != Proposal.CandidateId || Choice.RequestId != ActiveRequest.RequestId ||
                    !RequestPlayerState.IsValid() || !Choice.Requester.HasSameIndexAndSerialNumber(RequestPlayerState) ||
                    !Companion->CanApplyHomeChoice(Choice, RequestPlayerState.Get(), ActiveRequest.CompanionCommandRevision))
                    return false;
                AcceptedHomeChoice = MakeShared<FDeliveryCompanionHomeChoice>(Choice);
            }
        }
        const bool bOffered =
            SelectedCandidate || ActiveRequest.Candidates.ContainsByPredicate(
                                     [&](const auto& Item)
                                     {
                                         return Item.Action == Proposal.Action && Item.TargetId == Proposal.TargetId;
                                     });
        if (!bOffered)
            return false;
    }

    // 5. 时长在范围内；回复玩家时说话对象必须是那位玩家
    Reason = TEXT("InvalidDuration");
    if (!FMath::IsFinite(Proposal.Duration) ||
        (DeliveryNpc::IsCompanionCommand(Proposal.Action)
             ? Proposal.Duration != 0.f
             : (Proposal.Duration < .1f || Proposal.Duration > DeliveryNpc::MaxDecisionDuration(Proposal.Action))))
        return false;
    FGuid PlayerMessage;
    AActor* SpeechTarget = nullptr;
    if (bResident)
    {
        for (const auto& Item : ActiveRequest.Evidence)
            if (Item.Kind == TEXT("PlayerSpeech"))
            {
                PlayerMessage = Item.EventId;
                SpeechTarget = KnownSubjects.FindRef(Item.SubjectId).Get();
            }
        if (ActiveRequest.Evidence.ContainsByPredicate(
                [&PlayerMessage](const auto& Item)
                {
                    return (Item.Kind == TEXT("SelfSpeech") || Item.Kind == TEXT("ActionReply")) &&
                           Item.ReplyToEventId == PlayerMessage;
                }))
            PlayerMessage.Invalidate();
        // 没有台词时仍要做出玩家能看见的动作，空等不能算作回应
        const bool bVisibleAction =
            bUpdatingAction || Proposal.Action == EDeliveryNpcAction::LookAt || Proposal.Action == EDeliveryNpcAction::MoveTo ||
            Proposal.Action == EDeliveryNpcAction::StrikeOnce || Proposal.Action == EDeliveryNpcAction::Fight ||
            Proposal.Action == EDeliveryNpcAction::HoldVehicle || Proposal.Action == EDeliveryNpcAction::ReleaseVehicle ||
            Proposal.Action == EDeliveryNpcAction::LieDown || Proposal.Action == EDeliveryNpcAction::GetUp ||
            Proposal.Action == EDeliveryNpcAction::Pace || Proposal.Action == EDeliveryNpcAction::MoveAside ||
            Proposal.Action == EDeliveryNpcAction::Flee || DeliveryNpc::IsCompanionCommand(Proposal.Action);
        if (PlayerMessage.IsValid() && Proposal.Speech.TrimStartAndEnd().IsEmpty() && !bVisibleAction)
        {
            Reason = TEXT("MissingPlayerReply");
            return false;
        }
        if (!PlayerMessage.IsValid())
        {
            SpeechTarget = nullptr;
            for (int32 Index = ActiveRequest.Evidence.Num() - 1; Index >= 0; --Index)
            {
                const auto& Message = ActiveRequest.Evidence[Index];
                if (!Message.bNeedsReply || (Message.Kind != TEXT("Speech") && Message.Kind != TEXT("NpcMessage")))
                    continue;
                if (ActiveRequest.Evidence.ContainsByPredicate(
                        [&Message](const auto& Item)
                        {
                            return Item.ReplyToEventId == Message.EventId &&
                                   (Item.Kind == TEXT("SelfSpeech") || Item.Kind == TEXT("ActionReply") ||
                                    (Message.Kind == TEXT("NpcMessage") && Item.Kind == TEXT("NpcMessageSent")));
                        }))
                    continue;
                PlayerMessage = Message.EventId;
                SpeechTarget = KnownSubjects.FindRef(Message.SubjectId).Get();
                break;
            }
        }
        // 台词对象和动作目标可以不同，例如看着小芳，却回答正在拱火的玩家
        if (ActiveRequest.bPlayerRequest && ActiveRequest.PlayerMessageId.IsValid())
        {
            PlayerMessage = ActiveRequest.PlayerMessageId;
            SpeechTarget = ActiveRequest.RequesterPC.IsValid() ? ActiveRequest.RequesterPC->GetPawn() : nullptr;
        }
        if (Proposal.bHasSpeechTarget)
        {
            SpeechTarget = KnownSubjects.FindRef(Proposal.SpeechTargetId).Get();
            if (Proposal.SpeechTargetId.IsValid() && (!IsValid(SpeechTarget) || SpeechTarget == GetOwner() ||
                                                      !Cast<APawn>(SpeechTarget) || SpeechTarget->GetWorld() != GetWorld()))
            {
                return RejectStale(TEXT("SpeechTargetChanged"));
            }
        }
    }
    if (JoinedListener)
    {
        PlayerMessage = JoinedEvent;
        SpeechTarget = JoinedListener;
    }
    // v1-5 保留原来的交战续期语义，v6 只有明确的 ContinueCurrent 可以保留活动
    const bool bKeepAction = ActiveRequest.RequestedCapabilityVersion < 6 && bUpdatingAction &&
                             PublicState.Action == Proposal.Action && PublicState.Target == Target;
    const double Now = GetWorld()->GetTimeSeconds();
    if (bKeepAction)
    {
        const double NewEnd = FMath::Min(Now + Proposal.Duration, CombatDeadline);
        const float Remaining = static_cast<float>(NewEnd - Now);
        auto* Npc = CastChecked<ADeliveryNpcCharacter>(GetOwner());
        if (Remaining <= 0 ||
            (Proposal.Action == EDeliveryNpcAction::Fight && !Npc->Combat->ContinueFight(PublicState.ActionId, Remaining)))
        {
            Reason = TEXT("CombatEnded");
            return false;
        }
        ActionEndsAt = NewEnd;
        ActionDeadline = NewEnd + 1;
        // 开始时间保持不变，客户端仍能接上原来的动作；本次回复使用新的编号
        PublicState.Duration = static_cast<float>(ServerTime() + Remaining - PublicState.ServerStartTime);
        PublicState.ReplyId = FGuid::NewGuid();
        ScheduleNextActionDecision();
    }
    else if (bUpdatingAction)
    {
        const FGuid PreviousAction = PublicState.ActionId;
        const EDeliveryNpcAction PreviousKind = PublicState.Action;
        const FGuid UpdatingRequest = ActiveRequest.RequestId;
        TGuardValue<bool> ApplyingUpdate(bApplyingActionUpdate, true);
        TGuardValue<bool> ReplacingFight(bReplacingFight, PreviousKind == EDeliveryNpcAction::Fight &&
                                                              Proposal.Action == EDeliveryNpcAction::Fight);
        // 全部校验通过后再停止旧拳，旧请求不能在停止通知中重入并换掉新安排
        if (!TryFinishAction(PreviousAction, EDeliveryNpcActionStatus::Cancelled, TEXT("ModelChangedAction")) || !IsAuthority() ||
            ActiveRequest.RequestId != UpdatingRequest || PublicState.ActionId != PreviousAction)
        {
            Reason = TEXT("ActionChangedDuringStop");
            return false;
        }
        if (PreviousKind == EDeliveryNpcAction::Fight || PreviousKind == EDeliveryNpcAction::Flee)
        {
            TGuardValue<bool> ChangingPlan(bChangingPlan, true);
            TArray<FGuid> PreviousReplies;
            for (const auto& Pair : Replies)
                if (Pair.Key != UpdatingRequest && Pair.Value.ActionId == PreviousAction && !Pair.Value.bContinueCurrent)
                    PreviousReplies.Add(Pair.Value.ReplyId);
            // 先撤下后端回复，再停整批旧声音，避免播放组件把已收到的台词当成插话撤回
            for (const FGuid& ReplyId : PreviousReplies)
                RemoveReply(ReplyId, TEXT("ModelChangedAction"));
            if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()); IsAuthority() && Npc)
                Npc->Presentation->CancelReplies(PreviousReplies);
            if (!IsAuthority() || ActiveRequest.RequestId != UpdatingRequest || PublicState.ActionId != PreviousAction)
            {
                Reason = TEXT("ActionChangedDuringStop");
                return false;
            }
        }
    }
    // 只记本次模型实际收到的事件，生成和播放期间听到的新话不能算已经处理
    LastDecisionEventIds.Reset(ActiveRequest.Evidence.Num());
    for (const auto& Item : ActiveRequest.Evidence)
        if (Item.Kind != DeliveryCompanionItemSearch::EvidenceKind && Item.Kind != TEXT("CompanionOwnerItemAvailable"))
            LastDecisionEventIds.Add(Item.EventId);
    ActionConversationDepth = ActiveRequest.ConversationDepth;
    if (!bKeepAction)
    {
        PublicState = {};
        PublicState.ActionId = FGuid::NewGuid();
        PublicState.ReplyId = PublicState.ActionId;
        PublicState.Status = EDeliveryNpcActionStatus::Accepted;
        PublicState.Duration = bCompanionPace ? DeliveryNpc::CandidateDuration(Proposal.Action) : Proposal.Duration;
        ActionEndsAt = 0;
        if (Proposal.Action != EDeliveryNpcAction::Fight && Proposal.Action != EDeliveryNpcAction::Flee)
            CombatDeadline = 0;
        // 行为树一直没有开始执行时也结束请求，避免人物永久停在接收状态
        ActionDeadline = Now + 1;
    }

    // 6. 写入公开状态：复制给客户端，行为树据此执行
    PublicState.ReplyToEventId = PlayerMessage;
    PublicState.PlanRevision = Proposal.PlanRevision;
    PublicState.Action = Proposal.Action;
    PublicState.Origin = EDeliveryNpcActionOrigin::Model;
    PublicState.Requester = RequestPlayerState.Get();
    PublicState.CompanionCommandRevision = ActiveRequest.CompanionCommandRevision;
    PublicState.CompanionAssistRevision = ActiveRequest.CompanionAssistRevision;
    PublicState.CompanionTravelMode =
        SelectedCandidate ? SelectedCandidate->TravelMode : EDeliveryCompanionTravelMode::Unspecified;
    PublicState.CompanionTask = ActiveRequest.CompanionTask.Get();
    PublicState.CompanionPackage = ActiveRequest.CompanionPackage.Get();
    PublicState.HomeChoice = MoveTemp(AcceptedHomeChoice);
    PublicState.ItemChoice = MoveTemp(AcceptedItemChoice);
    PublicState.Target = Target;
    PublicState.SpeechTarget = SpeechTarget;
    PublicState.Speech = Proposal.Speech.Left(120);
    // 同目标继续当前动作而没有新台词时，保留上一轮模型选择的表情
    if (!bKeepAction || !PublicState.Speech.TrimStartAndEnd().IsEmpty())
        PublicState.Emotion = Proposal.Emotion;
    PublicState.bStreamSpeech = false;
    ActionTargetId = Proposal.TargetId;
    ActionEvidenceIds = Proposal.EvidenceIds;
    Reason = NAME_None;
    return true;
}

// 请求发出时记录的当前活动，现在是否仍然一样
bool UDeliveryNpcAgentComponent::IsActivitySnapshotCurrent(const FDeliveryNpcDecisionRequest& Request) const
{
    if (Request.RunningActionId.IsValid())
    {
        if (!HasActiveAction() || Request.RunningActionId != PublicState.ActionId ||
            Request.RunningPlanRevision != PublicState.PlanRevision || Request.RunningAction != PublicState.Action ||
            Request.RunningOrigin != PublicState.Origin ||
            !Request.RunningTarget.HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(PublicState.Target.Get())) ||
            !Request.RunningRequester.HasSameIndexAndSerialNumber(TWeakObjectPtr<APlayerState>(PublicState.Requester.Get())) ||
            Request.RunningStartTime != PublicState.ServerStartTime || Request.RunningDuration != PublicState.Duration ||
            Request.RunningDeadline != ActionDeadline)
            return false;
        if (!Request.RunningTarget.IsExplicitlyNull() && !Request.RunningTarget.IsValid())
            return false;
        if (!Request.RunningRequester.IsExplicitlyNull() && !Request.RunningRequester.IsValid())
            return false;
    }
    else if (HasActiveAction())
        return false;
    if (Request.bHasCompanionContext)
    {
        const auto* Companion = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
        if (!Companion || (Request.bRunningCompanion && !Request.RunningCompanionOwner.IsValid()) ||
            !Request.RunningCompanionOwner.HasSameIndexAndSerialNumber(
                TWeakObjectPtr<APlayerState>(Companion->GetOwnerPlayerState())) ||
            Request.CompanionCommandRevision != Companion->GetCommandRevision() ||
            Request.RunningCompanionMode != uint8(Companion->GetMode()) ||
            Request.bRunningErrand != Companion->HasActiveErrand() ||
            Request.bRunningHome != Companion->HasActiveHomeArrangement())
            return false;
        FDeliveryNpcDecisionRequest Current;
        Companion->ExtendDecisionRequest(Current, Request.RequesterPlayerState.Get());
        if (!Request.CompanionTask.HasSameIndexAndSerialNumber(Current.CompanionTask) ||
            !Request.CompanionPackage.HasSameIndexAndSerialNumber(Current.CompanionPackage))
            return false;
        if (Request.bRunningHome && (Request.RunningHomeArrangement != Companion->GetHomeState().ArrangementId ||
                                     !Request.RunningHomeProp.HasSameIndexAndSerialNumber(
                                         TWeakObjectPtr<AActor>(Companion->GetHomeState().CarriedProp.Get()))))
            return false;
        if (Request.RequestedCapabilityVersion >= 7 &&
            (!Request.RunningHeldItem.HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(Companion->GetCarriedItem())) ||
             !Request.RunningErrandItem.HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(Companion->GetErrand().Item.Get())) ||
             (!Request.RunningHeldItem.IsExplicitlyNull() && !Request.RunningHeldItem.IsValid()) ||
             (!Request.RunningErrandItem.IsExplicitlyNull() && !Request.RunningErrandItem.IsValid())))
            return false;
        if (Request.bRunningCompanion && !HasCompanionActivity())
            return false;
    }
    return Request.RunningActionId.IsValid() || Request.bRunningCompanion;
}

// 同伴的委托在模型思考期间变了，需要重新请求时返回原因
FName UDeliveryNpcAgentComponent::GetRequestedCompanionErrandRefreshReason(EDeliveryNpcAction ProposedAction) const
{
    if (!bPlayerRequest || !ActiveRequest.bPlayerRequest || !ActiveRequest.bPendingPlayerMessage ||
        !ActiveRequest.bRequesterIsCompanionOwner || !ActiveRequest.bRunningErrand || !PlayerReplyMessage.IsValid() ||
        PlayerReplyMessage != ActiveRequest.PlayerMessageId)
        return NAME_None;
    const auto* Requester = ActiveRequest.RequesterPC.Get();
    auto* PlayerState = ActiveRequest.RequesterPlayerState.Get();
    const auto* Companion = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
    if (!IsValid(Requester) || Requester != PlayerReplyController.Get() || Requester->GetWorld() != GetWorld() ||
        Requester->PlayerState != PlayerState || ActiveRequest.RunningCompanionOwner.Get() != PlayerState || !Companion ||
        !Companion->CanCommand(PlayerState))
        return NAME_None;
    const auto Errand = Companion->GetErrand();
    // 只刷新仍在等待的同一主人问题，拒绝从旧快照继续已经结束的委托
    if (Errand.CommandRevision == ActiveRequest.CompanionCommandRevision && DeliveryNpc::IsTerminal(Errand.Status))
        return TEXT("CompanionErrandEnded");
    // 签收回调可能已接下新委托，原请求仍可按旧订单的真实完成状态刷新
    const auto* Task = ActiveRequest.CompanionTask.Get();
    const auto* Manager = UDeliveryTaskManagerComponent::Get(this);
    if (IsValid(Task) && Manager && !ActiveRequest.CompanionErrandTaskId.IsEmpty() &&
        ActiveRequest.CompanionErrandTaskId == ActiveRequest.CompanionTaskId &&
        ActiveRequest.CompanionTaskPhase == TEXT("InProgress") && Manager->GetTaskStatus(Task) == EDeliveryTaskStatus::Completed)
        return TEXT("CompanionErrandEnded");

    // 跑腿中的进度回答和包裹候选使用旧交接一侧时重判，直接停下或召回仍沿原命令入口
    const bool bReadsPackage =
        ProposedAction == EDeliveryNpcAction::ContinueCurrent || ProposedAction == EDeliveryNpcAction::Wait ||
        ProposedAction == EDeliveryNpcAction::LookAt ||
        (DeliveryNpc::IsCompanionErrand(ProposedAction) && !DeliveryNpc::IsCompanionItemErrand(ProposedAction));
    if (!bReadsPackage || ActiveRequest.bCompanionHasPackage || !IsValid(Task) || Errand.Task != Task ||
        Errand.CommandRevision != ActiveRequest.CompanionCommandRevision ||
        Errand.CommandRevision != Companion->GetCommandRevision() ||
        ActiveRequest.CompanionErrandAction != DeliveryNpc::ActionName(Errand.Action))
        return NAME_None;
    const bool bWasPickingUp =
        ActiveRequest.CompanionErrandPhase == TEXT("GoToPickup") && ActiveRequest.CompanionPackage.IsExplicitlyNull();
    const bool bWasReceiving = ActiveRequest.CompanionErrandPhase == TEXT("ReceiveFromOwner") &&
                               ActiveRequest.CompanionPackage.IsValid() &&
                               ActiveRequest.CompanionPackage.Get() == Errand.Package.Get();
    return (bWasPickingUp || bWasReceiving) && Companion->HasReceivedErrandPackage() ? FName(TEXT("CompanionErrandAdvanced"))
                                                                                     : NAME_None;
}

// 校验 ContinueCurrent：当前活动仍然一样才保留原安排
bool UDeliveryNpcAgentComponent::AcceptContinueCurrent(const FDeliveryNpcDecisionProposal& Proposal,
                                                       EDeliveryCompanionSpeechPreference Preference, FName& Reason)
{
    if (ActiveRequest.bPendingPlayerMessage && Proposal.Speech.TrimStartAndEnd().IsEmpty())
    {
        Reason = TEXT("MissingPlayerReply");
        return false;
    }
    Reason = TEXT("StaleActivity");
    if (ActiveRequest.RequestedCapabilityVersion < 6 || Proposal.Duration != 0.f || Proposal.TargetId.IsValid() ||
        !Proposal.EvidenceIds.IsEmpty() || !IsActivitySnapshotCurrent(ActiveRequest))
        return false;
    AActor* Listener = nullptr;
    FGuid ReplyTo = ActiveRequest.PlayerMessageId;
    if (auto* Turns = BoundNpcTurns.Get(); Turns && ActiveRequest.ParentNpcMessage.IsValid())
    {
        FDeliveryNpcMessage Parent;
        if (Turns->GetNpcMessage(ActiveRequest.ParentNpcMessage, Cast<ADeliveryNpcCharacter>(GetOwner()), Parent))
        {
            Listener = Parent.Sender.Get();
            ReplyTo = Parent.Handle.MessageId;
        }
    }
    if (ActiveRequest.RequesterPC.IsValid())
        Listener = ActiveRequest.RequesterPC->GetPawn();
    if (Proposal.bHasSpeechTarget)
    {
        Listener = KnownSubjects.FindRef(Proposal.SpeechTargetId).Get();
        if (Proposal.SpeechTargetId.IsValid() &&
            (!IsValid(Listener) || !Cast<APawn>(Listener) || Listener == GetOwner() || Listener->GetWorld() != GetWorld()))
        {
            Reason = TEXT("InvalidSpeechTarget");
            return false;
        }
    }
    if (const FGuid JoinedEvent = GetJoinedResponseEvent(ActiveRequest); JoinedEvent.IsValid() && !ActiveRequest.bPlayerRequest)
    {
        ReplyTo = JoinedEvent;
        Listener = GetJoinedResponseListener();
        if (!Listener)
        {
            Reason = TEXT("JoinedOwnerUnavailable");
            return false;
        }
    }
    if (Preference != EDeliveryCompanionSpeechPreference::Unspecified)
    {
        auto* Companion = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
        const auto* Requester = ActiveRequest.RequesterPC.Get();
        auto* RequesterState = ActiveRequest.RequesterPlayerState.Get();
        auto* CurrentPawn = Requester ? Requester->GetPawn<APawn>() : nullptr;
        Reason = TEXT("SpeechPreferenceRequiresOwnerMessage");
        if (ActiveRequest.RequestedCapabilityVersion < 8 || !ActiveRequest.bPlayerRequest ||
            !ActiveRequest.bPendingPlayerMessage || !IsValid(Requester) || Requester != PlayerReplyController.Get() ||
            Requester->PlayerState != RequesterState || !IsValid(CurrentPawn) || CurrentPawn->GetWorld() != GetWorld() ||
            !Companion || !Companion->ApplySpeechPreference(RequesterState, ActiveRequest.CompanionCommandRevision, Preference))
            return false;
        Listener = CurrentPawn;
    }
    LastDecisionEventIds.Reset(ActiveRequest.Evidence.Num());
    for (const auto& Item : ActiveRequest.Evidence)
        if (Item.Kind != DeliveryCompanionItemSearch::EvidenceKind && Item.Kind != TEXT("CompanionOwnerItemAvailable"))
            LastDecisionEventIds.Add(Item.EventId);
    // 只更新本轮回复，不重新执行 GAS/BT，也不延长原来的活动期限
    PublicState.ReplyId = FGuid::NewGuid();
    PublicState.ReplyToEventId = ReplyTo;
    PublicState.SpeechTarget = Listener;
    PublicState.Speech = Proposal.Speech.Left(120);
    PublicState.Emotion = Proposal.Emotion;
    PublicState.bStreamSpeech = false;
    ActionConversationDepth = ActiveRequest.ConversationDepth;
    Reason = NAME_None;
    return true;
}

// 非流式回复或请求失败时的统一入口：校验建议、记录结果、按需重试
void UDeliveryNpcAgentComponent::ReceiveDecision(FGuid RequestId, const FDeliveryNpcDecisionProposal* Proposal, FName Error)
{
    if (!IsAuthority() || bChangingPlan || !RequestId.IsValid() || RequestId != ActiveRequest.RequestId)
        return;
    const FGuid TraceWorldEpoch = ActiveRequest.WorldEpoch;
    if (Proposal)
        TraceProposal(*Proposal);
    const FGuid RequestedPlayerMessage = ActiveRequest.PlayerMessageId;
    const auto RequestedPlayerController = ActiveRequest.RequesterPC;

    // 校验建议
    TOptional<FReply> AcceptedReply;
    bool bAccepted = Error.IsNone() && Proposal && TryAcceptProposal(*Proposal, Error);
    // 初遇主动请求用尽后保留事件，不让通用传输重试再叠加一轮
    if (!bAccepted && !ActiveRequest.bPlayerRequest && GetJoinedResponseEvent(ActiveRequest).IsValid() &&
        JoinedResponseAttempts >= 3)
    {
        UE_LOG(LogDeliveryNpcAgent, Warning, TEXT("CompanionJoined response deferred npc=%s event=%s reason=%s attempts=%d"),
               *GetNameSafe(GetOwner()), *PendingJoinedEvent.ToString(), *Error.ToString(), JoinedResponseAttempts);
        Error = TEXT("JoinedResponseDeferred");
    }
    TraceValidation(TraceWorldEpoch, RequestId, bAccepted, Error);
    const bool bRetry = bDecisionQueued && !bAccepted;
    const bool bContinueConversation = bDecisionQueued && bAccepted;
    FGuid FailedMessage;
    AActor* FailedSpeaker = nullptr;
    if (!bAccepted)
        GetRequestedPlayerMessage(FailedMessage, FailedSpeaker);
    const bool bFinishingCurrentPlayer =
        !bPlayerRequest || (ActiveRequest.PlayerMessageId.IsValid() && ActiveRequest.PlayerMessageId == PlayerReplyMessage);
    if (bAccepted)
    {
        FReply Reply;
        BindJoinedResponse(Reply, ActiveRequest);
        Reply.ReplyId = PublicState.GetReplyId();
        Reply.MessageId = PublicState.ReplyToEventId;
        Reply.RequesterPC = ActiveRequest.RequesterPC;
        Reply.RequesterPlayerState = ActiveRequest.RequesterPlayerState;
        Reply.ActionId = PublicState.ActionId;
        Reply.Action = PublicState.Action;
        Reply.PlanRevision = ActiveRequest.PlanRevision;
        Reply.bModelFinished = true;
        Reply.bTransferFinished = true;
        Reply.bContinueCurrent = Proposal->Action == EDeliveryNpcAction::ContinueCurrent;
        Reply.ApprovedText = Proposal->Speech;
        Reply.bHasApprovedText = DeliveryNpcMessages::IsValidReplyText(Reply.ApprovedText);
        Reply.SentenceCount = Reply.ApprovedText.IsEmpty() ? 0 : 1;
        if (RegisterReplyMessageContext(Reply, ActiveRequest, true))
        {
            Replies.Add(RequestId, Reply);
            AcceptedReply = Reply;
        }
        else
        {
            bAccepted = false;
            Error = TEXT("NpcMessageParentChanged");
            if (Reply.bOwnsAction)
                TryFinishAction(Reply.ActionId, EDeliveryNpcActionStatus::Cancelled, Error);
        }
    }

    // 本次请求结束，日志 Decision ... accepted= reason=
    ActiveRequest = {};
    bDecisionPending = false;
    bDecisionQueued = false;
    UE_LOG(LogDeliveryNpcAgent, Display, TEXT("Decision npc=%s request=%s accepted=%d reason=%s"), *NpcId.ToString(),
           *RequestId.ToString(), bAccepted, *Error.ToString());
    if (bAccepted)
    {
        TryDeliverReplyMessage(RequestId);
        if (!IsAuthority())
            return;
        PublishState();
        // 普通 HTTP 回复也可能续接同一段行动，使用本次已保存的回复，回调后的新 PublicState 不能冒充旧台词
        if (!IsAuthority())
            return;
        if (AcceptedReply.IsSet() && Replies.Contains(RequestId))
        {
            const FReply Reply = AcceptedReply.GetValue();
            if ((Reply.bActionStarted || Reply.bContinueCurrent) && !Reply.ApprovedText.IsEmpty())
                if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
                    if (!Npc->Presentation->PublishSpeech(Reply.ApprovedText, Reply.Emotion, Reply.Depth, Reply.MessageId,
                                                          Reply.ReplyId))
                        CancelReply(Reply.ReplyId);
        }
    }
    if (bAccepted && bFinishingCurrentPlayer &&
        (!bPlayerRequest || (PlayerReplyMessage == RequestedPlayerMessage && PlayerReplyController == RequestedPlayerController)))
    {
        bPlayerRequest = false;
        const bool bAwaitingAction = HasActiveAction() && PlayerReplyMessage.IsValid() &&
                                     PlayerReplyMessage == PublicState.ReplyToEventId &&
                                     PublicState.Speech.TrimStartAndEnd().IsEmpty();
        if (!bAwaitingAction)
        {
            PlayerReplyMessage.Invalidate();
            PlayerReplyController.Reset();
        }
        EndListening();
    }
    if (bAccepted && AcceptedReply.IsSet() && AcceptedReply->bContinueCurrent &&
        AcceptedReply->ApprovedText.TrimStartAndEnd().IsEmpty())
        FinishReplyPlayback(AcceptedReply->ReplyId);
    // 终态通知只能处理原请求，新请求已由同步回调开始时不重置它的唤醒与重试状态
    if (!IsAuthority() || (ActiveRequest.RequestId.IsValid() && ActiveRequest.RequestId != RequestId))
        return;

    // 通知角色安排重试或下一次决策
    OnDecisionFinished.Broadcast(Error);
    if (!IsAuthority())
        return;
    auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    // 完成通知会结束本轮等待，新话需要在当前动作和台词结束后继续处理
    if (bContinueConversation && Npc)
        Npc->WakeDecision();
    const bool bRetryAllowed =
        !FailedMessage.IsValid() || FailedMessage != PlayerReplyMessage || (Npc && Npc->IsDecisionRetryScheduled());
    const bool bRetryStarted = bRetry && bRetryAllowed && !IsDecisionPending() &&
                               (!HasActiveAction() || CanUpdateRunningAction() || bPlayerRequest) && RequestDecision();
    if (!bAccepted)
    {
        FinishFailedPlayerReply(FailedMessage, FailedSpeaker, bRetryStarted || (Npc && Npc->IsDecisionRetryScheduled()));
    }
    GetOwner()->ForceNetUpdate();
}

// 当前请求在回答哪位玩家的哪句话
void UDeliveryNpcAgentComponent::GetRequestedPlayerMessage(FGuid& OutMessage, AActor*& OutSpeaker) const
{
    OutMessage.Invalidate();
    OutSpeaker = nullptr;
    if (!bPlayerRequest || !ActiveRequest.bPlayerRequest || !PlayerReplyMessage.IsValid())
        return;
    const auto* Message = ActiveRequest.Evidence.FindByPredicate(
        [this](const auto& Item)
        {
            return Item.Kind == TEXT("PlayerSpeech") && Item.EventId == PlayerReplyMessage;
        });
    if (!Message)
        return;
    OutMessage = Message->EventId;
    OutSpeaker = KnownSubjects.FindRef(Message->SubjectId).Get();
}

// 回答玩家失败：会重试时保留，不再重试时通知交谈面板
void UDeliveryNpcAgentComponent::FinishFailedPlayerReply(FGuid MessageId, AActor* Speaker, bool bWillRetry)
{
    // 失败通知只处理原来那句玩家发言，新的插话和新请求继续按自己的状态执行
    if (!MessageId.IsValid() || PlayerReplyMessage != MessageId || ActiveRequest.RequestId.IsValid() ||
        (ListeningMessage.IsValid() && ListeningMessage != MessageId))
        return;
    if (bWillRetry)
    {
        // 模型已选动作后若连接中断，重试期间继续看着原来的玩家，不重播短呼声
        if (!IsListening() && IsValid(Speaker) && Speaker->GetWorld() == GetWorld())
        {
            ListeningSpeaker = Speaker;
            ListeningMessage = MessageId;
            GetOwner()->ForceNetUpdate();
        }
        return;
    }
    bPlayerRequest = false;
    PlayerReplyMessage.Invalidate();
    auto* Controller = PlayerReplyController.Get();
    PlayerReplyController.Reset();
    EndListening();
    // 玩家换 Pawn 后仍通知原来的控制器，不把消息发给后来占有该 Pawn 的人
    if (auto* Conversation = Controller && Controller->GetWorld() == GetWorld()
                                 ? Controller->FindComponentByClass<UDeliveryConversationComponent>()
                                 : nullptr)
        Conversation->ClientResult(TEXT("ReplyFailed"), MessageId);
    TArray<FGuid> FailedReplies;
    for (const auto& Pair : Replies)
        if (Pair.Value.MessageId == MessageId && Pair.Value.RequesterPC.Get() == Controller)
            FailedReplies.Add(Pair.Value.ReplyId);
    for (const FGuid& ReplyId : FailedReplies)
        RemoveReply(ReplyId, TEXT("ReplyFailed"));
    if (Controller && FailedReplies.IsEmpty())
        OnPlayerReplyEnded.Broadcast(Controller, MessageId, TEXT("ReplyFailed"));
}

// 是否有已采纳或正在执行的动作
bool UDeliveryNpcAgentComponent::HasActiveAction() const
{
    return PublicState.Status == EDeliveryNpcActionStatus::Accepted || PublicState.Status == EDeliveryNpcActionStatus::Running;
}

// 交战或逃跑期间，模型能否决定下一步
bool UDeliveryNpcAgentComponent::CanUpdateRunningAction() const
{
    if (!IsAuthority() || bChangingPlan || PublicState.Status != EDeliveryNpcActionStatus::Running ||
        PublicState.Origin != EDeliveryNpcActionOrigin::Model ||
        (PublicState.Action != EDeliveryNpcAction::Fight && PublicState.Action != EDeliveryNpcAction::Flee))
        return false;
    const auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || Npc->IsKnockedDown() || !IsValid(PublicState.Target) || PublicState.Target->GetWorld() != GetWorld() ||
        GetWorld()->GetTimeSeconds() >= ActionEndsAt || GetWorld()->GetTimeSeconds() >= CombatDeadline)
        return false;
    return PublicState.Action != EDeliveryNpcAction::Fight || Npc->Combat->GetFightStopReason(PublicState.ActionId).IsNone();
}

// 交战中只给交战相关的选项，其余交给角色 CanExecuteForRequester 判断
bool UDeliveryNpcAgentComponent::CanChooseAction(EDeliveryNpcAction Action, AActor* Target, APlayerState* Requester) const
{
    const auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc)
        return false;
    // 追打和逃跑已经会跟踪对手，交战中由模型明确选择继续、退开或停手
    if (!bPlayerRequest && CanUpdateRunningAction() && Action != EDeliveryNpcAction::Fight &&
        Action != EDeliveryNpcAction::Flee && Action != EDeliveryNpcAction::Wait)
        return false;
    return Npc->CanExecuteForRequester(Action, Target, Requester);
}

// 当前动作还剩多少秒
double UDeliveryNpcAgentComponent::GetActionTimeRemaining() const
{
    return GetWorld() ? FMath::Max(0.0, ActionEndsAt - GetWorld()->GetTimeSeconds()) : 0;
}

// 交战中安排下一次决策的时间
void UDeliveryNpcAgentComponent::ScheduleNextActionDecision()
{
    NextActionDecisionAt =
        GetWorld()->GetTimeSeconds() + FMath::Max(.25, GetActionTimeRemaining() - DeliveryNpc::CombatDecisionLeadSeconds);
}

// 交谈流程第 16 步，服务器：行为树领取动作前做最后一次检查
bool UDeliveryNpcAgentComponent::StartAction(FDeliveryNpcPublicState& OutAction)
{
    if (!IsAuthority() || bChangingPlan || PublicState.Status != EDeliveryNpcActionStatus::Accepted)
        return false;
    if (PublicState.Origin == EDeliveryNpcActionOrigin::Model && PublicState.ActionId == NpcMessageActionId &&
        PublicState.PlanRevision == NpcMessageActionPlan && !IsNpcMessageParentCurrent(NpcMessageActionParent))
    {
        TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Cancelled, TEXT("NpcMessageParentChanged"));
        return false;
    }

    // 引用的事件仍在、期限未过，并再次确认能执行
    PruneEvidence();
    const auto* ItemChoice = PublicState.ItemChoice.Get();
    const bool bSearchLead = ItemChoice && ItemChoice->UsesRequestEvidence();
    bool bEvidenceValid;
    if (bSearchLead)
    {
        // HTTP 回执后原请求已回收，开始执行时核对随动作保存的同一份线索
        const auto* Companion = GetOwner()->FindComponentByClass<UDeliveryCompanionComponent>();
        bEvidenceValid = Companion && DeliveryNpc::IsCompanionItemErrand(PublicState.Action) &&
                         ItemChoice->GetAction() == PublicState.Action && PublicState.Target == ItemChoice->Item.Get() &&
                         ActionTargetId == ItemChoice->SubjectId && ActionEvidenceIds.Num() == 1 &&
                         ActionEvidenceIds[0] == ItemChoice->EvidenceId &&
                         Companion->CanApplyItemChoice(*ItemChoice, PublicState.Requester, PublicState.CompanionCommandRevision);
    }
    else if (PublicState.Origin == EDeliveryNpcActionOrigin::GameplayRule)
        bEvidenceValid = IsGameplayRuleActionValid();
    else
        bEvidenceValid = Algo::AllOf(ActionEvidenceIds,
                                     [this](const FGuid& Id)
                                     {
                                         return Evidence.ContainsByPredicate(
                                             [&Id](const auto& Item)
                                             {
                                                 return Item.EventId == Id;
                                             });
                                     });
    if (GetWorld()->GetTimeSeconds() >= ActionDeadline || !bEvidenceValid ||
        (PublicState.Action == EDeliveryNpcAction::LookAt &&
         (!KnownSubjects.Contains(ActionTargetId) || !CanLookAt(PublicState.Target))))
    {
        TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Failed, TEXT("PreconditionChanged"));
        return false;
    }
    if (PublicState.Origin == EDeliveryNpcActionOrigin::GameplayRule)
    {
        if (!IsGameplayRuleActionValid())
        {
            TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Failed, TEXT("GameplayRuleExpired"));
            return false;
        }
    }
    else if (auto* Resident = Cast<ADeliveryNpcCharacter>(GetOwner()); Resident && !bSearchLead)
        if (!Resident->CanExecuteForRequester(PublicState.Action, PublicState.Target, PublicState.Requester))
        {
            TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Failed, TEXT("PreconditionChanged"));
            return false;
        }
    for (auto& Pair : Replies)
    {
        auto& Reply = Pair.Value;
        if (Reply.bPlayerReply || !Reply.JoinedEventId.IsValid() || Reply.ActionId != PublicState.ActionId ||
            Reply.ReplyId != PublicState.GetReplyId())
            continue;
        auto* Listener = GetJoinedResponseListener();
        if (!Listener || Reply.JoinedEventId != PendingJoinedEvent ||
            !Reply.JoinedOwner.HasSameIndexAndSerialNumber(PendingJoinedOwner))
        {
            TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Failed, TEXT("JoinedOwnerUnavailable"));
            return false;
        }
        // 模型已选好的原台词仍属于这次道谢，行为树开始时使用主人实际控制对象
        Reply.Listener = Listener;
        PublicState.SpeechTarget = Listener;
        break;
    }

    // 开始执行：记录开始时间和结束时间
    PublicState.Status = EDeliveryNpcActionStatus::Running;
    PublicState.ServerStartTime = ServerTime();
    const double Now = GetWorld()->GetTimeSeconds();
    if (PublicState.Action == EDeliveryNpcAction::GetUp)
        PublicState.Duration = FMath::Max(PublicState.Duration, UDeliveryNpcCombatComponent::RecoveryMotionSeconds);
    ActionEndsAt = Now + PublicState.Duration;
    if (PublicState.Origin == EDeliveryNpcActionOrigin::Model &&
        (PublicState.Action == EDeliveryNpcAction::Fight || PublicState.Action == EDeliveryNpcAction::Flee))
    {
        CombatEventDecisionAt = -1;
        if (CombatDeadline <= 0)
            CombatDeadline = Now + DeliveryNpc::MaxContinuousCombatSeconds;
        ActionEndsAt = FMath::Min(ActionEndsAt, CombatDeadline);
        PublicState.Duration = static_cast<float>(ActionEndsAt - Now);
        if (PublicState.Duration <= 0)
        {
            TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Failed, TEXT("CombatTimeLimit"));
            return false;
        }
        ScheduleNextActionDecision();
        if (bRetaliationNeedsReply)
            NextActionDecisionAt = Now + .25;
    }
    bRetaliationNeedsReply = false;
    ActionDeadline = ActionEndsAt + 1.0;
    for (auto& Pair : Replies)
        if (Pair.Value.ActionId == PublicState.ActionId && !Pair.Value.RequiresCompanionCommandReceipt())
            Pair.Value.bActionStarted = true;

    // 把状态交给行为树，并通知客户端
    OutAction = PublicState;
    TraceAction(OutAction.ActionId, TEXT("ActionStarted"));
    PublishState();
    // 状态通知可能同步取消动作，行为树不能接着执行已经取消的那一份状态
    return IsAuthority() && PublicState.ActionId == OutAction.ActionId && PublicState.Status == EDeliveryNpcActionStatus::Running;
}

// 交谈流程第 23 步，服务器：动作结束，停止执行并把结果写成事件
bool UDeliveryNpcAgentComponent::TryFinishAction(FGuid ActionId, EDeliveryNpcActionStatus Result, FName Reason)
{
    if (!IsAuthority() || !HasActiveAction() || ActionId != PublicState.ActionId || !DeliveryNpc::IsTerminal(Result) ||
        (Result == EDeliveryNpcActionStatus::Succeeded && PublicState.Status != EDeliveryNpcActionStatus::Running))
        return false;
    TGuardValue<bool> ChangingPlan(bChangingPlan, true);
    const bool bGameplayRule = PublicState.Origin == EDeliveryNpcActionOrigin::GameplayRule;
    const bool bNeverStarted = PublicState.Status == EDeliveryNpcActionStatus::Accepted;
    const bool bCompanionCommand = DeliveryNpc::IsCompanionCommand(PublicState.Action);
    FGuid RetryPlayerMessage;
    TWeakObjectPtr<APlayerController> RetryPlayerController;
    if (!bApplyingActionUpdate)
    {
        // 已结束行动的后台回复失效，倒地恢复或换目标后也不能让旧决定重新生效
        if (ActiveRequest.RunningActionId == ActionId && !Replies.Contains(ActiveRequest.RequestId))
        {
            const auto Expired = ActiveRequest;
            const auto* Companion = Cast<ADeliveryCompanionCharacter>(GetOwner());
            const bool bOwnerCanRetryPace =
                Result == EDeliveryNpcActionStatus::Failed && PublicState.Action == EDeliveryNpcAction::Pace && Companion &&
                !Companion->IsKnockedDown() && Companion->Companion->CanCommand(Expired.RequesterPlayerState.Get());
            // 动作结束后重新判断现场，保留踱步受阻时主人尚未处理的召回等请求
            const bool bKeepPlayerMessage = (Result == EDeliveryNpcActionStatus::Succeeded || bOwnerCanRetryPace) &&
                                            Expired.bPlayerRequest && Expired.bPendingPlayerMessage && bPlayerRequest &&
                                            Expired.PlayerMessageId.IsValid() && Expired.PlayerMessageId == PlayerReplyMessage &&
                                            Expired.RequesterPC.IsValid() && Expired.RequesterPC == PlayerReplyController;
            if (bKeepPlayerMessage)
            {
                RetryPlayerMessage = Expired.PlayerMessageId;
                RetryPlayerController = Expired.RequesterPC;
            }
            ActiveRequest = {};
            bDecisionPending = false;
            bDecisionQueued = false;
            if (auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>())
                Gateway->CancelRequest(this, Expired.RequestId);
            if (!bKeepPlayerMessage && Expired.PlayerMessageId == PlayerReplyMessage)
            {
                PlayerReplyMessage.Invalidate();
                PlayerReplyController.Reset();
                bPlayerRequest = false;
                EndListening();
            }
            if (!bKeepPlayerMessage && Expired.RequesterPC.IsValid())
                OnPlayerReplyEnded.Broadcast(Expired.RequesterPC.Get(), Expired.PlayerMessageId, TEXT("ActivityEnded"));
            UE_LOG(LogDeliveryNpcAgent, Log,
                   TEXT("NpcLatency request=%s speech=%s stage=activity_request_ended retry_player=%d result=%s reason=%s"),
                   *Expired.RequestId.ToString(), *Expired.PlayerMessageId.ToString(), bKeepPlayerMessage,
                   *StaticEnum<EDeliveryNpcActionStatus>()->GetNameStringByValue(static_cast<int64>(Result)), *Reason.ToString());
        }
        CombatDeadline = 0;
        CombatEventDecisionAt = -1;
        LastCombatDecisionAt = -1;
    }
    ActionEndsAt = 0;
    // 先记下动作已经结束，再发送通知，避免通知过程中再次结束同一动作
    PublicState.Status = Result;
    PublicState.Reason = Reason;
    const auto* FightingNpc = Cast<ADeliveryNpcCharacter>(GetOwner());
    const FString FightDescription = PublicState.Action == EDeliveryNpcAction::Fight && FightingNpc
                                         ? FightingNpc->Combat->DescribeFight(ActionId, Reason)
                                         : FString();
    FString FleeDescription;
    if (PublicState.Action == EDeliveryNpcAction::Flee)
    {
        const TCHAR* Stop = TEXT("这次逃跑动作已停止");
        if (Reason == TEXT("FleeReachedDistance"))
            Stop = TEXT("已经与对方拉开距离，停止这次逃跑");
        else if (Reason == TEXT("FleeTimeLimit") || Reason == TEXT("FleeTravelLimit"))
            Stop = TEXT("这次短程逃跑结束");
        else if (Reason == TEXT("FleePathUnavailable"))
            Stop = TEXT("前面的路走不通，我停下了");
        else if (Reason == TEXT("ReconsiderAfterHit"))
            Stop = TEXT("逃跑时挨了一拳，我停下重新判断");
        else if (Reason == TEXT("FleeTargetLost"))
            Stop = TEXT("原先躲避的对象已经不在当前场景里");
        FleeDescription = Stop;
        if (IsValid(PublicState.Target))
            FleeDescription +=
                FString::Printf(TEXT("，当前与对方相距约 %.0f 厘米"),
                                FVector::Dist2D(GetOwner()->GetActorLocation(), PublicState.Target->GetActorLocation()));
        FleeDescription += TEXT("。这不代表对方已经放弃追赶。");
    }
    // 先停止移动和尚未发生的命中，避免打断后到下一帧仍补出一拳
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
        Npc->StopAction(ActionId);
    // 停止能力期间可能换主人或销毁，旧动作不能把结果写回已清空的上下文
    if (!IsAuthority() || PublicState.ActionId != ActionId)
        return true;
    TArray<FGuid, TInlineAllocator<1>> ConfirmedReplies;
    if (bCompanionCommand && Result == EDeliveryNpcActionStatus::Succeeded)
        for (auto& Pair : Replies)
            if (Pair.Value.ActionId == ActionId && Pair.Value.RequiresCompanionCommandReceipt())
            {
                Pair.Value.bActionStarted = true;
                ConfirmedReplies.Add(Pair.Key);
            }
    UE_LOG(LogDeliveryNpcAgent, Display, TEXT("Action npc=%s action=%s revision=%d result=%s reason=%s"), *NpcId.ToString(),
           *ActionId.ToString(), PublicState.PlanRevision,
           *StaticEnum<EDeliveryNpcActionStatus>()->GetNameStringByValue(static_cast<int64>(Result)), *Reason.ToString());
    TraceAction(ActionId, TEXT("ActionFinished"));
    AActor* FinishedTarget = PublicState.Target;
    PublicState.Target = nullptr;
    ActionEvidenceIds.Reset();
    ActionTargetId.Invalidate();

    // 写入 ActionFinished 事件，模型下一轮能看到这次结果
    FDeliveryNpcObservation ResultEvent;
    ResultEvent.EventId = FGuid::NewGuid();
    ResultEvent.Kind = TEXT("ActionFinished");
    ResultEvent.Source = EDeliveryNpcEvidenceSource::ActionResult;
    ResultEvent.Subject = GetOwner();
    const bool bFinishedLead = PublicState.ItemChoice.IsValid() && PublicState.ItemChoice->UsesRequestEvidence();
    ResultEvent.Addressee = !bFinishedLead && IsValid(FinishedTarget) ? FinishedTarget : nullptr;
    ResultEvent.Description = FString::Printf(
        TEXT("%s %s %s"), *DeliveryNpc::ActionName(PublicState.Action),
        *StaticEnum<EDeliveryNpcActionStatus>()->GetNameStringByValue(static_cast<int64>(Result)), *Reason.ToString());
    if (!FightDescription.IsEmpty())
        ResultEvent.Description = FightDescription;
    else if (!FleeDescription.IsEmpty())
        ResultEvent.Description = FleeDescription;
    ResultEvent.Lifetime = 30.f;
    ObserveEvent(ResultEvent);
    RecordActionReply();
    // 同伴指令已进入行为树也可能被拒绝，清理它预收的回复
    if ((bNeverStarted || bCompanionCommand) && Result != EDeliveryNpcActionStatus::Succeeded)
    {
        TArray<FGuid> UnstartedReplies;
        for (const auto& Pair : Replies)
            if (Pair.Value.ActionId == ActionId && !Pair.Value.bActionStarted &&
                (bNeverStarted || Pair.Value.RequiresCompanionCommandReceipt()))
                UnstartedReplies.Add(Pair.Key);
        // 活动已经终止，尚未兑现的行动台词按真实失败结束或重试，不能一直等开口
        TGuardValue<bool> EndingReply(bChangingPlan, false);
        for (const FGuid& RequestId : UnstartedReplies)
        {
            const auto* Reply = Replies.Find(RequestId);
            if (Reply && Reply->RequiresCompanionCommandReceipt())
                FailReply(RequestId);
            else
                FinishReply(RequestId, false);
        }
        if (!IsAuthority() || PublicState.ActionId != ActionId)
            return true;
    }
    // 回复已结束时处理动作回执；等待重试的玩家仍要收到下一次请求的结果
    if (!bDecisionPending && !bPlayerRequest && PlayerReplyMessage == PublicState.ReplyToEventId)
    {
        const bool bSilent = PublicState.bStreamSpeech
                                 ? FightingNpc && FightingNpc->Presentation->IsReplyWithoutSpeech(PublicState.GetReplyId())
                                 : PublicState.Speech.TrimStartAndEnd().IsEmpty();
        if (Result != EDeliveryNpcActionStatus::Succeeded && bSilent)
            FinishFailedPlayerReply(PublicState.ReplyToEventId, nullptr, false);
        PlayerReplyMessage.Invalidate();
        PlayerReplyController.Reset();
    }
    PublishState();
    const bool bRetryPlayer = RetryPlayerMessage.IsValid() && RetryPlayerMessage == PlayerReplyMessage &&
                              RetryPlayerController.IsValid() && RetryPlayerController == PlayerReplyController && bPlayerRequest;
    if ((bGameplayRule || bRetryPlayer) && IsAuthority() && PublicState.ActionId == ActionId)
        if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
            Npc->WakeDecision();
    // 快文本只在真实确认后尝试，已准备的语音继续由原播放队列处理
    if (!ConfirmedReplies.IsEmpty())
    {
        TGuardValue<bool> SendingReply(bChangingPlan, false);
        for (const FGuid& RequestId : ConfirmedReplies)
            TryDeliverReplyMessage(RequestId);
    }
    return true;
}

// 动作成功且带台词时，记下“已回答”
void UDeliveryNpcAgentComponent::RecordActionReply()
{
    if (PublicState.Status != EDeliveryNpcActionStatus::Succeeded || !PublicState.ReplyToEventId.IsValid())
        return;
    const auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    const bool bSilent = PublicState.bStreamSpeech ? Npc && Npc->Presentation->IsReplyWithoutSpeech(PublicState.GetReplyId())
                                                   : PublicState.Speech.TrimStartAndEnd().IsEmpty();
    if (!bSilent || Evidence.ContainsByPredicate(
                        [this](const auto& Item)
                        {
                            return Item.Kind == TEXT("ActionReply") && Item.ReplyToEventId == PublicState.ReplyToEventId;
                        }))
        return;
    // 动作成功且确认没有台词后，才记录这次回应；台词记忆仍由播放完成时写入
    FDeliveryNpcObservation Reply;
    Reply.Kind = TEXT("ActionReply");
    Reply.Source = EDeliveryNpcEvidenceSource::ActionResult;
    Reply.Subject = GetOwner();
    Reply.Addressee = IsValid(PublicState.SpeechTarget) ? PublicState.SpeechTarget.Get() : nullptr;
    Reply.ReplyToEventId = PublicState.ReplyToEventId;
    Reply.Lifetime = 60;
    if (PublicState.Reason == TEXT("CompanionErrandContinued"))
        Reply.Description = TEXT("原委托继续执行，这次没有台词");
    else if (PublicState.Reason == TEXT("CompanionErrandAccepted"))
        Reply.Description = TEXT("二狗已接下委托，进度由游戏确认，这次没有台词");
    else if (PublicState.Action == EDeliveryNpcAction::Follow && PublicState.Reason == TEXT("CompanionModeChanged"))
        Reply.Description = TEXT("Follow 指令已接收，持续跟随主人，这次没有台词");
    else
        Reply.Description = FString::Printf(TEXT("%s 动作已完成，这次没有台词"), *DeliveryNpc::ActionName(PublicState.Action));
    ObserveEvent(Reply);
    if (PlayerReplyMessage != Reply.ReplyToEventId)
        return;
    auto* Controller = PlayerReplyController.Get();
    PlayerReplyMessage.Invalidate();
    PlayerReplyController.Reset();
    // 控制器换过 Pawn 也只通知原玩家，界面继续按发言编号过滤旧回执
    if (auto* Conversation = Controller && Controller->GetWorld() == GetWorld()
                                 ? Controller->FindComponentByClass<UDeliveryConversationComponent>()
                                 : nullptr)
        Conversation->ClientResult(TEXT("ActionReply"), Reply.ReplyToEventId);
    FinishReplyPlayback(PublicState.GetReplyId());
}

// 取消请求和当前动作，已发生的命中不撤销
void UDeliveryNpcAgentComponent::CancelPlan(FName Reason)
{
    if (!IsAuthority())
        return;
    TGuardValue<bool> ChangingPlan(bChangingPlan, true);
    CancelDecision();
    TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Cancelled, Reason);
    EndListening();
    bPlayerRequest = false;
    PlayerReplyMessage.Invalidate();
    PlayerReplyController.Reset();
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
        Npc->Presentation->CancelSpeech();
}

// 同伴换主人时清空对话和经历，旧请求失效
void UDeliveryNpcAgentComponent::ResetContext(FName Reason)
{
    if (!IsAuthority())
        return;
    CancelPlan(Reason);
    PendingJoinedEvent.Invalidate();
    PendingJoinedOwner.Reset();
    PendingJoinedSubject.Reset();
    JoinedResponseAttempts = 0;
    Evidence.Reset();
    KnownSubjects.Reset();
    RecentEventIds.Reset();
    LastDecisionEventIds.Reset();
    ActionEvidenceIds.Reset();
    ActionTargetId.Invalidate();
    ActionConversationDepth = 0;
    ++ContextRevision;
    PublicState = {};
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()))
        Npc->ResetDecisionScheduling();
    PublishState();
}

// 服务器每 0.1 秒：清理孤立回复，处理倾听超时，安排初遇回应和交战中的下一次决策
void UDeliveryNpcAgentComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                               FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!IsAuthority())
        return;
    const double Now = GetWorld()->GetTimeSeconds();
    auto* Resident = Cast<ADeliveryNpcCharacter>(GetOwner());
    TOptional<bool> JoinedHearing;
    TArray<FGuid> OrphanedReplies;
    TArray<FGuid> UnheardJoinedReplies;

    // 清理已经没有动作的回复
    for (auto& Pair : Replies)
    {
        auto& Reply = Pair.Value;
        const bool bJoined = !Reply.bPlayerReply && Reply.JoinedEventId.IsValid();
        if ((!Reply.RequesterPC.IsExplicitlyNull() && !Reply.RequesterPC.IsValid()) ||
            (bJoined && (!IsJoinedResponseOwnerCurrent() || Reply.JoinedEventId != PendingJoinedEvent ||
                         Now >= JoinedResponseExpiresAt || !Reply.JoinedOwner.HasSameIndexAndSerialNumber(PendingJoinedOwner))))
        {
            OrphanedReplies.Add(Reply.ReplyId);
            continue;
        }
        if (!bJoined)
            continue;
        if (!JoinedHearing.IsSet())
            JoinedHearing = CanHearJoinedResponse(GetJoinedResponseListener());
        if (*JoinedHearing)
        {
            Reply.JoinedOutOfReachSince = -1;
            continue;
        }
        // 分句之间暂时走远可等一会儿，正在播放时走远则这整轮不能结清
        if (Resident && Resident->Presentation->IsReplyPlaying(Reply.ReplyId))
            Reply.bJoinedPlaybackMissed = true;
        if (Reply.JoinedOutOfReachSince < 0)
            Reply.JoinedOutOfReachSince = Now;
        if (Now - Reply.JoinedOutOfReachSince >= 6.)
            UnheardJoinedReplies.Add(Reply.ReplyId);
    }
    if (Resident && !OrphanedReplies.IsEmpty())
        Resident->Presentation->CancelReplies(OrphanedReplies);
    for (const FGuid& ReplyId : OrphanedReplies)
        RemoveReply(ReplyId, TEXT("RequesterLost"));
    if (!UnheardJoinedReplies.IsEmpty())
    {
        NextJoinedResponseRetryAt = FMath::Max(NextJoinedResponseRetryAt, Now + 4.);
        if (IsValid(Resident))
            Resident->Presentation->CancelReplies(UnheardJoinedReplies);
        for (const FGuid& ReplyId : UnheardJoinedReplies)
            RemoveReply(ReplyId, TEXT("JoinedListenerOutOfReach"));
    }

    // 发问的玩家离开后取消请求
    if (ActiveRequest.bPlayerRequest && !ActiveRequest.RequesterPC.IsValid())
    {
        const FGuid RequestId = ActiveRequest.RequestId;
        ActiveRequest = {};
        bDecisionPending = false;
        bDecisionQueued = false;
        if (auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>())
            Gateway->CancelRequest(this, RequestId);
        PlayerReplyMessage.Invalidate();
        PlayerReplyController.Reset();
        bPlayerRequest = false;
        EndListening();
    }
    const auto* VoiceConversation =
        PlayerReplyController.IsValid() ? PlayerReplyController->FindComponentByClass<UDeliveryConversationComponent>() : nullptr;
    const bool bLinkedVoice = ListeningMessage.IsValid() && VoiceConversation &&
                              VoiceConversation->IsVoiceReplyMessage(ListeningMessage, Cast<ADeliveryNpcCharacter>(GetOwner()));
    // 同一玩家上下车时倾听跟随当前控制对象，连麦使用服务器原轮次而非现场距离
    if (bLinkedVoice && PlayerReplyController.IsValid() && ListeningSpeaker != PlayerReplyController->GetPawn())
    {
        ListeningSpeaker = PlayerReplyController->GetPawn();
        GetOwner()->ForceNetUpdate();
    }

    // 倾听超时后结束
    if (ListeningMessage.IsValid() &&
        (!IsValid(ListeningSpeaker) || FPlatformTime::Seconds() > ListeningUntil || ListeningSpeaker->GetWorld() != GetWorld() ||
         (!bLinkedVoice &&
          ((PlayerReplyController.IsValid() && PlayerReplyController->GetPawn() != ListeningSpeaker) ||
           FVector::DistSquared(GetOwner()->GetActorLocation(), ListeningSpeaker->GetActorLocation()) > FMath::Square(900.f)))))
    {
        if (auto* Requester = PlayerReplyController.Get())
            InterruptPlayerReply(Requester);
        else
            EndListening();
    }
    if (HasActiveAction() && PublicState.Origin == EDeliveryNpcActionOrigin::Model &&
        PublicState.ActionId == NpcMessageActionId && PublicState.PlanRevision == NpcMessageActionPlan &&
        !IsNpcMessageParentCurrent(NpcMessageActionParent))
        TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Cancelled, TEXT("NpcMessageParentChanged"));
    else if (HasActiveAction() && PublicState.Origin == EDeliveryNpcActionOrigin::GameplayRule && !IsGameplayRuleActionValid())
        TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Cancelled, TEXT("GameplayRuleExpired"));
    else if (HasActiveAction() && GetWorld()->GetTimeSeconds() >= ActionDeadline)
        TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Failed, TEXT("ExecutorTimeout"));
    else if (PublicState.Status == EDeliveryNpcActionStatus::Running && PublicState.Action == EDeliveryNpcAction::LookAt &&
             !CanLookAt(PublicState.Target))
        TryFinishAction(PublicState.ActionId, EDeliveryNpcActionStatus::Cancelled, TEXT("TargetUnavailable"));
    // 初遇回应最多主动补两次，玩家录音、发言和正在播放的回复始终优先
    if (IsJoinedResponseOwnerCurrent() && JoinedResponseAttempts < 3 && Now < JoinedResponseExpiresAt &&
        Now >= NextJoinedResponseRetryAt && !bChangingPlan && !bPlayerRequest && !IsListening() && !IsDecisionPending() &&
        !HasActiveAction() && Replies.IsEmpty() && !IsPlayerInputBeingRecorded() &&
        CanHearJoinedResponse(GetJoinedResponseListener()))
    {
        auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
        if (Npc && !Npc->IsKnockedDown() && !Npc->Combat->IsReactingToHit() && !Npc->Presentation->HasPendingSpeech())
        {
            NextJoinedResponseRetryAt = GetWorld()->GetTimeSeconds() + 4;
            Npc->WakeDecision();
        }
    }
    // 模型完成上一轮选择后开始考虑下一步，当前动作和对白继续进行
    const bool bCombatEventDue = CombatEventDecisionAt >= 0 && Now >= CombatEventDecisionAt;
    if (CanUpdateRunningAction() && !IsDecisionPending() && (bCombatEventDue || Now >= NextActionDecisionAt))
    {
        NextActionDecisionAt = Now + 1;
        if (!RequestDecision() && bCombatEventDue)
            CombatEventDecisionAt = NextActionDecisionAt;
    }
}

// 状态变化后强制复制，并在服务器本机触发通知
void UDeliveryNpcAgentComponent::PublishState()
{
    GetOwner()->ForceNetUpdate();
    OnRep_PublicState();
}

// 客户端收到新状态，通知界面和表现组件
void UDeliveryNpcAgentComponent::OnRep_PublicState()
{
    OnPublicStateChanged.Broadcast(PublicState);
}

// 复制公开状态、等待标记和倾听对象
void UDeliveryNpcAgentComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UDeliveryNpcAgentComponent, PublicState);
    DOREPLIFETIME(UDeliveryNpcAgentComponent, bDecisionPending);
    DOREPLIFETIME(UDeliveryNpcAgentComponent, ListeningSpeaker);
    DOREPLIFETIME(UDeliveryNpcAgentComponent, ListeningStartedAt);
}

// 退出时禁止新请求，取消请求、回复和动作
void UDeliveryNpcAgentComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 退出时先禁止新请求，再取消剩余操作，不再通知即将销毁的表现组件
    bEndingPlay = true;
    PendingJoinedEvent.Invalidate();
    PendingJoinedOwner.Reset();
    PendingJoinedSubject.Reset();
    InvalidateNpcMessageTurns(TEXT("EndPlay"), true);
    if (auto* Turns = BoundNpcTurns.Get())
        Turns->OnNpcMessagesInvalidated.RemoveAll(this);
    BoundNpcTurns.Reset();
    PendingNpcMessageCancellations.Reset();
    TSet<FGuid> EndedMessages;
    TArray<TPair<TWeakObjectPtr<APlayerController>, FGuid>> EndedReplies;
    if (PlayerReplyController.IsValid() && PlayerReplyMessage.IsValid())
    {
        EndedMessages.Add(PlayerReplyMessage);
        EndedReplies.Emplace(PlayerReplyController, PlayerReplyMessage);
    }
    for (const auto& Pair : Replies)
        if (Pair.Value.RequesterPC.IsValid() && Pair.Value.MessageId.IsValid() && !EndedMessages.Contains(Pair.Value.MessageId))
        {
            EndedMessages.Add(Pair.Value.MessageId);
            EndedReplies.Emplace(Pair.Value.RequesterPC, Pair.Value.MessageId);
        }
    Replies.Reset();
    ListeningSpeaker = nullptr;
    ListeningMessage.Invalidate();
    PlayerReplyMessage.Invalidate();
    PlayerReplyController.Reset();
    bPlayerRequest = false;
    // 先清空自身状态再通知，外部结束回调不能修改正在遍历的回复表
    for (const auto& Ended : EndedReplies)
        if (Ended.Key.IsValid())
            OnPlayerReplyEnded.Broadcast(Ended.Key.Get(), Ended.Value, TEXT("EndPlay"));
    if (GetWorld())
        if (auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>())
            Gateway->Cancel(this);
    ActiveRequest = {};
    bDecisionPending = false;
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
    DecisionTrace = {};
    TraceQueuedAt = -1;
    TraceDispatchedAt = -1;
    OnDecisionTraceChanged.Clear();
    Super::EndPlay(EndPlayReason);
}

// 按对象查编号
FGuid UDeliveryNpcAgentComponent::FindKnownSubjectId(const AActor* Actor) const
{
    if (!IsValid(Actor) || Actor->GetWorld() != GetWorld())
        return FGuid();
    for (const auto& Pair : KnownSubjects)
        if (Pair.Value.Get() == Actor)
            return Pair.Key;
    return FGuid();
}

// 这个事件是否仍然记得
bool UDeliveryNpcAgentComponent::HasObservationEvent(FGuid EventId) const
{
    const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0;
    return EventId.IsValid() && Evidence.ContainsByPredicate(
                                    [&](const auto& Item)
                                    {
                                        return Item.EventId == EventId && Item.ExpiresAt > Now;
                                    });
}
