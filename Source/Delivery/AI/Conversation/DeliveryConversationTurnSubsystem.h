#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "AI/Conversation/DeliveryNpcMessageTypes.h"
#include "Speech/DeliverySpeechPlayerContext.h"
#include "DeliveryConversationTurnSubsystem.generated.h"

class ADeliveryNpcCharacter;
class APlayerController;
class APawn;

/// 服务器给这一轮对话生成的发言编号：本机之后调用 ServerFinishVoice、ServerSubmitVoice、ServerCancelVoice 时都带上它
/// 服务器比较 WorldEpoch、Token、InputSequence，就知道是哪一轮、是不是已经过时的旧请求
USTRUCT()
struct FDeliveryConversationTurnHandle
{
    GENERATED_BODY()

    /// 生成这个发言编号时的世界编号，换了世界就作废
    UPROPERTY() FGuid WorldEpoch;

    /// 服务器发的这一轮编号，也是这句话的消息编号
    UPROPERTY() FGuid Token;

    // 这个玩家的打字和开麦共用一个递增序号，旧序号的请求一律作废
    UPROPERTY() uint64 InputSequence = 0;

    /// @brief 三项都有值才算有效
    bool IsValid() const { return WorldEpoch.IsValid() && Token.IsValid() && InputSequence > 0; }

    /// @brief 三项都相同才是同一轮
    bool operator==(const FDeliveryConversationTurnHandle& Other) const
    { return WorldEpoch == Other.WorldEpoch && Token == Other.Token && InputSequence == Other.InputSequence; }
};

/// 一轮对话走到哪一步：还在录音、说完了在等识别文字、文字已经交给 NPC 在等它回话
enum class EDeliveryConversationTurnPhase : uint8 { Recording, FinalPending, AwaitingReply };

/// 开始或推进一轮时的结果；AlreadyActive 是同一请求重发，Busy 是 NPC 正被别人占着
enum class EDeliveryConversationTurnResult : uint8 { Accepted, AlreadyActive, Busy, Invalid, Stale, WrongPhase };

/// 一轮被动结束的原因：超时、玩家换了控制对象、NPC 不在了
enum class EDeliveryConversationLeaseEnd : uint8 { TimedOut, OwnerChanged, TargetGone };

/// 一个 NPC 当前被哪个玩家的哪一轮占着，从开口到 NPC 回完话
struct FDeliveryConversationTurnLease
{
    /// 这一轮的发言编号
    FDeliveryConversationTurnHandle Handle;

    // 客户端编号只用来认出重发的开始请求，之后的命令都认 Handle
    FGuid ClientToken;

    /// 说话的玩家
    TWeakObjectPtr<APlayerController> Owner;

    /// 开口时玩家控制的角色
    TWeakObjectPtr<APawn> PawnAtBegin;

    // 开麦时记下玩家原来控制的角色，说话途中上下车不会丢掉这句话
    FDeliverySpeechPlayerContext VoiceContext;

    /// 是不是开麦说的
    bool bVoice = false;

    /// 玩家身份从什么时候开始对不上
    double VoiceContextPendingSince = -1;

    /// 被占住的 NPC
    TWeakObjectPtr<ADeliveryNpcCharacter> Npc;

    /// 文字交给 NPC 时用的消息编号，交给 NPC 之前为空
    FGuid MessageId;

    /// 走到哪一步
    EDeliveryConversationTurnPhase Phase = EDeliveryConversationTurnPhase::Recording;

    /// 开始时间
    double StartedAt = 0;

    /// 这一步的截止时间，过了就超时
    double Deadline = 0;
};

/// @brief NPC 之间的接话被取消时通知，各 NPC 的 Agent 据此停止对应的回复和动作
DECLARE_MULTICAST_DELEGATE_OneParam(FDeliveryNpcMessagesInvalidated,const TArray<FDeliveryNpcMessageCancellation>& /* 被取消的接话 */);

/// @brief 一轮对话因为超时、玩家换了角色或 NPC 不在了而结束时，通知交谈组件
DECLARE_MULTICAST_DELEGATE_TwoParams(FDeliveryConversationLeaseExpired, const FDeliveryConversationTurnLease& /* 结束的那一轮 */, EDeliveryConversationLeaseEnd /* 结束原因 */);

/**
 * @brief 服务器记录每个 NPC 此刻在听谁说话，保证一个 NPC 同时只听一个人说
 *
 * 两个玩家同时找同一位 NPC 说话时，NPC 只能先听一个、回完再听下一个；
 * NPC 正在听某位玩家说话、还没回完时，这里说 NPC 被这一轮占着，Leases 里有这位 NPC 的记录。每个 World 一份
 *
 * 玩家开口：从 TryBeginText 或 TryBeginVoice 开始，CommitFinal 记下文字已经交给 NPC，之后等 NPC 回话
 * 结束：NPC 回复播完时调用 ReleaseReply，删掉这一轮的记录，NPC 可以听别人说话；
 * 玩家取消时调用 Cancel；超时由 Tick 删掉记录并发 OnLeaseExpired 通知交谈组件
 * NPC 接话：从 RegisterNpcReply 开始，TryPrepareNpcMessage 准备给下一位 NPC 的一句
 * 只记谁占着谁、到什么时候，距离检查、发声和界面都由交谈组件负责
 */
UCLASS()
class DELIVERY_API UDeliveryConversationTurnSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    // ── 生命周期 ──
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickableWhenPaused() const override { return true; }

    /// 这个世界的编号，带着旧世界编号的发言编号据此作废
    FGuid GetWorldEpoch() const { return WorldEpoch; }

    // ── 开口 ──
    // TryBeginText / TryBeginVoice → 开麦时玩家说完调用 FinishRecording → ValidateFinal 核对 → CommitFinal 记下文字已交给 NPC
    // 同一请求重发返回 AlreadyActive，不重置时限；被顶掉的旧轮次放进 OutReplaced，由调用方打断旧回复

    /// @brief 打字和开麦共用的开始入口，看 NPC 是不是正被别的玩家占着，决定接受还是拒绝
    EDeliveryConversationTurnResult TryBegin(APlayerController* Owner, ADeliveryNpcCharacter* Npc,
        FGuid Token /* 本机给这句话的编号 */, uint64 InputSequence /* 这个玩家第几次输入 */, FDeliveryConversationTurnHandle& OutHandle,
        TArray<FDeliveryConversationTurnLease>& OutReplaced /* 被顶掉的旧轮次 */);

    /// @brief 打字开始一轮
    EDeliveryConversationTurnResult TryBeginText(APlayerController* Owner, ADeliveryNpcCharacter* Npc,
        FGuid Token /* 本机给这句话的编号 */, uint64 InputSequence /* 这个玩家第几次输入 */, FDeliveryConversationTurnHandle& OutHandle,
        TArray<FDeliveryConversationTurnLease>& OutReplaced /* 被顶掉的旧轮次 */);

    /// @brief 开麦开始一轮，同时记下玩家开口时控制的角色
    EDeliveryConversationTurnResult TryBeginVoice(APlayerController* Owner, ADeliveryNpcCharacter* Npc,
        FGuid Token /* 本机给这句话的编号 */, uint64 InputSequence /* 这个玩家第几次输入 */, FDeliveryConversationTurnHandle& OutHandle,
        TArray<FDeliveryConversationTurnLease>& OutReplaced /* 被顶掉的旧轮次 */);

    /// @brief 开麦这一轮里，玩家身份是否已和开口时对上
    EDeliverySpeechPlayerContextState GetVoiceContextState(APlayerController* Owner,
        const FDeliveryConversationTurnHandle& Handle);

    /// @brief 玩家说完松键，开始等识别文字，最多等 5 秒；重复松键不延长
    EDeliveryConversationTurnResult FinishRecording(APlayerController* Owner, const FDeliveryConversationTurnHandle& Handle);

    /// @brief 文字交给 NPC 之前检查：Handle 属于这个玩家、对象 NPC 没换、Phase 是 FinalPending（等文字）
    EDeliveryConversationTurnResult ValidateFinal(APlayerController* Owner, ADeliveryNpcCharacter* Npc,
        const FDeliveryConversationTurnHandle& Handle);

    /// @brief 文字已交给 NPC，这一轮改为等回话；MessageId 必须等于 Handle.Token，只能交一次
    /// @return 接受时返回 true；NPC 不肯听时由调用方再 Cancel
    bool CommitFinal(APlayerController* Owner, ADeliveryNpcCharacter* Npc,
        const FDeliveryConversationTurnHandle& Handle, FGuid MessageId /* 交给 NPC 的消息编号 */);

    // ── 结束 ──
    // 让 NPC 不再被这一轮占着，别人可以找它说话；被动结束的情况在下面 OnLeaseExpired 通知交谈组件

    /// @brief 玩家主动取消自己的一轮
    bool Cancel(APlayerController* Owner, const FDeliveryConversationTurnHandle& Handle,
        FDeliveryConversationTurnLease* OutRemoved = nullptr);

    /// @brief 玩家退出时，删掉他所有轮次的记录，这些 NPC 可以听别人说话了
    void ReleasePlayer(APlayerController* Owner, TArray<FDeliveryConversationTurnLease>& OutRemoved);

    /// @brief NPC 这句回复真的播完或明确失败时让 NPC 不再被占着；动作结束、模型返回完毕都不算
    bool ReleaseReply(ADeliveryNpcCharacter* Npc, FGuid MessageId /* 玩家那句话的编号 */,
        FDeliveryConversationTurnLease* OutRemoved = nullptr);

    /// @brief 查这个 NPC 当前被哪一轮占着
    bool GetLease(ADeliveryNpcCharacter* Npc, FDeliveryConversationTurnLease& OutLease);

    // ── 接话 ──
    // NPC A 的回复被采纳后 RegisterNpcReply 登记 → TryPrepareNpcMessage 准备一句给 B → ConfirmNpcMessageDelivery 确认 B 收下
    // A 的回复开始播、播完、被打断都用 ReportNpcReplyPresentation 报告；A 被打断时，接在它后面的话一并作废

    /// @brief NPC 的回复被采纳时登记，之后它才能把话递给别的 NPC
    bool RegisterNpcReply(const FDeliveryNpcReplyRegistration& Reply);

    /// @brief 准备一句 NPC 说给另一位 NPC 的话：接收者此刻没被玩家占着、一串最多接 3 层、每条回复只能递一句
    bool TryPrepareNpcMessage(ADeliveryNpcCharacter* Sender /* 说话的 NPC */,FGuid RequestId /* 这条回复来自哪次模型请求 */,FGuid ReplyId /* 回复编号 */,
        ADeliveryNpcCharacter* Receiver /* 听的 NPC */,const FString& Text,FGuid ReplyToEventId /* 在回答哪件事 */,FDeliveryNpcMessage& OutMessage);

    /// @brief 接收的 NPC 收下或拒绝这句话
    bool ConfirmNpcMessageDelivery(const FDeliveryNpcMessageHandle& Handle,ADeliveryNpcCharacter* Receiver,bool bAccepted /* 对方收下了没有 */);

    /// @brief 取一句递给自己的话，bAllowPrepared 时还没确认送达的也能取
    bool GetNpcMessage(const FDeliveryNpcMessageHandle& Handle,const ADeliveryNpcCharacter* Receiver,
        FDeliveryNpcMessage& OutMessage,bool bAllowPrepared=false /* true 时还没确认送达的也取 */) const;

    /// @brief 这句话还算数：没作废、确实是给这位 NPC 的
    bool IsNpcMessageCurrent(const FDeliveryNpcMessageHandle& Handle,const ADeliveryNpcCharacter* Receiver) const;

    /// @brief 按消息编号找到递给这位 NPC 的那句
    FDeliveryNpcMessageHandle GetNpcMessageHandle(FGuid MessageId,const ADeliveryNpcCharacter* Receiver) const;

    /// @brief 这条回复递出的话对方收到没有
    bool WasNpcMessageDelivered(FGuid ReplyId,const ADeliveryNpcCharacter* Receiver) const;

    /// @brief 这条回复能不能开始播：记录还在、没作废
    bool CanStartNpcReplyPresentation(FGuid ReplyId) const;

    /// @brief 有没有这条回复的记录
    bool HasNpcReplyRecord(FGuid ReplyId) const { return NpcReplies.Contains(ReplyId); }

    /// @brief 这条回复没被作废
    bool IsNpcReplyValid(FGuid ReplyId) const;

    /// @brief 报告回复播放到哪了：开始、播完、被打断；被打断时接在后面的话一并作废
    void ReportNpcReplyPresentation(FGuid ReplyId,EDeliveryNpcReplyPresentation State,
        TArray<FDeliveryNpcMessageCancellation>& OutCancelled);

    /// @brief 这位 NPC 倒地或离开时，作废跟他有关的所有接话
    void InvalidateNpcBranchesFor(ADeliveryNpcCharacter* Npc,FName Reason,
        TArray<FDeliveryNpcMessageCancellation>& OutCancelled,bool bLeavingWorld=false /* true 表示 NPC 正在离开这个世界 */);

    /// 有接话被作废时通知
    FDeliveryNpcMessagesInvalidated OnNpcMessagesInvalidated;

    // ── 结束 ──

    /// 只在超时、玩家换了角色、NPC 被销毁时通知；玩家主动取消的、被新一句顶掉的不在这里通知，
    /// 它们从 Cancel 和 TryBegin 的返回值里交给调用方
    FDeliveryConversationLeaseExpired OnLeaseExpired;

    /// 一次录音最长 15 秒，说完后最多等 5 秒识别文字
    static constexpr double RecordingTimeoutSeconds = 15.0;

    /// 说完后最多等 5 秒识别文字
    static constexpr double FinalTextTimeoutSeconds = 5.0;

    // NPC 的回复 300 秒还没播完，就当它出了故障，删掉这一轮的记录，免得 NPC 永远不再听人说话
    // 正常情况下回复播完就调用 ReleaseReply 删掉记录，这个时间不代表回复要播这么久
    static constexpr double ReplyFailureTimeoutSeconds = 300.0;

protected:
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
    friend class FDeliveryConversationLeaseWorldTest;
    friend class FDeliveryConversationVehicleVoiceTest;
    friend class FDeliveryNpcMessageFlowTest;

    /// 每个玩家用过的最大序号和最近的客户端编号，用来认出重发和过期请求
    struct FOwnerHistory
    {
        uint64 HighestInputSequence = 0;
        TArray<FGuid> RecentTokens;
    };

    // ── 开口 ──
    // 内部检查：Handle、世界和玩家是否还有效

    /// TryBeginText 和 TryBeginVoice 的共同实现：查序号、查重发、查 NPC 是否被别人占着，顶掉同一玩家的旧轮次
    EDeliveryConversationTurnResult BeginInternal(APlayerController* Owner, ADeliveryNpcCharacter* Npc,
        FGuid Token /* 本机给这句话的编号 */, uint64 InputSequence /* 这个玩家第几次输入 */, bool bText /* true 表示打字 */, bool bVoice /* true 表示开麦 */, FDeliveryConversationTurnHandle& OutHandle,
        TArray<FDeliveryConversationTurnLease>& OutReplaced);

    /// 只在服务器世界工作
    bool IsServerWorld() const;

    /// 玩家控制器有效且属于这个世界
    bool IsValidOwner(const APlayerController* Owner) const;

    /// 按 Owner 和 Handle 找到这一轮的记录
    FDeliveryConversationTurnLease* FindOwned(APlayerController* Owner, const FDeliveryConversationTurnHandle& Handle,
        bool bAllowPendingVoice = false /* true 时玩家身份暂时认不出的开麦轮次也算 */);

    /// 清掉 NPC 或玩家已经不在的那些占用记录
    void PruneInvalidLeases();

    // ── 接话 ──
    // 每条记录从登记回复开始，到回复播完或作废为止；最多保留 128 条，一串接话最长 120 秒

    /// 一条 NPC 回复的接话记录：递出的话、所在的一串、播放状态
    struct FNpcReplyRecord
    {
        FDeliveryNpcReplyRegistration Reply;
        FDeliveryNpcMessage Message;
        FGuid ChainId;
        EDeliveryNpcReplyPresentation Presentation=EDeliveryNpcReplyPresentation::Pending;
        double CreatedAt=0;
        double ChainStartedAt=0;
        double TerminalAt=-1;
        bool bMessagePrepared=false;
        bool bMessageDelivered=false;
        bool bMessageActive=false;
        bool bInvalid=false;
    };

    /// NPC 有效且在这个世界
    bool IsNpcHere(const ADeliveryNpcCharacter* Npc) const;

    /// 记一条作废通知
    void AddNpcCancellation(FNpcReplyRecord& Record,FName Reason,TArray<FDeliveryNpcMessageCancellation>& OutCancelled);

    /// 作废这条回复递出的话
    void InvalidateNpcMessage(FNpcReplyRecord& Record,FName Reason,TArray<FDeliveryNpcMessageCancellation>& OutCancelled);

    /// 作废所有接在已作废回复后面的话
    void InvalidateNpcDescendants(FName Reason,TArray<FDeliveryNpcMessageCancellation>& OutCancelled);

    /// 清掉过期和超出数量的记录
    void PruneNpcMessages();

    /// 最多保留 128 条接话记录
    static constexpr int32 MaxNpcReplyRecords=128;

    /// 一条回复最多等 60 秒开始播
    static constexpr double MaxNpcAudioWaitSeconds=60;

    /// 一串接话最长 120 秒
    static constexpr double MaxNpcChainSeconds=120;

    /// 播完的记录再留 300 秒备查
    static constexpr double NpcReceiptRetentionSeconds=300;

    /// 按回复编号存的接话记录
    TMap<FGuid,FNpcReplyRecord> NpcReplies;

    /// 下一句接话的版本号
    uint32 NextNpcMessageGeneration=0;

    // ── 开口 ──
    // 每个 NPC 正被哪一轮占着和每个玩家用过的序号

    /// 这个世界的编号
    FGuid WorldEpoch;

    /// 每个 NPC 正被哪一轮占着
    TMap<TWeakObjectPtr<ADeliveryNpcCharacter>, FDeliveryConversationTurnLease> Leases;

    /// 每个玩家用过的序号
    TMap<TWeakObjectPtr<APlayerController>, FOwnerHistory> Owners;

    /// 下次清理的时间
    double NextPruneAt = 0;

    /// 子系统正在关闭
    bool bDeinitializing = false;
};
