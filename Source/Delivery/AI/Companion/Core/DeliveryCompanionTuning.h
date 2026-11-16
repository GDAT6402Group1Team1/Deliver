#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DeliveryCompanionTuning.generated.h"
class UGA_CompanionItemTransfer;
class UCurveVector;
class UAnimSequence;

/**
 * @brief 保存同伴跟随与初遇的配置，各角色的运行状态由 CompanionComponent 管理
 */
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryCompanionTuning : public UDataAsset
{
    GENERATED_BODY()
public:
    /// 限制查询附近物品的开销，实际可见范围同时受既有 Sight 配置限制
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Companion|Items",meta=(ClampMin="100",ClampMax="3000",Units="cm"))
    float ItemSightRadius=1200.f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Companion|Items",meta=(ClampMin="0.2",ClampMax="2",Units="s"))
    float ItemObservationInterval=.5f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Companion|Items",meta=(ClampMin="1",ClampMax="8"))
    int32 MaximumSeenItems=8;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Companion|Items")
    TSubclassOf<UGA_CompanionItemTransfer> ItemTransferAbility;
    /// 地面线索在停稳后使用全身查看动作，留空时沿用站姿
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Companion|Items|Search")
    TObjectPtr<UAnimSequence> LowItemInspectionAnimation;
    /// 可复用的拾取全身动画与作者剪辑中右手触物位置，厘米以角色网格为基准
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Companion|Items|Pickup")
    TObjectPtr<UAnimSequence> GroundPickupAnimation;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Companion|Items|Pickup",meta=(ClampMin="0.1",Units="s"))
    float GroundPickupContactTime=1.65f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Companion|Items|Pickup",meta=(Units="cm"))
    FVector GroundPickupPalmPosition=FVector(26.48,48.88,3.73);
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Companion|Items|Pickup",meta=(ClampMin="5",ClampMax="30",Units="cm"))
    float GroundPickupContactTolerance=16.f;

    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="初遇",meta=(ClampMin="200",Units="cm")) float EncounterRadius=900;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="初遇",meta=(ClampMin="0.3",ClampMax="3",Units="s")) float HelpHoldSeconds=1;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="初遇",meta=(ClampMin="0.3",ClampMax="5",Units="s")) float HelpRecoverySeconds=1.6f;
    /// 曲线为相对起落点连线的厘米偏移，X沿抛出方向，Y横向，Z向上，首尾归零
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="初遇|抛出") TObjectPtr<UCurveVector> EjectionCurve;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="初遇|抛出",meta=(ClampMin="0.3",ClampMax="5",Units="s")) float EjectionDuration=1.3f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="初遇|抛出",meta=(ClampMin="2",ClampMax="30",Units="s",
        DisplayName="玩家离场后保留初遇的时间")) float DoorWaitSeconds=10;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="初遇|抛出",meta=(ClampMin="10",ClampMax="100",Units="cm")) float EjectionArrivalTolerance=50;
    /// 保留原属性名与已保存数值，Follow 现在用它配置主人身后的距离
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跟随",meta=(DisplayName="跟随后距",ClampMin="140",ClampMax="400",Units="cm",ToolTip="行走跟随时与主人保持的后方距离")) float LeadDistance=190;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跟随",meta=(ClampMin="80",ClampMax="250",Units="cm")) float SideDistance=95;
    /// 亲密度从 50 降到 20 时逐步增加这段后距，回暖时逐步收回
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跟随",meta=(DisplayName="受委屈额外后距",ClampMin="0",ClampMax="150",Units="cm")) float WithdrawnFollowExtraDistance=70;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跟随",meta=(ClampMin="50",ClampMax="200",Units="cm")) float RepathDistance=65;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跟随",meta=(ClampMin="100",ClampMax="300",Units="cm")) float YieldDistance=150;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跟随",meta=(ClampMin="100",ClampMax="400",Units="cm")) float DestinationClearance=220;
    /// 等候期间只能在原位置附近让路，不能借避让追到远处主人身边
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="等候",meta=(ClampMin="200",ClampMax="600",Units="cm")) float WaitYieldRadius=400;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跟随",meta=(ClampMin="0.2",ClampMax="2",Units="s")) float NavigationInterval=.35f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跟随",meta=(ClampMin="1",ClampMax="15",Units="s")) float BlockedRetrySeconds=3;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="补位",meta=(ClampMin="1500",ClampMax="10000",Units="cm")) float CatchUpDistance=2500;
    /// 包围盒加余量后再检查屏幕外，查询期间停止自身移动
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="补位",meta=(ClampMin="50",ClampMax="300",Units="cm")) float CatchUpVisibilityMargin=100;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="补位",meta=(ClampMin="1",ClampMax="10",Units="s")) float CatchUpRetrySeconds=3;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="关系",meta=(ClampMin="0",ClampMax="100")) int32 InitialAffinity=70;
    /// 主人要求少说话后的主动闲聊暂停时间，主动提问仍能得到回复
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="对话",meta=(ClampMin="15",ClampMax="180",Units="s")) float QuietSpeechSeconds=45.f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="关系",meta=(ClampMin="0",ClampMax="20")) int32 DeliveryTogetherAffinity=3;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="关系",meta=(ClampMin="0",ClampMax="20")) int32 CompanionDeliveryAffinity=5;
    /// 主人扶起一次战斗倒地并实际完成站立后增加，只结算一次
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="关系",meta=(ClampMin="0",ClampMax="20")) int32 HelpUpAffinity=5;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="关系",meta=(ClampMin="0",ClampMax="30")) int32 OwnerHitAffinityLoss=8;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="关系",meta=(ClampMin="0",ClampMax="30")) int32 OwnerDownHitAffinityLoss=12;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="关系",meta=(ClampMin="0",ClampMax="30")) int32 OwnerVehicleHitAffinityLoss=10;
    /// 附近真实命中可提出协战，执行时检查主人、二狗与目标的当前距离
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="共同战斗",meta=(ClampMin="200",ClampMax="1200",Units="cm")) float AssistRange=900;
    /// 只限制尚未确认的命中候选，已确认协战保留到主人收手
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="共同战斗",meta=(ClampMin="5",ClampMax="60",Units="s")) float AssistLifetimeSeconds=30;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="共同战斗",meta=(ClampMin="1.2",ClampMax="5",Units="s")) float AssistActionSeconds=5;
    /// 只累计实际执行时间，倾听、倒地、等主人下车或腾手期间暂停计时
    /// 正常行走时间由各阶段首次完整路径补足，此值给受阻与交互留出额外活动时间
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跑腿",meta=(ClampMin="10",ClampMax="600",Units="s")) float ErrandTimeoutSeconds=120;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="跑腿",meta=(ClampMin="1",ClampMax="30",Units="s")) float ErrandInteractionTimeoutSeconds=6;
};
