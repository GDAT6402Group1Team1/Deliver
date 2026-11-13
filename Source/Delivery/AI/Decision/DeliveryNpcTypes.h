#pragma once

#include "CoreMinimal.h"
#include "DeliveryNpcTypes.generated.h"

// 只允许模型选择游戏已经实现的动作
UENUM(BlueprintType)
enum class EDeliveryNpcAction : uint8 { None, Wait, LookAt, MoveTo, StrikeOnce, HoldVehicle, ReleaseVehicle, LieDown, GetUp, Pace, MoveAside, Fight };

UENUM(BlueprintType)
enum class EDeliveryNpcActionStatus : uint8 { Idle, Accepted, Running, Succeeded, Failed, Cancelled };

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
};

// 将动作状态同步给玩家，NPC 的个人经历和发给模型的信息保存在服务器
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcPublicState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) FGuid ActionId;
    UPROPERTY(BlueprintReadOnly) int32 PlanRevision = 0;
    UPROPERTY(BlueprintReadOnly) EDeliveryNpcAction Action = EDeliveryNpcAction::None;
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
};

struct FDeliveryNpcCandidate
{
    EDeliveryNpcAction Action=EDeliveryNpcAction::Wait;
    FGuid TargetId;
    TArray<FGuid> EvidenceIds;
};

// 只介绍 NPC 已经遇见的人物，不包含其他人的私有记忆
struct FDeliveryNpcSubject
{
    FGuid SubjectId;
    FString Name;
    FString Kind;
};

struct DELIVERY_API FDeliveryNpcDecisionRequest
{
    static constexpr int32 ProtocolVersion = 1;
    static constexpr int32 CapabilityVersion = 1;
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
    TArray<EDeliveryNpcAction> AllowedActions;
    TArray<FDeliveryNpcCandidate> Candidates;
    bool bPlayerRequest = false;
    // 只有游戏与后端都启用时，语音才会在一句话生成期间分段返回
    bool bStreamSentenceAudio = false;
    FString Voice = TEXT("male");
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
    DELIVERY_API bool IsTerminal(EDeliveryNpcActionStatus Status);
    DELIVERY_API FString ActionName(EDeliveryNpcAction Action);
    DELIVERY_API FString EncodeRequest(const FDeliveryNpcDecisionRequest& Request);
    // 解析成功后写入 OutProposal，失败原因写入 Reason
    DELIVERY_API bool DecodeProposal(const FString& Json, FDeliveryNpcDecisionProposal& OutProposal, FName& Reason);
}
