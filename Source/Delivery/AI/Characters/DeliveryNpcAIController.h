#pragma once
#include "CoreMinimal.h"
#include "AIController.h"
#include "Perception/AIPerceptionTypes.h"
#include "DeliveryNpcAIController.generated.h"
class UAISenseConfig_Sight;
class UAISenseConfig_Hearing;
class ADeliveryNpcCharacter;
class UPathFollowingComponent;
class UPrimitiveComponent;
struct FNavigationPath;
struct FNavLinkId;

/**
 * @brief NPC 的 AI 控制器：运行行为树，把看见听见的事告诉 Agent，顺便管站位和跳过矮障碍
 *
 * 感知：UE 视听感知触发 OnTargetSeenOrHeard，把看见、看不见写成 Agent 事件
 * 行为树：角色每帧调 RefreshBlackboard 更新黑板，行为树按黑板切分支，再调角色的 ExecuteAction
 * 站位：空闲时 UpdateStandingSpace 发现离别人太近就挪开一点
 * 越障：沿导航走时 UpdateObstacleTraversal 发现前面有矮障碍就跳过去
 */
UCLASS()
class DELIVERY_API ADeliveryNpcAIController : public AAIController
{
    GENERATED_BODY()
public:
    ADeliveryNpcAIController();

    // ── 行为树 ──
    // 把 Agent 当前的动作和倾听状态写进黑板，行为树据此选分支；Tick 里更新站位和越障

    /// @brief 由角色每帧调用
    void RefreshBlackboard();
    virtual void Tick(float DeltaSeconds) override;

    // ── 越障 ──

    /// @brief 这次移动是不是正在跳障碍，移动任务据此不把跳跃当成走偏
    bool IsTraversingObstacle(FAIRequestID Request) const;

    // ── 站位 ──

    /// 空闲时离别人太近就挪开
    UPROPERTY(EditAnywhere,Category="Delivery|站位") bool bKeepDistanceWhenIdle=true;

    // 身体边缘靠近到这个距离才挪步，胶囊半径另外计算
    UPROPERTY(EditAnywhere,Category="Delivery|站位",meta=(ClampMin="20",ClampMax="150",Units="cm")) float MinStandingGap=80;

    // 挪开后留出更宽间隔，避免刚停下又开始移动
    UPROPERTY(EditAnywhere,Category="Delivery|站位",meta=(ClampMin="40",ClampMax="200",Units="cm")) float ComfortableStandingGap=120;

    // ── 越障 ──
    // 导航路线上遇到矮障碍时跳过去，跳起和落地由角色移动组件处理

    /// 是否自动跳过矮障碍
    UPROPERTY(EditAnywhere,Category="Delivery|越障") bool bAutoJumpLowObstacles=true;

    /// 往前探多远找障碍
    UPROPERTY(EditAnywhere,Category="Delivery|越障",meta=(ClampMin="60",ClampMax="180",Units="cm")) float ObstacleJumpProbeDistance=130.f;

    /// 落点最多比脚下高多少
    UPROPERTY(EditAnywhere,Category="Delivery|越障",meta=(ClampMin="25",ClampMax="100",Units="cm")) float ObstacleJumpMaxLandingRise=80.f;

    /// 跳失败后隔多久再试
    UPROPERTY(EditAnywhere,Category="Delivery|越障",meta=(ClampMin="0.2",ClampMax="3",Units="s")) float ObstacleJumpRetrySeconds=.85f;
protected:
    // ── 生命周期 ──
    virtual void OnPossess(APawn* Pawn) override;
    virtual void OnUnPossess() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    friend class FDeliveryNpcObstacleTraversalTest;
    friend class FDeliveryCompanionRulesTest;

    // ── 站位 ──
    // 每隔一会儿查身边最近的人，太近就找空地挪过去，挪完留出更宽的间隔

    /// 查身边最近的人，太近就挪开
    void UpdateStandingSpace();

    /// 停下挪位
    void StopSpacingMove();

    /// 目标点站得下 NPC 的胶囊
    bool HasRoomToStand(const ADeliveryNpcCharacter* Npc,const FVector& Destination) const;

    // ── 越障 ──
    // 前方探到障碍 → 确认跳得过去、落点不太高、抛物线不撞东西 → 起跳；被挡住时先退一步再试，实现在 DeliveryNpcObstacleTraversal.cpp

    /// 每帧看导航前方有没有要跳的障碍
    void UpdateObstacleTraversal();

    /// 条件都满足就起跳
    bool TryStartObstacleTraversal(ADeliveryNpcCharacter& Npc,UPathFollowingComponent& Path,double Now);

    /// 能不能跳：在地上、没倒地、正在沿导航走
    bool CanTraverseObstacle(const ADeliveryNpcCharacter& Npc) const;

    /// 跳跃的抛物线一路不撞东西
    bool HasClearObstacleJumpArc(const ADeliveryNpcCharacter& Npc,const FVector& LaunchVelocity,double FlightTime) const;

    /// 把落点沿路线往前延，越过障碍；遇到自定义导航连接时停在它前面
    static FVector ExtendObstacleRouteTarget(const FNavigationPath& Route,uint32 NextPathIndex,
        const FVector& Feet,const FVector& Direction,const FVector& CurrentTarget,const FNavLinkId& WalkThroughLink,bool& bStopsBeforeCustomLink);

    /// 这次移动是不是已经为这个障碍后退过
    bool HasTriedObstacleRetreat(FAIRequestID Request,const UPrimitiveComponent& Blocker,const FVector& Feet) const;

    /// 结束跳跃
    void StopObstacleTraversal();

    /// 正在进行的一次跳跃
    struct FObstacleTraversal
    {
        TWeakObjectPtr<ADeliveryNpcCharacter> Pawn;
        FAIRequestID Request=FAIRequestID::InvalidRequest;
        FVector LaunchVelocity=FVector::ZeroVector;
        FVector LandingFeet=FVector::ZeroVector;
        double ExpiresAt=0.;
        bool bBeganFalling=false;
        bool bBackingUp=false;
    } ObstacleTraversal;

    /// 一次后退记录，同一障碍只退一次
    struct FObstacleRetreat
    {
        FAIRequestID Request=FAIRequestID::InvalidRequest;
        TWeakObjectPtr<UPrimitiveComponent> Blocker;
        FVector Feet=FVector::ZeroVector;
        FVector BlockerLocation=FVector::ZeroVector;
    };

    /// 这次移动里后退过的记录
    TArray<FObstacleRetreat,TInlineAllocator<2>> ObstacleRetreatAttempts;

    /// 下次探测障碍的时间
    double NextObstacleProbeAt=0.;

    // ── 站位 ──

    /// 挪位用的移动请求
    FAIRequestID SpacingMoveId=FAIRequestID::InvalidRequest;

    /// 正在躲开的人
    TWeakObjectPtr<ADeliveryNpcCharacter> SpacingNeighbor;

    /// 下次检查站位的时间
    double NextSpacingCheck=0;

    /// 挪位最晚什么时候结束
    double SpacingMoveEndsAt=0;

    // ── 感知 ──

    /// 下次刷新视觉的时间
    double NextSightRefresh=0;

    /// 视觉配置
    UPROPERTY() TObjectPtr<UAISenseConfig_Sight> Sight;

    /// 听觉配置
    UPROPERTY() TObjectPtr<UAISenseConfig_Hearing> Hearing;

    // 看见、看不见、听见时写成 Agent 事件
    UFUNCTION() void OnTargetSeenOrHeard(AActor* Target,FAIStimulus Stimulus);
};
