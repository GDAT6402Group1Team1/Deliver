#pragma once

#include "CoreMinimal.h"

class ADeliveryNpcCharacter;
class AActor;

// 服务器消息编号只关联这次对话，不携带玩家或任务权限
struct FDeliveryNpcMessageHandle
{
    FGuid WorldEpoch;
    FGuid MessageId;
    uint32 Generation=0;
    bool IsValid() const { return WorldEpoch.IsValid() && MessageId.IsValid() && Generation>0; }
    bool operator==(const FDeliveryNpcMessageHandle& Other) const
    { return WorldEpoch==Other.WorldEpoch && MessageId==Other.MessageId && Generation==Other.Generation; }
};

enum class EDeliveryNpcReplyPresentation : uint8 { Pending, Completed, Unavailable, Interrupted };

struct FDeliveryNpcReplyRegistration
{
    TWeakObjectPtr<ADeliveryNpcCharacter> Npc;
    FGuid RequestId;
    FGuid ReplyId;
    FDeliveryNpcMessageHandle ParentMessage;
    FGuid ActionId;
    int32 PlanRevision=0;
    int32 Depth=0;
    bool bOwnsAction=false;
    bool bPlayerReply=false;
};

struct FDeliveryNpcMessage
{
    FDeliveryNpcMessageHandle Handle;
    TWeakObjectPtr<ADeliveryNpcCharacter> Sender;
    TWeakObjectPtr<ADeliveryNpcCharacter> Receiver;
    FGuid ReplyToEventId;
    FString Text;
    int32 Depth=0;
    bool bNeedsReply=false;
};

// 先使关系失效，再由各 Agent 按本次编号清理，旧回调不会清掉新安排
struct FDeliveryNpcMessageCancellation
{
    TWeakObjectPtr<ADeliveryNpcCharacter> Npc;
    FDeliveryNpcMessageHandle IncomingMessage;
    FGuid RequestId;
    FGuid ReplyId;
    FGuid ActionId;
    int32 PlanRevision=0;
    bool bCancelAction=false;
    FName Reason;
};

struct FDeliveryNpcCompleteReply
{
    FGuid ReplyId;
    FGuid MessageId;
    FString Text;
    FName Emotion;
    int32 Depth=0;
    TWeakObjectPtr<AActor> Listener;
};

namespace DeliveryNpcMessages
{
// Python 的120字符按Unicode标量计数，不截断UTF16代理对
DELIVERY_API bool IsValidReplyText(const FString& Text,bool bAllowEmpty=true);
}
