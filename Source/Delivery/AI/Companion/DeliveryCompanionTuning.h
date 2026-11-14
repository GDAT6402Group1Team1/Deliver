#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DeliveryCompanionTuning.generated.h"
class UCurveVector;
class ADeliveryCaterpillarItem;

// 保存同伴跟随与初遇的配置，各角色的运行状态由 CompanionComponent 管理
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryCompanionTuning : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="初遇",meta=(ClampMin="200",Units="cm")) float EncounterRadius=900;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="初遇",meta=(ClampMin="0.3",ClampMax="3",Units="s")) float HelpHoldSeconds=1;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="初遇",meta=(ClampMin="0.3",ClampMax="5",Units="s")) float HelpRecoverySeconds=1.6f;
    // 曲线为相对起落点连线的厘米偏移，X沿抛出方向，Y横向，Z向上，首尾归零
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="初遇|抛出") TObjectPtr<UCurveVector> EjectionCurve;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="初遇|抛出",meta=(ClampMin="0.3",ClampMax="5",Units="s")) float EjectionDuration=1.3f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="初遇|抛出",meta=(ClampMin="2",ClampMax="30",Units="s")) float DoorWaitSeconds=10;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="初遇|抛出",meta=(ClampMin="10",ClampMax="100",Units="cm")) float EjectionArrivalTolerance=50;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跟随",meta=(ClampMin="200",ClampMax="400",Units="cm")) float LeadDistance=280;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跟随",meta=(ClampMin="80",ClampMax="250",Units="cm")) float SideDistance=150;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跟随",meta=(ClampMin="50",ClampMax="200",Units="cm")) float RepathDistance=100;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跟随",meta=(ClampMin="100",ClampMax="300",Units="cm")) float YieldDistance=150;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跟随",meta=(ClampMin="100",ClampMax="400",Units="cm")) float DestinationClearance=220;
    // 等候期间只能在原位置附近让路，不能借避让追到远处主人身边
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="等候",meta=(ClampMin="200",ClampMax="600",Units="cm")) float WaitYieldRadius=400;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跟随",meta=(ClampMin="0.2",ClampMax="2",Units="s")) float NavigationInterval=.5f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跟随",meta=(ClampMin="1",ClampMax="15",Units="s")) float BlockedRetrySeconds=3;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="补位",meta=(ClampMin="1500",ClampMax="10000",Units="cm")) float CatchUpDistance=2500;
    // 包围盒加余量后再检查屏幕外，查询期间停止自身移动
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="补位",meta=(ClampMin="50",ClampMax="300",Units="cm")) float CatchUpVisibilityMargin=100;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="补位",meta=(ClampMin="1",ClampMax="10",Units="s")) float CatchUpRetrySeconds=3;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="关系",meta=(ClampMin="0",ClampMax="100")) int32 InitialAffinity=70;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="关系",meta=(ClampMin="0",ClampMax="20")) int32 DeliveryTogetherAffinity=3;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="关系",meta=(ClampMin="0",ClampMax="20")) int32 CompanionDeliveryAffinity=5;
    // 主人扶起一次战斗倒地并实际完成站立后增加，只结算一次
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="关系",meta=(ClampMin="0",ClampMax="20")) int32 HelpUpAffinity=5;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="关系",meta=(ClampMin="0",ClampMax="30")) int32 OwnerHitAffinityLoss=8;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="关系",meta=(ClampMin="0",ClampMax="30")) int32 OwnerDownHitAffinityLoss=12;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="关系",meta=(ClampMin="0",ClampMax="30")) int32 OwnerVehicleHitAffinityLoss=10;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="小绿") TSoftClassPtr<ADeliveryCaterpillarItem> CaterpillarItemClass=
        TSoftClassPtr<ADeliveryCaterpillarItem>(FSoftObjectPath(TEXT("/Game/Blueprint/Item/BP_Caterpillar.BP_Caterpillar_C")));
    // 只响应附近真实命中，目标与二狗都不能离开本次护主发生的范围
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="共同战斗",meta=(ClampMin="200",ClampMax="1200",Units="cm")) float AssistRange=900;
    // 倒地或倾听期间保留目标，期限到达后需要新的真实命中才能再开始
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="共同战斗",meta=(ClampMin="5",ClampMax="60",Units="s")) float AssistLifetimeSeconds=30;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="共同战斗",meta=(ClampMin="1.2",ClampMax="5",Units="s")) float AssistActionSeconds=5;
    // 只累计实际执行时间，倾听、倒地、等主人下车或腾手期间暂停计时
    // 正常行走时间由各阶段首次完整路径补足，此值给受阻与交互留出额外活动时间
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跑腿",meta=(ClampMin="10",ClampMax="600",Units="s")) float ErrandTimeoutSeconds=120;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跑腿",meta=(ClampMin="1",ClampMax="30",Units="s")) float ErrandInteractionTimeoutSeconds=6;
};
