#pragma once

#include "CoreMinimal.h"

class ADeliveryNpcCharacter;
class AActor;

/// NPC 之间一句接话的编号：WorldEpoch、MessageId、Generation 三项都对上才是同一条消息
struct FDeliveryNpcMessageHandle
{
    /// 哪个世界
    FGuid WorldEpoch;

    /// 这句话的编号
    FGuid MessageId;

    /// 版本号，同一编号重发时加一，旧的作废
    uint32 Generation=0;

    /// @brief 三项都有值才有效
    bool IsValid() const { return WorldEpoch.IsValid() && MessageId.IsValid() && Generation>0; }

    /// @brief 三项都相同才是同一句
    bool operator==(const FDeliveryNpcMessageHandle& Other) const
    { return WorldEpoch==Other.WorldEpoch && MessageId==Other.MessageId && Generation==Other.Generation; }
};

/// 一条回复播到哪了：等着、播完、播不了、被打断
enum class EDeliveryNpcReplyPresentation : uint8 { Pending, Completed, Unavailable, Interrupted };

/// 回复被采纳时向轮次子系统登记的内容，之后才能把台词递给别的 NPC
struct FDeliveryNpcReplyRegistration
{
    /// 回复的 NPC
    TWeakObjectPtr<ADeliveryNpcCharacter> Npc;

    /// 这条回复来自哪次模型请求
    FGuid RequestId;

    /// 回复编号
    FGuid ReplyId;

    /// 这条回复在接哪句话，玩家开的头为空
    FDeliveryNpcMessageHandle ParentMessage;

    /// 一起采纳的动作
    FGuid ActionId;

    /// 采纳时的计划修订号
    int32 PlanRevision=0;

    /// 第几层接话
    int32 Depth=0;

    /// 这条回复带着自己的动作，回复作废时动作也要停
    bool bOwnsAction=false;

    /// 是在回答玩家
    bool bPlayerReply=false;
};

/// 一位 NPC 说给另一位 NPC 的一句话
struct FDeliveryNpcMessage
{
    /// 这句话的编号
    FDeliveryNpcMessageHandle Handle;

    /// 说的人
    TWeakObjectPtr<ADeliveryNpcCharacter> Sender;

    /// 听的人
    TWeakObjectPtr<ADeliveryNpcCharacter> Receiver;

    /// 在回应哪件事
    FGuid ReplyToEventId;

    /// 说的话
    FString Text;

    /// 第几层接话
    int32 Depth=0;

    /// 对方需要接着回话
    bool bNeedsReply=false;
};

/// 一条作废通知：轮次子系统先作废，再由各 Agent 按里面的编号清理，旧通知不会清掉新安排
struct FDeliveryNpcMessageCancellation
{
    /// 要清理的 NPC
    TWeakObjectPtr<ADeliveryNpcCharacter> Npc;

    /// 作废的是递给他的哪句话
    FDeliveryNpcMessageHandle IncomingMessage;

    /// 要取消的请求
    FGuid RequestId;

    /// 要取消的回复
    FGuid ReplyId;

    /// 要停的动作
    FGuid ActionId;

    /// 动作所属的计划修订号，对不上就不停，免得停掉新安排
    int32 PlanRevision=0;

    /// 是否连动作一起停
    bool bCancelAction=false;

    /// 作废原因
    FName Reason;
};

/// 一轮纯文字回复的完整内容，没有合成声音时用
struct FDeliveryNpcCompleteReply
{
    /// 回复编号
    FGuid ReplyId;

    /// 在回答哪句话
    FGuid MessageId;

    /// 完整台词
    FString Text;

    /// 情绪
    FName Emotion;

    /// 第几层接话
    int32 Depth=0;

    /// 说给谁听
    TWeakObjectPtr<AActor> Listener;
};

namespace DeliveryNpcMessages
{
/// @brief 台词是否合规：最多 120 个字，按 Unicode 字符数，和后端 Python 的算法一致
DELIVERY_API bool IsValidReplyText(const FString& Text,bool bAllowEmpty=true /* true 时空字符串也算合规 */);
}
