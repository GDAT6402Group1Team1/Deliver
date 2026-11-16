#pragma once

#include "CoreMinimal.h"
#include "World/Home/DeliveryHomeObjectComponent.h"
#include "DeliveryCompanionHomeTypes.generated.h"

class APlayerState;
class APawn;
class UAnimSequence;
class UPrimitiveComponent;

// 物件清单只包含实际感知和已实现的用途，不向模型发送 UObject 或 SO 句柄
struct FDeliveryNpcHomeSlotView
{
    FString SlotId;
    FString Name;
    bool bAvailable=false;
};

struct FDeliveryNpcHomeObjectView
{
    FGuid ObjectId;
    FString Name;
    FString State;
    TArray<FString> Uses;
    TArray<FDeliveryNpcHomeSlotView> Slots;
};

// 请求快照绑定实物和用途，接受、开始和实际交接都重新检查弱身份与版本
struct FDeliveryCompanionHomeChoice
{
    FGuid CandidateId;
    FGuid RequestId;
    TWeakObjectPtr<APlayerState> Requester;
    TWeakObjectPtr<UDeliveryHomeObjectComponent> SourceObject;
    TWeakObjectPtr<UDeliveryHomeObjectComponent> DestinationObject;
    FGuid SourceId;
    FGuid DestinationId;
    int32 SourceRevision=0;
    int32 DestinationRevision=0;
    FName DestinationSlotId;
    EDeliveryHomeOperation Operation=EDeliveryHomeOperation::None;
    FDeliveryHomeUseCandidate Use;
    FDeliveryHomeUseCandidate TakeUse;
    FGuid SeatArrangementId;
    TWeakObjectPtr<APawn> PlacementOwnerPawn;
};

UENUM(BlueprintType)
enum class EDeliveryCompanionHomePhase : uint8
{
    None, GoToObject, TakeProp, GoToDestination, EnterUse, Using, PlaceProp, LeaveUse, Completed, Failed, Cancelled
};

// 服务器给出本次座位移动，模拟代理创建同参数 RMS，由 CMC 负责位移和碰撞
USTRUCT()
struct FDeliveryCompanionSeatMotion
{
    GENERATED_BODY()
    UPROPERTY() FGuid ArrangementId;
    UPROPERTY() int32 Revision=0;
    UPROPERTY() TObjectPtr<AActor> Seat;
    UPROPERTY() FVector Start=FVector::ZeroVector;
    UPROPERTY() FVector Target=FVector::ZeroVector;
    UPROPERTY() double StartedServerTime=0;
    UPROPERTY() float Duration=0;
    UPROPERTY() bool bActive=false;
    UPROPERTY() bool bMoving=false;
};

// 仅保存这个组件实际接管的资源，复制状态与本机 RMS 编号分开
struct FDeliveryCompanionSeatRuntime
{
    FGuid ArrangementId;
    TWeakObjectPtr<AActor> Seat;
    TWeakObjectPtr<UPrimitiveComponent> Capsule;
    FName MotionName;
    uint16 MotionId=0;
    int32 AppliedRevision=0;
    int32 FinishedRevision=0;
    uint8 PreviousMovementMode=0;
    uint8 PreviousCustomMode=0;
    bool bAddedMoveIgnore=false;
    bool bSavedMovement=false;
    bool bSavedOrientToMovement=false;
    bool bSavedControllerRotation=false;
};

// 所有人看到同一动作与实物结果，槽位是否可用仍由服务器 Smart Objects 判断
USTRUCT(BlueprintType)
struct FDeliveryCompanionHomeState
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FGuid ArrangementId;
    UPROPERTY(BlueprintReadOnly) EDeliveryCompanionHomePhase Phase=EDeliveryCompanionHomePhase::None;
    UPROPERTY(BlueprintReadOnly) EDeliveryHomeOperation Operation=EDeliveryHomeOperation::None;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<AActor> Object;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<AActor> Destination;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<AActor> CarriedProp;
    UPROPERTY(BlueprintReadOnly) FName Reason;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<UAnimSequence> PoseAnimation;
    UPROPERTY(BlueprintReadOnly) double PoseStartedServerTime=0;
    UPROPERTY(BlueprintReadOnly) bool bLoopPose=false;
    UPROPERTY(BlueprintReadOnly) bool bUpperBodyPose=false;
    UPROPERTY() FDeliveryCompanionSeatMotion SeatMotion;
};
