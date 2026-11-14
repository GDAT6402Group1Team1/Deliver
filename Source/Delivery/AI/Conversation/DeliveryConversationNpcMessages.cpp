#include "AI/Conversation/DeliveryConversationTurnSubsystem.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "HAL/PlatformTime.h"

bool DeliveryNpcMessages::IsValidReplyText(const FString& Text,bool bAllowEmpty)
{
    if (Text.IsEmpty()) return bAllowEmpty;
    if (Text.Len()>240 || Text.TrimStartAndEnd().IsEmpty()) return false;
    int32 Characters=0;
    for (int32 Index=0;Index<Text.Len();++Index)
    {
        const uint32 Character=static_cast<uint32>(Text[Index]);
        if (Character>=0xD800 && Character<=0xDBFF)
        {
            if (++Index>=Text.Len()) return false;
            const uint32 Low=static_cast<uint32>(Text[Index]);
            if (Low<0xDC00 || Low>0xDFFF) return false;
        }
        else if (Character>=0xDC00 && Character<=0xDFFF) return false;
        if (++Characters>120) return false;
    }
    return true;
}

bool UDeliveryConversationTurnSubsystem::IsNpcHere(const ADeliveryNpcCharacter* Npc) const
{
    return IsServerWorld() && IsValid(Npc) && !Npc->IsActorBeingDestroyed() &&
        Npc->HasAuthority() && Npc->GetWorld()==GetWorld();
}

bool UDeliveryConversationTurnSubsystem::RegisterNpcReply(const FDeliveryNpcReplyRegistration& Reply)
{
    check(IsInGameThread());
    if (!IsNpcHere(Reply.Npc.Get()) || !Reply.RequestId.IsValid() || !Reply.ReplyId.IsValid() ||
        Reply.Depth<0 || Reply.Depth>3 || NpcReplies.Contains(Reply.ReplyId) || NpcReplies.Num()>=MaxNpcReplyRecords) return false;
    const double Now=FPlatformTime::Seconds();
    FGuid ChainId=Reply.ReplyId;
    double ChainStartedAt=Now;
    if (Reply.ParentMessage.IsValid())
    {
        FDeliveryNpcMessage Parent;
        if (!GetNpcMessage(Reply.ParentMessage,Reply.Npc.Get(),Parent) || !Parent.bNeedsReply ||
            Parent.Depth!=Reply.Depth) return false;
        const auto* Ancestor=NpcReplies.Find(Reply.ParentMessage.MessageId);
        if (!Ancestor || Ancestor->bInvalid) return false;
        ChainId=Ancestor->ChainId;
        ChainStartedAt=Ancestor->ChainStartedAt;
    }
    FNpcReplyRecord Record;
    Record.Reply=Reply;
    Record.ChainId=ChainId;
    Record.CreatedAt=Now;
    Record.ChainStartedAt=ChainStartedAt;
    NpcReplies.Add(Reply.ReplyId,MoveTemp(Record));
    return true;
}

bool UDeliveryConversationTurnSubsystem::TryPrepareNpcMessage(ADeliveryNpcCharacter* Sender,FGuid RequestId,
    FGuid ReplyId,ADeliveryNpcCharacter* Receiver,const FString& Text,FGuid ReplyToEventId,FDeliveryNpcMessage& OutMessage)
{
    check(IsInGameThread());
    OutMessage={};
    auto* Record=NpcReplies.Find(ReplyId);
    if (!IsNpcHere(Sender) || !IsNpcHere(Receiver) || Sender==Receiver || !Record ||
        Record->Reply.Npc.Get()!=Sender || Record->Reply.RequestId!=RequestId || Record->bInvalid ||
        Record->bMessagePrepared || Record->Reply.Depth>=3 || !DeliveryNpcMessages::IsValidReplyText(Text,false) ||
        NextNpcMessageGeneration==MAX_uint32 ||
        FPlatformTime::Seconds()-Record->ChainStartedAt>=MaxNpcChainSeconds) return false;
    if (Record->Reply.ParentMessage.IsValid() && !IsNpcMessageCurrent(Record->Reply.ParentMessage,Sender)) return false;
    FDeliveryConversationTurnLease Lease;
    if (GetLease(Receiver,Lease)) return false;
    // GetLease 可通知过期占用，回调结束后重新取得当前记录
    Record=NpcReplies.Find(ReplyId);
    if (!IsNpcHere(Sender) || !IsNpcHere(Receiver) || !Record || Record->bInvalid || Record->bMessagePrepared ||
        Record->Reply.Npc.Get()!=Sender || Record->Reply.RequestId!=RequestId ||
        (Record->Reply.ParentMessage.IsValid() && !IsNpcMessageCurrent(Record->Reply.ParentMessage,Sender))) return false;
    Record->bMessagePrepared=true;
    Record->bMessageActive=true;
    auto& Message=Record->Message;
    Message.Handle.WorldEpoch=WorldEpoch;
    Message.Handle.MessageId=ReplyId;
    Message.Handle.Generation=++NextNpcMessageGeneration;
    Message.Sender=Sender;
    Message.Receiver=Receiver;
    Message.Text=Text;
    Message.ReplyToEventId=ReplyToEventId;
    Message.Depth=Record->Reply.Depth+1;
    Message.bNeedsReply=Message.Depth<3;
    OutMessage=Message;
    return true;
}

bool UDeliveryConversationTurnSubsystem::GetNpcMessage(const FDeliveryNpcMessageHandle& Handle,
    const ADeliveryNpcCharacter* Receiver,FDeliveryNpcMessage& OutMessage,bool bAllowPrepared) const
{
    OutMessage={};
    const auto* Record=NpcReplies.Find(Handle.MessageId);
    if (!IsNpcHere(Receiver) || !Handle.IsValid() || Handle.WorldEpoch!=WorldEpoch || !Record ||
        Record->bInvalid || !Record->bMessageActive || !Record->bMessagePrepared ||
        (!bAllowPrepared && !Record->bMessageDelivered) || !(Record->Message.Handle==Handle) ||
        Record->Message.Receiver.Get()!=Receiver || !IsNpcHere(Record->Message.Sender.Get()) ||
        FPlatformTime::Seconds()-Record->ChainStartedAt>=MaxNpcChainSeconds) return false;
    OutMessage=Record->Message;
    return true;
}

bool UDeliveryConversationTurnSubsystem::ConfirmNpcMessageDelivery(const FDeliveryNpcMessageHandle& Handle,
    ADeliveryNpcCharacter* Receiver,bool bAccepted)
{
    check(IsInGameThread());
    FDeliveryNpcMessage Message;
    if (!GetNpcMessage(Handle,Receiver,Message,true)) return false;
    auto* Record=NpcReplies.Find(Handle.MessageId);
    if (!Record || Record->bMessageDelivered) return false;
    Record->bMessageDelivered=bAccepted;
    Record->bMessageActive=bAccepted;
    return bAccepted;
}

bool UDeliveryConversationTurnSubsystem::IsNpcMessageCurrent(const FDeliveryNpcMessageHandle& Handle,
    const ADeliveryNpcCharacter* Receiver) const
{
    FDeliveryNpcMessage Message;
    return GetNpcMessage(Handle,Receiver,Message) && Message.bNeedsReply;
}

bool UDeliveryConversationTurnSubsystem::WasNpcMessageDelivered(FGuid ReplyId,const ADeliveryNpcCharacter* Receiver) const
{
    const auto* Record=NpcReplies.Find(ReplyId);
    return IsNpcHere(Receiver) && Record && Record->bMessageDelivered && Record->Message.Receiver.Get()==Receiver;
}

FDeliveryNpcMessageHandle UDeliveryConversationTurnSubsystem::GetNpcMessageHandle(FGuid MessageId,
    const ADeliveryNpcCharacter* Receiver) const
{
    const auto* Record=NpcReplies.Find(MessageId);
    return Record && IsNpcMessageCurrent(Record->Message.Handle,Receiver)?Record->Message.Handle:FDeliveryNpcMessageHandle();
}

bool UDeliveryConversationTurnSubsystem::IsNpcReplyValid(FGuid ReplyId) const
{
    const auto* Record=NpcReplies.Find(ReplyId);
    return IsServerWorld() && Record && !Record->bInvalid && IsNpcHere(Record->Reply.Npc.Get()) &&
        FPlatformTime::Seconds()-Record->ChainStartedAt<MaxNpcChainSeconds;
}

bool UDeliveryConversationTurnSubsystem::CanStartNpcReplyPresentation(FGuid ReplyId) const
{
    const auto* Record=NpcReplies.Find(ReplyId);
    if (!IsServerWorld() || !Record || Record->bInvalid || !IsNpcHere(Record->Reply.Npc.Get()) ||
        FPlatformTime::Seconds()-Record->ChainStartedAt>=MaxNpcChainSeconds) return false;
    const auto* Current=Record;
    for (int32 Depth=0;Current->Reply.ParentMessage.IsValid();++Depth)
    {
        if (Depth>=3) return false;
        const auto ParentHandle=Current->Reply.ParentMessage;
        if (!IsNpcMessageCurrent(ParentHandle,Current->Reply.Npc.Get())) return false;
        const auto* Parent=NpcReplies.Find(ParentHandle.MessageId);
        if (!Parent || Parent->bInvalid || Parent->Presentation==EDeliveryNpcReplyPresentation::Interrupted ||
            Parent->Presentation==EDeliveryNpcReplyPresentation::Pending) return false;
        Current=Parent;
    }
    return true;
}

void UDeliveryConversationTurnSubsystem::AddNpcCancellation(FNpcReplyRecord& Record,FName Reason,
    TArray<FDeliveryNpcMessageCancellation>& OutCancelled)
{
    if (Record.bInvalid) return;
    Record.bInvalid=true;
    Record.TerminalAt=FPlatformTime::Seconds();
    Record.Presentation=EDeliveryNpcReplyPresentation::Interrupted;
    FDeliveryNpcMessageCancellation Cancel;
    Cancel.Npc=Record.Reply.Npc;
    Cancel.RequestId=Record.Reply.RequestId;
    Cancel.ReplyId=Record.Reply.ReplyId;
    Cancel.ActionId=Record.Reply.ActionId;
    Cancel.PlanRevision=Record.Reply.PlanRevision;
    Cancel.bCancelAction=Record.Reply.bOwnsAction && !Record.Reply.bPlayerReply;
    Cancel.Reason=Reason;
    OutCancelled.Add(Cancel);
    InvalidateNpcMessage(Record,Reason,OutCancelled);
}

void UDeliveryConversationTurnSubsystem::InvalidateNpcMessage(FNpcReplyRecord& Record,FName Reason,
    TArray<FDeliveryNpcMessageCancellation>& OutCancelled)
{
    if (!Record.bMessageActive) return;
    Record.bMessageActive=false;
    Record.Message.bNeedsReply=false;
    if (Record.bMessageDelivered)
    {
        FDeliveryNpcMessageCancellation Cancel;
        Cancel.Npc=Record.Message.Receiver;
        Cancel.IncomingMessage=Record.Message.Handle;
        Cancel.Reason=Reason;
        OutCancelled.Add(Cancel);
    }
}

void UDeliveryConversationTurnSubsystem::InvalidateNpcDescendants(FName Reason,TArray<FDeliveryNpcMessageCancellation>& OutCancelled)
{
    // 新节点只引用较早消息且深度递增，最多三轮即可使全部后代失效
    for (int32 Pass=0;Pass<4;++Pass)
    {
        bool bChanged=false;
        for (auto& Pair:NpcReplies)
        {
            auto& Record=Pair.Value;
            if (!Record.bInvalid && Record.Reply.ParentMessage.IsValid() &&
                !IsNpcMessageCurrent(Record.Reply.ParentMessage,Record.Reply.Npc.Get()))
            { AddNpcCancellation(Record,Reason,OutCancelled); bChanged=true; }
        }
        if (!bChanged) break;
    }
}

void UDeliveryConversationTurnSubsystem::InvalidateNpcBranchesFor(ADeliveryNpcCharacter* Npc,FName Reason,
    TArray<FDeliveryNpcMessageCancellation>& OutCancelled,bool bLeavingWorld)
{
    check(IsInGameThread());
    OutCancelled.Reset();
    if (!IsServerWorld() || !Npc || Npc->GetWorld()!=GetWorld()) return;
    for (auto& Pair:NpcReplies)
    {
        auto& Record=Pair.Value;
        if (Record.Message.Receiver.Get()==Npc) InvalidateNpcMessage(Record,Reason,OutCancelled);
        if (Record.Reply.Npc.Get()==Npc && (bLeavingWorld || !Record.Reply.bPlayerReply))
            AddNpcCancellation(Record,Reason,OutCancelled);
    }
    InvalidateNpcDescendants(Reason,OutCancelled);
}

void UDeliveryConversationTurnSubsystem::ReportNpcReplyPresentation(FGuid ReplyId,
    EDeliveryNpcReplyPresentation State,TArray<FDeliveryNpcMessageCancellation>& OutCancelled)
{
    check(IsInGameThread());
    OutCancelled.Reset();
    auto* Record=NpcReplies.Find(ReplyId);
    if (!IsServerWorld() || !Record || Record->bInvalid) return;
    if (State==EDeliveryNpcReplyPresentation::Interrupted)
    {
        AddNpcCancellation(*Record,TEXT("NpcReplyInterrupted"),OutCancelled);
        InvalidateNpcDescendants(TEXT("NpcReplyInterrupted"),OutCancelled);
        return;
    }
    if (Record->Presentation!=EDeliveryNpcReplyPresentation::Pending || State==EDeliveryNpcReplyPresentation::Pending) return;
    Record->Presentation=State;
    Record->TerminalAt=FPlatformTime::Seconds();
}

void UDeliveryConversationTurnSubsystem::PruneNpcMessages()
{
    TArray<FDeliveryNpcMessageCancellation> Cancelled;
    const double Now=FPlatformTime::Seconds();
    for (auto& Pair:NpcReplies)
    {
        auto& Record=Pair.Value;
        if (Record.bInvalid) continue;
        const bool bExpired=Now-Record.ChainStartedAt>=MaxNpcChainSeconds;
        const bool bMissing=!IsNpcHere(Record.Reply.Npc.Get()) ||
            (Record.bMessagePrepared && !IsNpcHere(Record.Message.Receiver.Get()));
        const bool bWaitExpired=Record.Reply.ParentMessage.IsValid() &&
            Record.Presentation==EDeliveryNpcReplyPresentation::Pending && Now-Record.CreatedAt>=MaxNpcAudioWaitSeconds &&
            !CanStartNpcReplyPresentation(Record.Reply.ReplyId);
        if (bExpired || bMissing || bWaitExpired)
            AddNpcCancellation(Record,bMissing?TEXT("NpcMessageParticipantLost"):TEXT("NpcMessageExpired"),Cancelled);
    }
    InvalidateNpcDescendants(TEXT("NpcMessageParentExpired"),Cancelled);
    // 回执保留到旧声音清理完；仍被活后代引用的终态绝不提前移除
    TSet<FGuid> Referenced;
    for (const auto& Pair:NpcReplies)
        if (!Pair.Value.bInvalid && Pair.Value.Reply.ParentMessage.IsValid()) Referenced.Add(Pair.Value.Reply.ParentMessage.MessageId);
    for (auto It=NpcReplies.CreateIterator();It;++It)
        if (It.Value().TerminalAt>=0 && Now-It.Value().TerminalAt>=NpcReceiptRetentionSeconds && !Referenced.Contains(It.Key()))
            It.RemoveCurrent();
    if (!Cancelled.IsEmpty()) OnNpcMessagesInvalidated.Broadcast(Cancelled);
}
