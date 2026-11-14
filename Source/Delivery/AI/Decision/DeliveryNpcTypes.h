#pragma once

#include "CoreMinimal.h"
#include "AI/Conversation/DeliveryNpcMessageTypes.h"
#include "AI/Companion/DeliveryCompanionHomeTypes.h"
#include "DeliveryNpcTypes.generated.h"
class APlayerState;
class APlayerController;
class UDeliveryTaskDefinition;
class ADeliveryPackageItem;

// 只允许模型选择游戏已经实现的动作
UENUM(BlueprintType)
enum class EDeliveryNpcAction : uint8 { None, Wait, LookAt, MoveTo, StrikeOnce, HoldVehicle, ReleaseVehicle, LieDown, GetUp, Pace, MoveAside, Fight, Flee, Follow, Stay, CancelCompanionTask, FetchPackage, TakePackage, DeliverPackage, ReturnPackage, OfferCaterpillar, GoHome, HomeAction, ContinueCurrent };

UENUM(BlueprintType)
enum class EDeliveryNpcActionStatus : uint8 { Idle, Accepted, Running, Succeeded, Failed, Cancelled };

// 模型建议与服务器规则共用执行器，来源只由服务器写入
UENUM(BlueprintType)
enum class EDeliveryNpcActionOrigin : uint8 { Model, GameplayRule };

UENUM(BlueprintType)
enum class EDeliveryNpcEvidenceSource : uint8 { Seen, Heard, ToldBy, ActionResult };

// 记录 NPC 实际看到、听到或被告知的事件，未感知的信息不加入记录
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcObservation
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite) FGuid EventId;
    // 记录这句话回答了哪次玩家发言，界面据此区分回复与 NPC 闲聊
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FGuid ReplyToEventId;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName Kind;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) EDeliveryNpcEvidenceSource Source = EDeliveryNpcEvidenceSource::Seen;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TObjectPtr<AActor> Subject = nullptr;
    // 交谈时记录这句话是对谁说的，旁人听到后也能分清谈话对象
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TObjectPtr<AActor> Addressee = nullptr;
    // 记录看到或听到事件时的位置，传闻中没有位置时留空
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bHasObservedLocation = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector ObservedLocation = FVector::ZeroVector;
    // 将玩家原话保存为对话内容，动作由模型从游戏提供的选项中选择
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Description;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0.1", ClampMax="60", Units="s"))
    float Lifetime = 10.f;
    // 只记住的 NPC 发言不作废当前回复；仅供 C++ 使用
    bool bNeedsReply = true;
};

// 将动作状态同步给玩家，NPC 的个人经历和发给模型的信息保存在服务器
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcPublicState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) FGuid ActionId;
    // 同一段交战可收到多次回复，换一句台词不会更换正在出拳的动作编号
    UPROPERTY(BlueprintReadOnly) FGuid ReplyId;
    FGuid GetReplyId() const { return ReplyId.IsValid()?ReplyId:ActionId; }
    UPROPERTY(BlueprintReadOnly) int32 PlanRevision = 0;
    UPROPERTY(BlueprintReadOnly) EDeliveryNpcAction Action = EDeliveryNpcAction::None;
    UPROPERTY(BlueprintReadOnly) EDeliveryNpcActionOrigin Origin = EDeliveryNpcActionOrigin::Model;
    // 服务器规则的当前代次，随动作复制供表现识别，模型协议不能写入
    UPROPERTY(BlueprintReadOnly) int32 RuleRevision = 0;
    UPROPERTY(BlueprintReadOnly) EDeliveryNpcActionStatus Status = EDeliveryNpcActionStatus::Idle;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<AActor> Target = nullptr;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<AActor> SpeechTarget = nullptr;
    UPROPERTY(BlueprintReadOnly) double ServerStartTime = 0;
    UPROPERTY(BlueprintReadOnly) float Duration = 0;
    UPROPERTY(BlueprintReadOnly) FName Reason;
    UPROPERTY(BlueprintReadOnly) FString Speech;
    UPROPERTY(BlueprintReadOnly) FGuid ReplyToEventId;
    UPROPERTY(BlueprintReadOnly) FName Emotion;
    UPROPERTY(BlueprintReadOnly) bool bStreamSpeech = false;
    // 仅服务器持有，动作权限不取决于台词面向谁
    UPROPERTY(Transient,NotReplicated) TObjectPtr<APlayerState> Requester = nullptr;
    int32 CompanionCommandRevision = 0;
    // 由服务器复制本次请求快照，模型不能指定或替换实物和订单
    UPROPERTY(Transient,NotReplicated) TObjectPtr<UDeliveryTaskDefinition> CompanionTask = nullptr;
    UPROPERTY(Transient,NotReplicated) TObjectPtr<ADeliveryPackageItem> CompanionPackage = nullptr;
    // 仅服务器保存选中的实物与槽位快照，不复制 Smart Objects 句柄
    TSharedPtr<FDeliveryCompanionHomeChoice> HomeChoice;
};

// 发送给后端的数据只保存编号和描述，不保存 UE 对象引用
struct DELIVERY_API FDeliveryNpcEvidence
{
    FGuid EventId;
    FGuid SubjectId;
    FGuid AddresseeId;
    FGuid ReplyToEventId;
    FName Kind;
    EDeliveryNpcEvidenceSource Source = EDeliveryNpcEvidenceSource::Seen;
    FString Description;
    bool bHasObservedLocation = false;
    FVector ObservedLocation = FVector::ZeroVector;
    double ObservedAt = 0;
    double ExpiresAt = 0;
    // 只记住的 NPC 发言不作废当前回复，不写入模型请求的 JSON
    bool bNeedsReply = true;
};

struct FDeliveryNpcCandidate
{
    FGuid CandidateId;
    EDeliveryNpcAction Action=EDeliveryNpcAction::Wait;
    FGuid TargetId;
    TArray<FGuid> EvidenceIds;
    TSharedPtr<FDeliveryCompanionHomeChoice> HomeChoice;
};

// 只介绍 NPC 已经遇见的人物，不包含其他人的私有记忆
struct FDeliveryNpcSubject
{
    FGuid SubjectId;
    FString Name;
    FString Kind;
};

// 当局往事只提供对话背景，不进入当下行动证据或延长旧命中的执行权限
struct DELIVERY_API FDeliveryNpcCompanionMemory
{
    FGuid EventId;
    FName Kind;
    FString Description;
};

// 本次请求刚观察到的公开地点，编号只供对话指代，不提供行动权限
struct DELIVERY_API FDeliveryNpcObservedPlace
{
    FString PlaceId;
    FString Name;
    FString Description;
    float DistanceCm=0;
};

struct DELIVERY_API FDeliveryNpcDecisionRequest
{
    // 仅服务器关联原消息，不序列化给模型，也不继承发送者的玩家权限
    FDeliveryNpcMessageHandle ParentNpcMessage;
    // 只在服务器核对：回复必须仍属于发请求时的那段行动
    FGuid RunningActionId;
    static constexpr int32 ProtocolVersion = 1;
    static constexpr int32 CapabilityVersion = 6;
    // 普通居民继续使用版本4，同伴有家居上下文时显式请求版本5
    int32 RequestedCapabilityVersion = 4;
    // 发言归属和活动快照只由服务器填写，不相信模型选择的说话对象
    TWeakObjectPtr<APlayerController> RequesterPC;
    TWeakObjectPtr<APlayerState> RequesterPlayerState;
    FGuid PlayerMessageId;
    int32 RunningPlanRevision=0;
    TWeakObjectPtr<AActor> RunningTarget;
    TWeakObjectPtr<APlayerState> RunningRequester;
    EDeliveryNpcActionOrigin RunningOrigin=EDeliveryNpcActionOrigin::Model;
    double RunningStartTime=0;
    float RunningDuration=0;
    double RunningDeadline=0;
    bool bRunningCompanion=false;
    bool bRunningErrand=false;
    bool bRunningHome=false;
    uint8 RunningCompanionMode=0;
    TWeakObjectPtr<APlayerState> RunningCompanionOwner;
    FGuid RunningHomeArrangement;
    TWeakObjectPtr<AActor> RunningHomeProp;
    FGuid WorldEpoch;
    FGuid NpcId;
    FGuid RequestId;
    int32 ContextRevision = 0;
    int32 PlanRevision = 0;
    FString Persona;
    // 使用人物表行名选择该 NPC 的背景资料，未选择人物行时留空
    FString ProfileId;
    FString SelfName;
    FGuid SelfSubjectId;
    TArray<FDeliveryNpcSubject> Subjects;
    TArray<FDeliveryNpcEvidence> Evidence;
    // 上次已采纳的决策读过哪些事件，用于区分随后听到的新话
    TArray<FGuid> LastDecisionEventIds;
    // 本次接话来自哪一轮，在 UE 内保存，不发送给模型
    int32 ConversationDepth = 0;
    TArray<EDeliveryNpcAction> AllowedActions;
    TArray<FDeliveryNpcCandidate> Candidates;
    bool bPlayerRequest = false;
    // 只有游戏与后端都启用时，语音才会在一句话生成期间分段返回
    bool bStreamSentenceAudio = false;
    // 版本 2 将模型完成与声音合成完成分别通知，未设置时沿用旧协议
    int32 StreamProtocolVersion = 0;
    bool bPendingPlayerMessage = false;
    EDeliveryNpcAction RunningAction=EDeliveryNpcAction::Wait;
    FGuid RunningTargetId;
    double RunningSecondsRemaining=0;
    FString Voice = TEXT("male");
    // 同伴持续安排与临时对白分开，字段只来自服务器
    bool bHasCompanionContext = false;
    FString CompanionOwnerName;
    FString CompanionRelationshipStage;
    bool bCanOfferCaterpillar=false;
    TArray<FDeliveryNpcCompanionMemory> CompanionMemories;
    TArray<FDeliveryNpcHomeObjectView> HomeObjects;
    TArray<FDeliveryNpcObservedPlace> CompanionObservedPlaces;
    bool bRequesterIsCompanionOwner = false;
    FString CompanionMode;
    FString CompanionSuspensionReason;
    int32 CompanionCommandRevision = 0;
    FString CompanionTaskId;
    FString CompanionTaskPhase;
    FString CompanionTaskTitle;
    bool bCompanionOwnerHasPackage = false;
    bool bCompanionHasPackage = false;
    FString CompanionErrandAction;
    FString CompanionErrandPhase;
    FString CompanionErrandStatus;
    FString CompanionErrandReason;
    // 只用于服务器采纳/执行前复核，不发送 UObject 地址给模型
    TWeakObjectPtr<UDeliveryTaskDefinition> CompanionTask;
    TWeakObjectPtr<ADeliveryPackageItem> CompanionPackage;
};

// 一句已经生成的台词及其语音，序号保证不同合成速度不会打乱播放顺序
struct FDeliveryNpcSentence
{
    int32 Sequence = 0;
    FString Text;
    FName Emotion;
    TArray<uint8> Pcm;
    int32 SampleRate = 16000;
};

// 同一句台词只发送一次文字，后续音频块沿用这个序号
struct FDeliveryNpcSpeechBegin
{
    int32 Sequence = 0;
    FString Text;
    FName Emotion;
    int32 SampleRate = 24000;
    int32 AudioStreamVersion = 1;
};

struct FDeliveryNpcAudioChunk
{
    int32 Sequence = 0;
    int32 ChunkIndex = 0;
    int32 OffsetBytes = 0;
    TArray<uint8> Pcm;
};

// 合成失败也结束当前句子，已有文字由表现层保留
struct FDeliveryNpcAudioEnd
{
    int32 Sequence = 0;
    int32 ChunkCount = 0;
    int32 TotalPcmBytes = 0;
    bool bSucceeded = false;
    FString Error;
};

struct DELIVERY_API FDeliveryNpcDecisionProposal
{
    int32 CapabilityVersion = 4;
    FGuid CandidateId;
    FGuid WorldEpoch;
    FGuid NpcId;
    FGuid RequestId;
    int32 ContextRevision = 0;
    int32 PlanRevision = 0;
    EDeliveryNpcAction Action = EDeliveryNpcAction::None;
    FGuid TargetId;
    FGuid SpeechTargetId;
    bool bHasSpeechTarget = false;
    float Duration = 0;
    FString Speech;
    FName Emotion;
    TArray<FGuid> EvidenceIds;
};

namespace DeliveryNpc
{
    constexpr float FightDecisionSeconds=8.f;
    constexpr float OtherActionSeconds=5.f;
    constexpr float MaxContinuousCombatSeconds=20.f;
    constexpr float CombatDecisionLeadSeconds=4.f;
    inline float MaxDecisionDuration(EDeliveryNpcAction Action)
    { return Action==EDeliveryNpcAction::Fight?FightDecisionSeconds:OtherActionSeconds; }
    DELIVERY_API bool IsTerminal(EDeliveryNpcActionStatus Status);
    DELIVERY_API FString ActionName(EDeliveryNpcAction Action);
    DELIVERY_API bool IsCompanionCommand(EDeliveryNpcAction Action);
    DELIVERY_API bool IsCompanionErrand(EDeliveryNpcAction Action);
    // 候选允许的最长时长，持续同伴安排返回0
    DELIVERY_API float CandidateDuration(EDeliveryNpcAction Action);
    DELIVERY_API FString EncodeRequest(const FDeliveryNpcDecisionRequest& Request);
    // 解析成功后写入 OutProposal，失败原因写入 Reason
    DELIVERY_API bool DecodeProposal(const FString& Json, FDeliveryNpcDecisionProposal& OutProposal, FName& Reason);
}
