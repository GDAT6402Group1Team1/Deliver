#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Conversation/DeliveryConversationTurnSubsystem.h"
#include "AI/Conversation/DeliveryNpcConversationTrace.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

// 服务器：订阅轮次子系统的接话失效通知
void UDeliveryNpcAgentComponent::BindNpcMessageTurns()
{
    if (!IsAuthority())
        return;
    if (auto* Turns = GetWorld()->GetSubsystem<UDeliveryConversationTurnSubsystem>())
    {
        BoundNpcTurns = Turns;
        Turns->OnNpcMessagesInvalidated.AddUObject(this, &UDeliveryNpcAgentComponent::HandleNpcMessageCancellations);
    }
}

// 这次回复所接的上一句话是否仍然有效
bool UDeliveryNpcAgentComponent::IsNpcMessageParentCurrent(const FDeliveryNpcMessageHandle& Parent) const
{
    if (!Parent.IsValid())
        return true;
    const auto* Turns = BoundNpcTurns.Get();
    return IsAuthority() && Turns && Turns->IsNpcMessageCurrent(Parent, Cast<ADeliveryNpcCharacter>(GetOwner()));
}

// 玩家是否正在对这位 NPC 录音或等待转写
bool UDeliveryNpcAgentComponent::IsPlayerInputBeingRecorded() const
{
    auto* Turns = BoundNpcTurns.Get();
    FDeliveryConversationTurnLease Lease;
    return Turns && Turns->GetLease(Cast<ADeliveryNpcCharacter>(GetOwner()), Lease) &&
           (Lease.Phase == EDeliveryConversationTurnPhase::Recording ||
            Lease.Phase == EDeliveryConversationTurnPhase::FinalPending);
}

// 请求附上“正在回应哪位 NPC 的哪句话”，玩家请求不附加
void UDeliveryNpcAgentComponent::AttachNpcMessageContext(FDeliveryNpcDecisionRequest& Request) const
{
    Request.ParentNpcMessage = {};
    if (Request.bPlayerRequest)
        return;
    const auto* Turns = BoundNpcTurns.Get();
    if (!Turns)
        return;
    for (int32 Index = Request.Evidence.Num() - 1; Index >= 0; --Index)
    {
        const auto& Message = Request.Evidence[Index];
        if (!Message.bNeedsReply || (Message.Kind != TEXT("Speech") && Message.Kind != TEXT("NpcMessage")))
            continue;
        const bool bAnswered = Request.Evidence.ContainsByPredicate(
            [&](const auto& Reply)
            {
                return Reply.ReplyToEventId == Message.EventId &&
                       (Reply.Kind == TEXT("SelfSpeech") || Reply.Kind == TEXT("ActionReply") ||
                        (Message.Kind == TEXT("NpcMessage") && Reply.Kind == TEXT("NpcMessageSent")));
            });
        if (bAnswered)
            continue;
        if (Message.Kind == TEXT("NpcMessage"))
        {
            const auto Parent = Turns->GetNpcMessageHandle(Message.EventId, Cast<ADeliveryNpcCharacter>(GetOwner()));
            FDeliveryNpcMessage Delivered;
            if (!Parent.IsValid() || !Turns->GetNpcMessage(Parent, Cast<ADeliveryNpcCharacter>(GetOwner()), Delivered))
                continue;
            Request.ParentNpcMessage = Parent;
            Request.ConversationDepth = Delivered.Depth;
        }
        break;
    }
}

// 采纳后向轮次子系统登记这一轮回复，之后可以把台词定向发给另一位 NPC
bool UDeliveryNpcAgentComponent::RegisterReplyMessageContext(FReply& Reply, const FDeliveryNpcDecisionRequest& Request,
                                                             bool bPlainText)
{
    Reply.Listener = PublicState.SpeechTarget;
    Reply.Target = PublicState.Target;
    Reply.bHadTarget = IsValid(PublicState.Target);
    Reply.Depth = Request.ConversationDepth;
    Reply.ParentNpcMessage = Request.ParentNpcMessage;
    Reply.Emotion = PublicState.Emotion;
    Reply.bPlainText = bPlainText;
    Reply.bPlayerReply = Request.bPlayerRequest;
    Reply.bOwnsAction = !Reply.bContinueCurrent && Reply.ActionId != Request.RunningActionId;
    Reply.bActionStarted =
        PublicState.Status == EDeliveryNpcActionStatus::Succeeded ||
        (!Reply.RequiresCompanionCommandReceipt() && (PublicState.Status == EDeliveryNpcActionStatus::Running ||
                                                      (Reply.bContinueCurrent && !Request.RunningActionId.IsValid())));
    if (Reply.bOwnsAction)
    {
        NpcMessageActionParent = Reply.ParentNpcMessage;
        NpcMessageActionId = Reply.ActionId;
        NpcMessageActionPlan = Reply.PlanRevision;
    }
    auto* Turns = BoundNpcTurns.Get();
    // 面向玩家或没有定向 NPC 的根回复保留原来的生命周期，不占用快文本链表
    if (Turns && (Reply.ParentNpcMessage.IsValid() || Cast<ADeliveryNpcCharacter>(Reply.Listener.Get())))
    {
        FDeliveryNpcReplyRegistration Entry;
        Entry.Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
        Entry.RequestId = Request.RequestId;
        Entry.ReplyId = Reply.ReplyId;
        Entry.ParentMessage = Reply.ParentNpcMessage;
        Entry.ActionId = Reply.ActionId;
        Entry.PlanRevision = Reply.PlanRevision;
        Entry.Depth = Reply.Depth;
        Entry.bOwnsAction = Reply.bOwnsAction;
        Entry.bPlayerReply = Reply.bPlayerReply;
        Reply.bNpcMessageTracked = Turns->RegisterNpcReply(Entry);
    }
    // 普通根回复仍可沿旧听闻；子回复失去父关系时不能继续执行旧建议
    return Reply.bNpcMessageTracked || !Reply.ParentNpcMessage.IsValid();
}

// 定向回复的声音能否开始播放
bool UDeliveryNpcAgentComponent::CanStartReplyMessageAudio(FGuid ReplyId) const
{
    for (const auto& Pair : Replies)
        if (Pair.Value.ReplyId == ReplyId && Pair.Value.bNpcMessageTracked)
        {
            const auto* Turns = BoundNpcTurns.Get();
            return Turns && Turns->CanStartNpcReplyPresentation(ReplyId);
        }
    const auto* Turns = BoundNpcTurns.Get();
    return !Turns || !Turns->HasNpcReplyRecord(ReplyId) || Turns->CanStartNpcReplyPresentation(ReplyId);
}

// 接话链失效后，已经在播的这一轮能否继续说完
bool UDeliveryNpcAgentComponent::CanRetainReplySpeech(FGuid ReplyId) const
{
    const auto* Turns = BoundNpcTurns.Get();
    if (!Turns || !Turns->HasNpcReplyRecord(ReplyId))
        return true;
    if (!Turns->IsNpcReplyValid(ReplyId))
        return false;
    for (const auto& Pair : Replies)
        if (Pair.Value.ReplyId == ReplyId)
            return true;
    return false;
}

// 是否有别的 NPC 对我说了话、还没回应
bool UDeliveryNpcAgentComponent::HasPendingNpcMessage() const
{
    if (!IsAuthority() || bPlayerRequest)
        return false;
    const auto* Turns = BoundNpcTurns.Get();
    if (!Turns)
        return false;
    return Evidence.ContainsByPredicate(
        [this, Turns](const auto& Message)
        {
            return Message.Kind == TEXT("NpcMessage") && Message.bNeedsReply &&
                   Turns->GetNpcMessageHandle(Message.EventId, Cast<ADeliveryNpcCharacter>(GetOwner())).IsValid() &&
                   !Evidence.ContainsByPredicate(
                       [&Message](const auto& Reply)
                       {
                           return Reply.ReplyToEventId == Message.EventId &&
                                  (Reply.Kind == TEXT("NpcMessageSent") || Reply.Kind == TEXT("SelfSpeech") ||
                                   Reply.Kind == TEXT("ActionReply"));
                       });
        });
}

// 读取整轮回复的完整文字，供纯文字回复使用
bool UDeliveryNpcAgentComponent::GetCompleteReply(FGuid ReplyId, FDeliveryNpcCompleteReply& OutReply) const
{
    OutReply = {};
    for (const auto& Pair : Replies)
    {
        const auto& Reply = Pair.Value;
        if (Reply.ReplyId != ReplyId || !Reply.bPlainText || !Reply.bHasApprovedText)
            continue;
        OutReply.ReplyId = Reply.ReplyId;
        OutReply.MessageId = Reply.MessageId;
        OutReply.Text = Reply.ApprovedText;
        OutReply.Emotion = Reply.Emotion;
        OutReply.Depth = Reply.Depth;
        OutReply.Listener = Reply.Listener;
        return true;
    }
    return false;
}

// 能否接收另一位 NPC 发来的定向消息
bool UDeliveryNpcAgentComponent::CanReceiveNpcMessage(const FDeliveryNpcMessage& Message) const
{
    const auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    const auto* Sender = Message.Sender.Get();
    if (!IsAuthority() || bChangingPlan || !IsValid(Npc) || !IsValid(Sender) || Sender == Npc || Message.Receiver.Get() != Npc ||
        Sender->GetWorld() != GetWorld() || Sender->IsActorBeingDestroyed() || Npc->IsKnockedDown() || Sender->IsKnockedDown() ||
        !DeliveryNpcMessages::IsValidReplyText(Message.Text, false) ||
        FVector::DistSquared(Sender->GetActorLocation(), Npc->GetActorLocation()) >= FMath::Square(800.f) ||
        !DeliveryNpcConversationTrace::HasClearPath(Npc, Sender))
        return false;
    auto* Turns = BoundNpcTurns.Get();
    FDeliveryConversationTurnLease Lease;
    if (!Turns || Turns->GetLease(const_cast<ADeliveryNpcCharacter*>(Npc), Lease))
        return false;
    return IsAuthority() && !bChangingPlan && IsValid(Sender) && !Sender->IsActorBeingDestroyed() && IsValid(Npc) &&
           !Npc->IsActorBeingDestroyed() && !Npc->IsKnockedDown() && !Sender->IsKnockedDown() &&
           Sender->GetWorld() == GetWorld() &&
           FVector::DistSquared(Sender->GetActorLocation(), Npc->GetActorLocation()) < FMath::Square(800.f) &&
           DeliveryNpcConversationTrace::HasClearPath(Npc, Sender) && !bPlayerRequest && !IsListening() &&
           !PlayerReplyMessage.IsValid();
}

// 接收另一位 NPC 的定向消息：写成 NpcMessage 事件并请求下一次决策
bool UDeliveryNpcAgentComponent::ReceiveNpcMessage(const FDeliveryNpcMessageHandle& Handle)
{
    auto* Turns = BoundNpcTurns.Get();
    auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner());
    FDeliveryNpcMessage Message;
    if (!Turns || !Turns->GetNpcMessage(Handle, Npc, Message, true) || !CanReceiveNpcMessage(Message))
        return false;
    // 距离查询可能触发旧占用清理，写观察前再核对原消息代次
    if (!Turns->GetNpcMessage(Handle, Npc, Message, true))
        return false;
    FDeliveryNpcObservation Event;
    Event.EventId = Handle.MessageId;
    Event.Kind = TEXT("NpcMessage");
    Event.Source = EDeliveryNpcEvidenceSource::ToldBy;
    Event.Subject = Message.Sender.Get();
    Event.Addressee = Npc;
    Event.Description = Message.Text;
    Event.ReplyToEventId = Message.ReplyToEventId;
    Event.bNeedsReply = Message.bNeedsReply;
    Event.bHasObservedLocation = false;
    Event.Lifetime = 300;
    if (!ObserveEvent(Event))
        return false;
    if (!Turns->ConfirmNpcMessageDelivery(Handle, Npc, true))
        return false;
    if (auto* Companion = Cast<ADeliveryCompanionCharacter>(Npc); IsValid(Companion))
        Companion->Companion->RecordDeliveredNeighborLine(Handle);
    return true;
}

// 把这一轮回复的完整台词定向发给说话对象，对方据此接话
void UDeliveryNpcAgentComponent::TryDeliverReplyMessage(FGuid RequestId)
{
    if (!IsAuthority() || bChangingPlan)
        return;
    auto* Found = Replies.Find(RequestId);
    if (!Found || !Found->bHasApprovedText || Found->bNpcMessageAttempted)
        return;
    // 委托确认后仍可发送这份已批准的文字，当前不占用实际发送机会
    if (Found->RequiresCompanionCommandReceipt() && !Found->bActionStarted)
        return;
    Found->bNpcMessageAttempted = true;
    const FReply Reply = *Found;
    auto* Turns = BoundNpcTurns.Get();
    auto* Sender = Cast<ADeliveryNpcCharacter>(GetOwner());
    auto* Receiver = Cast<ADeliveryNpcCharacter>(Reply.Listener.Get());
    if (!Reply.bNpcMessageTracked || !Turns || !IsValid(Sender) || !IsValid(Receiver) ||
        !IsNpcMessageParentCurrent(Reply.ParentNpcMessage) || Reply.ApprovedText.IsEmpty())
        return;
    if (Reply.bPlayerReply)
    {
        FDeliveryConversationTurnLease Lease;
        if (!Reply.RequesterPC.IsValid() || !Reply.RequesterPlayerState.IsValid() ||
            Reply.RequesterPC->PlayerState != Reply.RequesterPlayerState.Get() || !Turns->GetLease(Sender, Lease) ||
            Lease.Owner != Reply.RequesterPC || Lease.MessageId != Reply.MessageId ||
            Lease.Phase != EDeliveryConversationTurnPhase::AwaitingReply)
            return;
        if (!IsAuthority() || !IsValid(Receiver))
            return;
    }
    FDeliveryNpcMessage Preview;
    Preview.Sender = Sender;
    Preview.Receiver = Receiver;
    Preview.Text = Reply.ApprovedText;
    if (!Receiver->Agent || !Receiver->Agent->CanReceiveNpcMessage(Preview))
        return;
    if (!IsAuthority() || !IsValid(Receiver) || Receiver->IsActorBeingDestroyed())
        return;
    Found = Replies.Find(RequestId);
    if (!Found || Found->ReplyId != Reply.ReplyId || Found->Listener != Reply.Listener)
        return;
    FDeliveryNpcMessage Message;
    if (!Turns->TryPrepareNpcMessage(Sender, RequestId, Reply.ReplyId, Receiver, Reply.ApprovedText, Reply.MessageId, Message))
        return;
    const TWeakObjectPtr<UDeliveryNpcAgentComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryNpcCharacter> WeakReceiver(Receiver);
    if (!Receiver->Agent->ReceiveNpcMessage(Message.Handle))
    {
        Turns->ConfirmNpcMessageDelivery(Message.Handle, Receiver, false);
        return;
    }
    if (!WeakThis.IsValid() || !IsAuthority() || !WeakReceiver.IsValid())
        return;
    FDeliveryNpcObservation Sent;
    Sent.Kind = TEXT("NpcMessageSent");
    Sent.Source = EDeliveryNpcEvidenceSource::ActionResult;
    Sent.Subject = Sender;
    Sent.Addressee = Receiver;
    Sent.Description = Reply.ApprovedText;
    Sent.bNeedsReply = false;
    Sent.Lifetime = 300;
    FDeliveryNpcMessage Parent;
    if (Reply.ParentNpcMessage.IsValid() && Turns->GetNpcMessage(Reply.ParentNpcMessage, Sender, Parent) &&
        Parent.Sender.Get() == Receiver)
        Sent.ReplyToEventId = Reply.ParentNpcMessage.MessageId;
    ObserveEvent(Sent);
    // 明确送达以后才唤醒，接收者从未取得发话玩家的指挥权限
    if (Message.bNeedsReply && Turns->IsNpcMessageCurrent(Message.Handle, Receiver))
    {
        Receiver->WakeDecision();
        if (Receiver->Agent->IsDecisionPending())
            Receiver->Agent->RequestDecision();
    }
}

// 这一轮回复的文字是否已送到某位 NPC
bool UDeliveryNpcAgentComponent::WasReplyTextDelivered(FGuid ReplyId, const ADeliveryNpcCharacter* Receiver) const
{
    const auto* Turns = BoundNpcTurns.Get();
    return Turns && Turns->WasNpcMessageDelivered(ReplyId, Receiver);
}

// 把回复的播放结果报告给轮次子系统，处理随之失效的下游
void UDeliveryNpcAgentComponent::ReportNpcReplyPresentation(FGuid ReplyId, EDeliveryNpcReplyPresentation State)
{
    if (auto* Turns = BoundNpcTurns.Get())
    {
        TArray<FDeliveryNpcMessageCancellation> Cancelled;
        Turns->ReportNpcReplyPresentation(ReplyId, State, Cancelled);
        if (!Cancelled.IsEmpty())
            Turns->OnNpcMessagesInvalidated.Broadcast(Cancelled);
    }
}

// 让这位 NPC 参与的接话分支失效
void UDeliveryNpcAgentComponent::InvalidateNpcMessageTurns(FName Reason, bool bLeavingWorld)
{
    if (auto* Turns = BoundNpcTurns.Get())
    {
        TArray<FDeliveryNpcMessageCancellation> Cancelled;
        Turns->InvalidateNpcBranchesFor(Cast<ADeliveryNpcCharacter>(GetOwner()), Reason, Cancelled, bLeavingWorld);
        if (!Cancelled.IsEmpty())
            Turns->OnNpcMessagesInvalidated.Broadcast(Cancelled);
    }
}

// 接话失效通知：取消对应的回复和由它引出的动作
void UDeliveryNpcAgentComponent::HandleNpcMessageCancellations(const TArray<FDeliveryNpcMessageCancellation>& Cancelled)
{
    if (!IsAuthority())
        return;
    for (const auto& Cancel : Cancelled)
        if (Cancel.Npc.Get() == GetOwner())
            PendingNpcMessageCancellations.Add(Cancel);
    if (bHandlingNpcMessageCancellation)
        return;
    TGuardValue<bool> Handling(bHandlingNpcMessageCancellation, true);
    while (IsAuthority() && !PendingNpcMessageCancellations.IsEmpty())
    {
        const auto Cancel = PendingNpcMessageCancellations[0];
        PendingNpcMessageCancellations.RemoveAt(0);
        if (Cancel.IncomingMessage.IsValid())
        {
            for (auto& Item : Evidence)
                if (Item.EventId == Cancel.IncomingMessage.MessageId && Item.Kind == TEXT("NpcMessage") && Item.bNeedsReply)
                {
                    Item.bNeedsReply = false;
                    ++ContextRevision;
                }
        }
        const bool bCancelRequest =
            ActiveRequest.RequestId.IsValid() &&
            (ActiveRequest.RequestId == Cancel.RequestId ||
             (Cancel.IncomingMessage.IsValid() && ActiveRequest.ParentNpcMessage == Cancel.IncomingMessage));
        if (bCancelRequest)
        {
            const FGuid OldRequest = ActiveRequest.RequestId;
            ActiveRequest = {};
            bDecisionPending = false;
            bDecisionQueued = false;
            RequestPlayerState.Reset();
            if (auto* Gateway = GetWorld()->GetSubsystem<UDeliveryAiGatewaySubsystem>())
                Gateway->CancelRequest(this, OldRequest);
        }
        if (!IsAuthority())
            return;
        if (Cancel.ReplyId.IsValid())
        {
            if (PublicState.GetReplyId() == Cancel.ReplyId)
            {
                PublicState.Speech.Reset();
                PublicState.ReplyToEventId.Invalidate();
                PublicState.bStreamSpeech = false;
            }
            if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetOwner()); Npc && Npc->Presentation)
                Npc->Presentation->CancelReply(Cancel.ReplyId);
            if (!IsAuthority())
                return;
            RemoveReply(Cancel.ReplyId, Cancel.Reason);
        }
        if (!IsAuthority())
            return;
        if (Cancel.bCancelAction && PublicState.Origin == EDeliveryNpcActionOrigin::Model &&
            PublicState.ActionId == Cancel.ActionId && PublicState.PlanRevision == Cancel.PlanRevision)
        {
            const bool bInterruptedDuringCombat =
                Cancel.Reason == TEXT("NpcReplyInterrupted") && PublicState.Status == EDeliveryNpcActionStatus::Running &&
                (PublicState.Action == EDeliveryNpcAction::Fight || PublicState.Action == EDeliveryNpcAction::Flee);
            if (bInterruptedDuringCombat)
            {
                // 轻拳能打断对话，已经开始的追打或逃跑仍按本次模型决定执行
                if (NpcMessageActionId == Cancel.ActionId && NpcMessageActionPlan == Cancel.PlanRevision)
                {
                    NpcMessageActionParent = {};
                    NpcMessageActionId.Invalidate();
                    NpcMessageActionPlan = 0;
                }
                if (PublicState.GetReplyId() == Cancel.ReplyId)
                    PublishState();
            }
            else
                TryFinishAction(Cancel.ActionId, EDeliveryNpcActionStatus::Cancelled, Cancel.Reason);
        }
    }
}
