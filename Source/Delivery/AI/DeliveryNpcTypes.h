#pragma once

#include "CoreMinimal.h"
#include "DeliveryNpcTypes.generated.h"

// 只有已提供行为树执行器的能力才进入模型白名单
UENUM(BlueprintType)
enum class EDeliveryNpcAction : uint8 { None, Wait, LookAt, MoveTo, StrikeOnce, HoldVehicle, ReleaseVehicle, LieDown, GetUp };

UENUM(BlueprintType)
enum class EDeliveryNpcActionStatus : uint8 { Idle, Accepted, Running, Succeeded, Failed, Cancelled };

UENUM(BlueprintType)
enum class EDeliveryNpcEvidenceSource : uint8 { Seen, Heard, ToldBy, ActionResult };

// 由服务器感知入口提供事实，Agent 不扫描世界来补齐未知信息
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcObservation
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite) FGuid EventId;
    // 完整对白明确回答了哪条玩家发言，不能靠播放结束时间推断
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FGuid ReplyToEventId;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName Kind;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) EDeliveryNpcEvidenceSource Source = EDeliveryNpcEvidenceSource::Seen;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TObjectPtr<AActor> Subject = nullptr;
    // 位置由感知入口明确提供，传闻只有对象身份时不能读取对象的实时位置
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bHasObservedLocation = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector ObservedLocation = FVector::ZeroVector;
    // 玩家原话只能作为证据内容，不能变成动作名称或执行脚本
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Description;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0.1", ClampMax="60", Units="s"))
    float Lifetime = 10.f;
};

// 只复制玩家能观察到的状态，证据和请求上下文留在服务器
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcPublicState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) FGuid ActionId;
    UPROPERTY(BlueprintReadOnly) int32 PlanRevision = 0;
    UPROPERTY(BlueprintReadOnly) EDeliveryNpcAction Action = EDeliveryNpcAction::None;
    UPROPERTY(BlueprintReadOnly) EDeliveryNpcActionStatus Status = EDeliveryNpcActionStatus::Idle;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<AActor> Target = nullptr;
    UPROPERTY(BlueprintReadOnly) double ServerStartTime = 0;
    UPROPERTY(BlueprintReadOnly) float Duration = 0;
    UPROPERTY(BlueprintReadOnly) FName Reason;
    UPROPERTY(BlueprintReadOnly) FString Speech;
    UPROPERTY(BlueprintReadOnly) FGuid ReplyToEventId;
    UPROPERTY(BlueprintReadOnly) FName Emotion;
};

// 传输对象不持有 UObject，网关可在异步完成后安全丢弃整个请求
struct DELIVERY_API FDeliveryNpcEvidence
{
    FGuid EventId;
    FGuid SubjectId;
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
    TArray<FDeliveryNpcEvidence> Evidence;
    TArray<EDeliveryNpcAction> AllowedActions;
    TArray<FDeliveryNpcCandidate> Candidates;
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
    // 失败时不改写 OutProposal，Reason 是稳定的诊断码
    DELIVERY_API bool DecodeProposal(const FString& Json, FDeliveryNpcDecisionProposal& OutProposal, FName& Reason);
}
