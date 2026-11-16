#include "AI/Conversation/DeliveryConversationTurnSubsystem.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "HAL/PlatformTime.h"

// ── NPC 之间接话 ──
// 一位 NPC 说完话，可以点名让另一位 NPC 接着回答，最多接三层，整条链超过时限就作废
// NPC 的回复被采纳时先调用 RegisterNpcReply 登记；要让另一位 NPC 接话时调用 TryPrepareNpcMessage，生成发给对方的 FDeliveryNpcMessage
// 对方调用 ConfirmNpcMessageDelivery 收下后，才去问模型怎么回；回复开始播、播完或被打断时调用 ReportNpcReplyPresentation
// 链上某位 NPC 被打断或离开时调用 InvalidateNpcBranchesFor，作废它之后的接话；PruneNpcMessages 定期作废超时的记录
// 两处作废的结果都通过 OnNpcMessagesInvalidated 通知，收到通知的代码停掉对应的声音和动作

/// @brief 检查 NPC 之间传的台词能不能用：最多 120 个字，而且不含损坏的汉字编码
/// UTF-16 里一些罕见字和表情要用两个编码单位（高位 + 低位）表示，只剩一半的就是损坏的
bool DeliveryNpcMessages::IsValidReplyText(const FString& Text, bool bAllowEmpty)
{
    if (Text.IsEmpty())
        return bAllowEmpty;

    // 120 个字最多占 240 个编码单位，超过的不用再逐个数
    if (Text.Len() > 240 || Text.TrimStartAndEnd().IsEmpty())
        return false;
    int32 Characters = 0;
    for (int32 Index = 0; Index < Text.Len(); ++Index)
    {
        const uint32 Character = static_cast<uint32>(Text[Index]);

        // 高位编码单位，后面必须紧跟一个低位
        if (Character >= 0xD800 && Character <= 0xDBFF)
        {
            if (++Index >= Text.Len())
                return false;
            const uint32 Low = static_cast<uint32>(Text[Index]);
            if (Low < 0xDC00 || Low > 0xDFFF)
                return false;
        }

        // 前面没有高位却出现了低位，也是损坏的
        else if (Character >= 0xDC00 && Character <= 0xDFFF)
            return false;
        if (++Characters > 120)
            return false;
    }
    return true;
}

/// @brief 判断这位 NPC 现在能参与接话：在服务器上、没有被销毁、在这个世界里
bool UDeliveryConversationTurnSubsystem::IsNpcHere(const ADeliveryNpcCharacter* Npc) const
{
    return IsServerWorld() && IsValid(Npc) && !Npc->IsActorBeingDestroyed() && Npc->HasAuthority() &&
           Npc->GetWorld() == GetWorld();
}

/// @brief 登记一轮已采纳的 NPC 回复，之后才能把它的台词发给别的 NPC
/// 回答玩家或自己先开口的回复是一条新链的开头；回答另一位 NPC 的回复沿用对方那条链的编号和开始时间
/// @return 登记成功返回 true；NPC 无效、层数超过 3、重复登记或记录满了返回 false
bool UDeliveryConversationTurnSubsystem::RegisterNpcReply(const FDeliveryNpcReplyRegistration& Reply)
{
    check(IsInGameThread());
    if (!IsNpcHere(Reply.Npc.Get()) || !Reply.RequestId.IsValid() || !Reply.ReplyId.IsValid() || Reply.Depth < 0 ||
        Reply.Depth > 3 || NpcReplies.Contains(Reply.ReplyId) || NpcReplies.Num() >= MaxNpcReplyRecords)
        return false;
    const double Now = FPlatformTime::Seconds();
    FGuid ChainId = Reply.ReplyId;
    double ChainStartedAt = Now;

    // ParentMessage 有效说明这轮在回答另一位 NPC：那条消息要能被 GetNpcMessage 取到、bNeedsReply 为 true、Depth 相同，才沿用它的 ChainId 和 ChainStartedAt
    if (Reply.ParentMessage.IsValid())
    {
        FDeliveryNpcMessage Parent;
        if (!GetNpcMessage(Reply.ParentMessage, Reply.Npc.Get(), Parent) || !Parent.bNeedsReply || Parent.Depth != Reply.Depth)
            return false;
        const auto* Ancestor = NpcReplies.Find(Reply.ParentMessage.MessageId);
        if (!Ancestor || Ancestor->bInvalid)
            return false;
        ChainId = Ancestor->ChainId;
        ChainStartedAt = Ancestor->ChainStartedAt;
    }
    FNpcReplyRecord Record;
    Record.Reply = Reply;
    Record.ChainId = ChainId;
    Record.CreatedAt = Now;
    Record.ChainStartedAt = ChainStartedAt;
    NpcReplies.Add(Reply.ReplyId, MoveTemp(Record));
    return true;
}

/// @brief 把一轮已登记的回复包成发给另一位 NPC 的消息
/// 每轮回复只能发一次；已经是第三层、链已超时、或者对方正在和玩家说话时不发
/// @param OutMessage 成功时写入要交给对方的消息，层数比这轮多一
bool UDeliveryConversationTurnSubsystem::TryPrepareNpcMessage(ADeliveryNpcCharacter* Sender, FGuid RequestId, FGuid ReplyId,
                                                              ADeliveryNpcCharacter* Receiver, const FString& Text,
                                                              FGuid ReplyToEventId, FDeliveryNpcMessage& OutMessage)
{
    check(IsInGameThread());
    OutMessage = {};
    auto* Record = NpcReplies.Find(ReplyId);
    if (!IsNpcHere(Sender) || !IsNpcHere(Receiver) || Sender == Receiver || !Record || Record->Reply.Npc.Get() != Sender ||
        Record->Reply.RequestId != RequestId || Record->bInvalid || Record->bMessagePrepared || Record->Reply.Depth >= 3 ||
        !DeliveryNpcMessages::IsValidReplyText(Text, false) || NextNpcMessageGeneration == MAX_uint32 ||
        FPlatformTime::Seconds() - Record->ChainStartedAt >= MaxNpcChainSeconds)
        return false;
    if (Record->Reply.ParentMessage.IsValid() && !IsNpcMessageCurrent(Record->Reply.ParentMessage, Sender))
        return false;

    // GetLease 返回 true 说明对方正被某位玩家的轮次占着，不发消息打断他们
    FDeliveryConversationTurnLease Lease;
    if (GetLease(Receiver, Lease))
        return false;

    // GetLease 会调用 PruneInvalidLeases 并发出 OnLeaseExpired，收到通知的代码可能改动 NpcReplies，所以重新调用 NpcReplies.Find 取 Record 并再检查一遍
    Record = NpcReplies.Find(ReplyId);
    if (!IsNpcHere(Sender) || !IsNpcHere(Receiver) || !Record || Record->bInvalid || Record->bMessagePrepared ||
        Record->Reply.Npc.Get() != Sender || Record->Reply.RequestId != RequestId ||
        (Record->Reply.ParentMessage.IsValid() && !IsNpcMessageCurrent(Record->Reply.ParentMessage, Sender)))
        return false;

    // bMessagePrepared 设为 true，这轮回复不能再发第二次；对方调用 ConfirmNpcMessageDelivery 之前 bMessageDelivered 保持 false
    Record->bMessagePrepared = true;
    Record->bMessageActive = true;
    auto& Message = Record->Message;
    Message.Handle.WorldEpoch = WorldEpoch;
    Message.Handle.MessageId = ReplyId;
    Message.Handle.Generation = ++NextNpcMessageGeneration;
    Message.Sender = Sender;
    Message.Receiver = Receiver;
    Message.Text = Text;
    Message.ReplyToEventId = ReplyToEventId;
    Message.Depth = Record->Reply.Depth + 1;

    // Depth 到 3 时 bNeedsReply 为 false，对方只记住这句话，不再回话
    Message.bNeedsReply = Message.Depth < 3;
    OutMessage = Message;
    return true;
}

/// @brief 按编号取发给某位 NPC 的消息；消息作废、链超时、收发双方有一位不在了都返回 false
/// @param bAllowPrepared true 时连对方还没收下的消息也取出来
bool UDeliveryConversationTurnSubsystem::GetNpcMessage(const FDeliveryNpcMessageHandle& Handle,
                                                       const ADeliveryNpcCharacter* Receiver, FDeliveryNpcMessage& OutMessage,
                                                       bool bAllowPrepared) const
{
    OutMessage = {};
    const auto* Record = NpcReplies.Find(Handle.MessageId);
    if (!IsNpcHere(Receiver) || !Handle.IsValid() || Handle.WorldEpoch != WorldEpoch || !Record || Record->bInvalid ||
        !Record->bMessageActive || !Record->bMessagePrepared || (!bAllowPrepared && !Record->bMessageDelivered) ||
        !(Record->Message.Handle == Handle) || Record->Message.Receiver.Get() != Receiver ||
        !IsNpcHere(Record->Message.Sender.Get()) || FPlatformTime::Seconds() - Record->ChainStartedAt >= MaxNpcChainSeconds)
        return false;
    OutMessage = Record->Message;
    return true;
}

/// @brief 接收的 NPC 告诉这里收不收下这条消息；不收下时这条消息作废
/// @return 收下并记录成功时返回 true
bool UDeliveryConversationTurnSubsystem::ConfirmNpcMessageDelivery(const FDeliveryNpcMessageHandle& Handle,
                                                                   ADeliveryNpcCharacter* Receiver, bool bAccepted)
{
    check(IsInGameThread());
    FDeliveryNpcMessage Message;
    if (!GetNpcMessage(Handle, Receiver, Message, true))
        return false;
    auto* Record = NpcReplies.Find(Handle.MessageId);
    if (!Record || Record->bMessageDelivered)
        return false;
    Record->bMessageDelivered = bAccepted;
    Record->bMessageActive = bAccepted;
    return bAccepted;
}

/// @brief 判断这条消息仍然有效，而且还在等对方回话
bool UDeliveryConversationTurnSubsystem::IsNpcMessageCurrent(const FDeliveryNpcMessageHandle& Handle,
                                                             const ADeliveryNpcCharacter* Receiver) const
{
    FDeliveryNpcMessage Message;
    return GetNpcMessage(Handle, Receiver, Message) && Message.bNeedsReply;
}

/// @brief 判断这轮回复的台词是不是已经被这位 NPC 收下了
bool UDeliveryConversationTurnSubsystem::WasNpcMessageDelivered(FGuid ReplyId, const ADeliveryNpcCharacter* Receiver) const
{
    const auto* Record = NpcReplies.Find(ReplyId);
    return IsNpcHere(Receiver) && Record && Record->bMessageDelivered && Record->Message.Receiver.Get() == Receiver;
}

/// @brief 按消息编号取发给这位 NPC 的完整编号；消息已经无效时返回空编号
FDeliveryNpcMessageHandle UDeliveryConversationTurnSubsystem::GetNpcMessageHandle(FGuid MessageId,
                                                                                  const ADeliveryNpcCharacter* Receiver) const
{
    const auto* Record = NpcReplies.Find(MessageId);
    return Record && IsNpcMessageCurrent(Record->Message.Handle, Receiver) ? Record->Message.Handle : FDeliveryNpcMessageHandle();
}

/// @brief 判断这轮回复还有效：没有作废、说话的 NPC 还在、整条链没有超时
bool UDeliveryConversationTurnSubsystem::IsNpcReplyValid(FGuid ReplyId) const
{
    const auto* Record = NpcReplies.Find(ReplyId);
    return IsServerWorld() && Record && !Record->bInvalid && IsNpcHere(Record->Reply.Npc.Get()) &&
           FPlatformTime::Seconds() - Record->ChainStartedAt < MaxNpcChainSeconds;
}

/// @brief NPC 开口说这轮回复之前调用：沿着链往前查，前面任何一层被打断或还没开始播，就先不说
bool UDeliveryConversationTurnSubsystem::CanStartNpcReplyPresentation(FGuid ReplyId) const
{
    const auto* Record = NpcReplies.Find(ReplyId);
    if (!IsServerWorld() || !Record || Record->bInvalid || !IsNpcHere(Record->Reply.Npc.Get()) ||
        FPlatformTime::Seconds() - Record->ChainStartedAt >= MaxNpcChainSeconds)
        return false;
    const auto* Current = Record;

    // 沿 ParentMessage 一层层往前查，最多查三层；前一层的 Presentation 还是 Pending（没开始播）或 Interrupted（被打断）时返回 false
    for (int32 Depth = 0; Current->Reply.ParentMessage.IsValid(); ++Depth)
    {
        if (Depth >= 3)
            return false;
        const auto ParentHandle = Current->Reply.ParentMessage;
        if (!IsNpcMessageCurrent(ParentHandle, Current->Reply.Npc.Get()))
            return false;
        const auto* Parent = NpcReplies.Find(ParentHandle.MessageId);
        if (!Parent || Parent->bInvalid || Parent->Presentation == EDeliveryNpcReplyPresentation::Interrupted ||
            Parent->Presentation == EDeliveryNpcReplyPresentation::Pending)
            return false;
        Current = Parent;
    }
    return true;
}

/// @brief 把一轮回复作废，记进取消列表，并让它发出去的消息也作废
/// 这轮回复如果带着一个动作（不是回答玩家的），取消列表里会要求把动作也停掉
void UDeliveryConversationTurnSubsystem::AddNpcCancellation(FNpcReplyRecord& Record, FName Reason,
                                                            TArray<FDeliveryNpcMessageCancellation>& OutCancelled)
{
    if (Record.bInvalid)
        return;
    Record.bInvalid = true;
    Record.TerminalAt = FPlatformTime::Seconds();
    Record.Presentation = EDeliveryNpcReplyPresentation::Interrupted;
    FDeliveryNpcMessageCancellation Cancel;
    Cancel.Npc = Record.Reply.Npc;
    Cancel.RequestId = Record.Reply.RequestId;
    Cancel.ReplyId = Record.Reply.ReplyId;
    Cancel.ActionId = Record.Reply.ActionId;
    Cancel.PlanRevision = Record.Reply.PlanRevision;
    Cancel.bCancelAction = Record.Reply.bOwnsAction && !Record.Reply.bPlayerReply;
    Cancel.Reason = Reason;
    OutCancelled.Add(Cancel);
    InvalidateNpcMessage(Record, Reason, OutCancelled);
}

/// @brief 让这轮回复发出的消息作废；对方已经收下的，在取消列表里通知对方别再回了
void UDeliveryConversationTurnSubsystem::InvalidateNpcMessage(FNpcReplyRecord& Record, FName Reason,
                                                              TArray<FDeliveryNpcMessageCancellation>& OutCancelled)
{
    if (!Record.bMessageActive)
        return;
    Record.bMessageActive = false;
    Record.Message.bNeedsReply = false;
    if (Record.bMessageDelivered)
    {
        FDeliveryNpcMessageCancellation Cancel;
        Cancel.Npc = Record.Message.Receiver;
        Cancel.IncomingMessage = Record.Message.Handle;
        Cancel.Reason = Reason;
        OutCancelled.Add(Cancel);
    }
}

/// @brief 有回复作废后，把回答它的后续回复也全部作废
void UDeliveryConversationTurnSubsystem::InvalidateNpcDescendants(FName Reason,
                                                                  TArray<FDeliveryNpcMessageCancellation>& OutCancelled)
{
    // 每一遍至少往下作废一层，接话最多三层，所以四遍一定能作废完；bChanged 为 false 时提前结束
    for (int32 Pass = 0; Pass < 4; ++Pass)
    {
        bool bChanged = false;
        for (auto& Pair : NpcReplies)
        {
            auto& Record = Pair.Value;
            if (!Record.bInvalid && Record.Reply.ParentMessage.IsValid() &&
                !IsNpcMessageCurrent(Record.Reply.ParentMessage, Record.Reply.Npc.Get()))
            {
                AddNpcCancellation(Record, Reason, OutCancelled);
                bChanged = true;
            }
        }
        if (!bChanged)
            break;
    }
}

/// @brief 某位 NPC 被打断或离开世界时，作废它收到的消息和它说的回复，以及后面的接话
/// @param bLeavingWorld true 时连它回答玩家的回复也作废；false 时那些回复保留，只作废 NPC 之间的接话
/// @param OutCancelled 输出被作废的回复和消息，调用方据此停掉对应的声音和动作
void UDeliveryConversationTurnSubsystem::InvalidateNpcBranchesFor(ADeliveryNpcCharacter* Npc, FName Reason,
                                                                  TArray<FDeliveryNpcMessageCancellation>& OutCancelled,
                                                                  bool bLeavingWorld)
{
    check(IsInGameThread());
    OutCancelled.Reset();
    if (!IsServerWorld() || !Npc || Npc->GetWorld() != GetWorld())
        return;
    for (auto& Pair : NpcReplies)
    {
        auto& Record = Pair.Value;
        if (Record.Message.Receiver.Get() == Npc)
            InvalidateNpcMessage(Record, Reason, OutCancelled);
        if (Record.Reply.Npc.Get() == Npc && (bLeavingWorld || !Record.Reply.bPlayerReply))
            AddNpcCancellation(Record, Reason, OutCancelled);
    }
    InvalidateNpcDescendants(Reason, OutCancelled);
}

/// @brief 表现组件报告这轮回复播放到哪了：开始播、播完，或者被打断
/// 被打断时作废这轮回复和后面的接话；开始和播完只记录一次，并记下时间供清理时使用
void UDeliveryConversationTurnSubsystem::ReportNpcReplyPresentation(FGuid ReplyId, EDeliveryNpcReplyPresentation State,
                                                                    TArray<FDeliveryNpcMessageCancellation>& OutCancelled)
{
    check(IsInGameThread());
    OutCancelled.Reset();
    auto* Record = NpcReplies.Find(ReplyId);
    if (!IsServerWorld() || !Record || Record->bInvalid)
        return;
    if (State == EDeliveryNpcReplyPresentation::Interrupted)
    {
        AddNpcCancellation(*Record, TEXT("NpcReplyInterrupted"), OutCancelled);
        InvalidateNpcDescendants(TEXT("NpcReplyInterrupted"), OutCancelled);
        return;
    }

    // Presentation 已经不是 Pending 时不再改；传入的 State 是 Pending 也不处理
    if (Record->Presentation != EDeliveryNpcReplyPresentation::Pending || State == EDeliveryNpcReplyPresentation::Pending)
        return;
    Record->Presentation = State;
    Record->TerminalAt = FPlatformTime::Seconds();
}

/// @brief 定期清理接话记录：链超时、有人不在了、等前一位开口等太久的，都作废
/// 作废或播完的记录保留一段时间（NpcReceiptRetentionSeconds），让旧声音有时间停掉；还被后续回复引用的不删
void UDeliveryConversationTurnSubsystem::PruneNpcMessages()
{
    TArray<FDeliveryNpcMessageCancellation> Cancelled;
    const double Now = FPlatformTime::Seconds();
    for (auto& Pair : NpcReplies)
    {
        auto& Record = Pair.Value;
        if (Record.bInvalid)
            continue;
        const bool bExpired = Now - Record.ChainStartedAt >= MaxNpcChainSeconds;
        const bool bMissing =
            !IsNpcHere(Record.Reply.Npc.Get()) || (Record.bMessagePrepared && !IsNpcHere(Record.Message.Receiver.Get()));

        // 这轮在回答另一位 NPC，但从 CreatedAt 起超过 MaxNpcAudioWaitSeconds 仍不能开始播（CanStartNpcReplyPresentation 返回 false），也作废
        const bool bWaitExpired =
            Record.Reply.ParentMessage.IsValid() && Record.Presentation == EDeliveryNpcReplyPresentation::Pending &&
            Now - Record.CreatedAt >= MaxNpcAudioWaitSeconds && !CanStartNpcReplyPresentation(Record.Reply.ReplyId);
        if (bExpired || bMissing || bWaitExpired)
            AddNpcCancellation(Record, bMissing ? TEXT("NpcMessageParticipantLost") : TEXT("NpcMessageExpired"), Cancelled);
    }
    InvalidateNpcDescendants(TEXT("NpcMessageParentExpired"), Cancelled);

    // Referenced 收集还被有效回复当作 ParentMessage 的记录编号，这些记录不删
    TSet<FGuid> Referenced;
    for (const auto& Pair : NpcReplies)
        if (!Pair.Value.bInvalid && Pair.Value.Reply.ParentMessage.IsValid())
            Referenced.Add(Pair.Value.Reply.ParentMessage.MessageId);
    for (auto It = NpcReplies.CreateIterator(); It; ++It)
        if (It.Value().TerminalAt >= 0 && Now - It.Value().TerminalAt >= NpcReceiptRetentionSeconds &&
            !Referenced.Contains(It.Key()))
            It.RemoveCurrent();

    // Cancelled 不为空时调用 OnNpcMessagesInvalidated.Broadcast，收到通知的代码停掉对应的声音和动作
    if (!Cancelled.IsEmpty())
        OnNpcMessagesInvalidated.Broadcast(Cancelled);
}
