#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "AI/Conversation/DeliveryNpcMessageTypes.h"
#include "DeliveryConversationTurnSubsystem.generated.h"

class ADeliveryNpcCharacter;
class APlayerController;
class APawn;

USTRUCT()
struct FDeliveryConversationTurnHandle
{
    GENERATED_BODY()

    UPROPERTY() FGuid WorldEpoch;
    UPROPERTY() FGuid Token;
    // 同一 PC 的打字与语音共用递增序号，不使用麦克风会话代次
    UPROPERTY() uint64 InputSequence = 0;

    bool IsValid() const { return WorldEpoch.IsValid() && Token.IsValid() && InputSequence > 0; }
    bool operator==(const FDeliveryConversationTurnHandle& Other) const
    { return WorldEpoch == Other.WorldEpoch && Token == Other.Token && InputSequence == Other.InputSequence; }
};

enum class EDeliveryConversationTurnPhase : uint8 { Recording, FinalPending, AwaitingReply };
enum class EDeliveryConversationTurnResult : uint8 { Accepted, AlreadyActive, Busy, Invalid, Stale, WrongPhase };
enum class EDeliveryConversationLeaseEnd : uint8 { TimedOut, OwnerChanged, TargetGone };

struct FDeliveryConversationTurnLease
{
    FDeliveryConversationTurnHandle Handle;
    // 客户端编号只关联 Begin 重发与本机提示，后续命令使用服务器 Handle
    FGuid ClientToken;
    TWeakObjectPtr<APlayerController> Owner;
    TWeakObjectPtr<APawn> PawnAtBegin;
    TWeakObjectPtr<ADeliveryNpcCharacter> Npc;
    FGuid MessageId;
    EDeliveryConversationTurnPhase Phase = EDeliveryConversationTurnPhase::Recording;
    double StartedAt = 0;
    double Deadline = 0;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FDeliveryNpcMessagesInvalidated,const TArray<FDeliveryNpcMessageCancellation>&);

DECLARE_MULTICAST_DELEGATE_TwoParams(FDeliveryConversationLeaseExpired,
    const FDeliveryConversationTurnLease&, EDeliveryConversationLeaseEnd);

// 服务器按 World 保存每个 NPC 的输入与回复占用，外层负责距离校验和精确取消对白
// 所有接口在游戏线程调用，不保存音频，不直接调用 Agent、表现或 UI
UCLASS()
class DELIVERY_API UDeliveryConversationTurnSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickableWhenPaused() const override { return true; }

    FGuid GetWorldEpoch() const { return WorldEpoch; }

    // 同 client token 重发返回 AlreadyActive 且不重置时限，Handle.Token 由服务器签发
    // 替换项由外层按原编号取消
    EDeliveryConversationTurnResult TryBegin(APlayerController* Owner, ADeliveryNpcCharacter* Npc,
        FGuid Token, uint64 InputSequence, FDeliveryConversationTurnHandle& OutHandle,
        TArray<FDeliveryConversationTurnLease>& OutReplaced);
    EDeliveryConversationTurnResult TryBeginText(APlayerController* Owner, ADeliveryNpcCharacter* Npc,
        FGuid Token, uint64 InputSequence, FDeliveryConversationTurnHandle& OutHandle,
        TArray<FDeliveryConversationTurnLease>& OutReplaced);

    // 首次松键开始最终文字的五秒期限，重复松键不延长期限
    EDeliveryConversationTurnResult FinishRecording(APlayerController* Owner, const FDeliveryConversationTurnHandle& Handle);
    EDeliveryConversationTurnResult ValidateFinal(APlayerController* Owner, ADeliveryNpcCharacter* Npc,
        const FDeliveryConversationTurnHandle& Handle);
    // MessageId 必须等于服务器 Handle.Token，只提交一次
    // HearSpeech 拒绝后由外层 Cancel 撤销这一份占用
    bool CommitFinal(APlayerController* Owner, ADeliveryNpcCharacter* Npc,
        const FDeliveryConversationTurnHandle& Handle, FGuid MessageId);

    bool Cancel(APlayerController* Owner, const FDeliveryConversationTurnHandle& Handle,
        FDeliveryConversationTurnLease* OutRemoved = nullptr);
    void ReleasePlayer(APlayerController* Owner, TArray<FDeliveryConversationTurnLease>& OutRemoved);
    // 仅在对应回复真实播放结束或明确失败时释放，动作结束和 model_done 不能调用
    bool ReleaseReply(ADeliveryNpcCharacter* Npc, FGuid MessageId,
        FDeliveryConversationTurnLease* OutRemoved = nullptr);
    bool GetLease(ADeliveryNpcCharacter* Npc, FDeliveryConversationTurnLease& OutLease);

    // NPC 文本只在已接受回复之间交付，玩家输入占用仍沿原接口
    bool RegisterNpcReply(const FDeliveryNpcReplyRegistration& Reply);
    bool TryPrepareNpcMessage(ADeliveryNpcCharacter* Sender,FGuid RequestId,FGuid ReplyId,
        ADeliveryNpcCharacter* Receiver,const FString& Text,FGuid ReplyToEventId,FDeliveryNpcMessage& OutMessage);
    bool ConfirmNpcMessageDelivery(const FDeliveryNpcMessageHandle& Handle,ADeliveryNpcCharacter* Receiver,bool bAccepted);
    bool GetNpcMessage(const FDeliveryNpcMessageHandle& Handle,const ADeliveryNpcCharacter* Receiver,
        FDeliveryNpcMessage& OutMessage,bool bAllowPrepared=false) const;
    bool IsNpcMessageCurrent(const FDeliveryNpcMessageHandle& Handle,const ADeliveryNpcCharacter* Receiver) const;
    FDeliveryNpcMessageHandle GetNpcMessageHandle(FGuid MessageId,const ADeliveryNpcCharacter* Receiver) const;
    bool WasNpcMessageDelivered(FGuid ReplyId,const ADeliveryNpcCharacter* Receiver) const;
    bool CanStartNpcReplyPresentation(FGuid ReplyId) const;
    bool HasNpcReplyRecord(FGuid ReplyId) const { return NpcReplies.Contains(ReplyId); }
    bool IsNpcReplyValid(FGuid ReplyId) const;
    void ReportNpcReplyPresentation(FGuid ReplyId,EDeliveryNpcReplyPresentation State,
        TArray<FDeliveryNpcMessageCancellation>& OutCancelled);
    void InvalidateNpcBranchesFor(ADeliveryNpcCharacter* Npc,FName Reason,
        TArray<FDeliveryNpcMessageCancellation>& OutCancelled,bool bLeavingWorld=false);
    FDeliveryNpcMessagesInvalidated OnNpcMessagesInvalidated;

    // 只通知超时与对象失效，手动取消和替换通过返回的旧记录处理
    FDeliveryConversationLeaseExpired OnLeaseExpired;

    static constexpr double RecordingTimeoutSeconds = 15.0;
    static constexpr double FinalTextTimeoutSeconds = 5.0;
    // 故障兜底，正常回复依靠 ReleaseReply，不能把这个值作为预计播放时长
    static constexpr double ReplyFailureTimeoutSeconds = 300.0;

protected:
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
    friend class FDeliveryConversationLeaseWorldTest;
    friend class FDeliveryNpcMessageFlowTest;
    struct FOwnerHistory
    {
        uint64 HighestInputSequence = 0;
        TArray<FGuid> RecentTokens;
    };

    EDeliveryConversationTurnResult BeginInternal(APlayerController* Owner, ADeliveryNpcCharacter* Npc,
        FGuid Token, uint64 InputSequence, bool bText, FDeliveryConversationTurnHandle& OutHandle,
        TArray<FDeliveryConversationTurnLease>& OutReplaced);
    bool IsServerWorld() const;
    bool IsValidOwner(const APlayerController* Owner) const;
    FDeliveryConversationTurnLease* FindOwned(APlayerController* Owner, const FDeliveryConversationTurnHandle& Handle);
    void PruneInvalidLeases();

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
    bool IsNpcHere(const ADeliveryNpcCharacter* Npc) const;
    void AddNpcCancellation(FNpcReplyRecord& Record,FName Reason,TArray<FDeliveryNpcMessageCancellation>& OutCancelled);
    void InvalidateNpcMessage(FNpcReplyRecord& Record,FName Reason,TArray<FDeliveryNpcMessageCancellation>& OutCancelled);
    void InvalidateNpcDescendants(FName Reason,TArray<FDeliveryNpcMessageCancellation>& OutCancelled);
    void PruneNpcMessages();
    static constexpr int32 MaxNpcReplyRecords=128;
    static constexpr double MaxNpcAudioWaitSeconds=60;
    static constexpr double MaxNpcChainSeconds=120;
    static constexpr double NpcReceiptRetentionSeconds=300;
    TMap<FGuid,FNpcReplyRecord> NpcReplies;
    uint32 NextNpcMessageGeneration=0;

    FGuid WorldEpoch;
    TMap<TWeakObjectPtr<ADeliveryNpcCharacter>, FDeliveryConversationTurnLease> Leases;
    TMap<TWeakObjectPtr<APlayerController>, FOwnerHistory> Owners;
    double NextPruneAt = 0;
    bool bDeinitializing = false;
};
