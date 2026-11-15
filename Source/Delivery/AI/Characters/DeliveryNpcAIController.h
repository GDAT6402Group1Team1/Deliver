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

// 使用 UE 的视听感知记录附近事件，并更新行为树需要的条件
// 模型回复由 Agent 处理，控制器负责行为树、朝向和空闲时的站位避让
UCLASS()
class DELIVERY_API ADeliveryNpcAIController : public AAIController
{
    GENERATED_BODY()
public:
    ADeliveryNpcAIController();
    void RefreshBlackboard();
    virtual void Tick(float DeltaSeconds) override;
    bool IsTraversingObstacle(FAIRequestID Request) const;
    UPROPERTY(EditAnywhere,Category="Delivery|站位") bool bKeepDistanceWhenIdle=true;
    // 身体边缘靠近到这个距离才挪步，胶囊半径另外计算
    UPROPERTY(EditAnywhere,Category="Delivery|站位",meta=(ClampMin="20",ClampMax="150",Units="cm")) float MinStandingGap=80;
    // 挪开后留出更宽间隔，避免刚停下又开始移动
    UPROPERTY(EditAnywhere,Category="Delivery|站位",meta=(ClampMin="40",ClampMax="200",Units="cm")) float ComfortableStandingGap=120;
    // 检查导航路线上的低障碍，跳起和落地都由角色移动组件处理
    UPROPERTY(EditAnywhere,Category="Delivery|越障") bool bAutoJumpLowObstacles=true;
    UPROPERTY(EditAnywhere,Category="Delivery|越障",meta=(ClampMin="60",ClampMax="180",Units="cm")) float ObstacleJumpProbeDistance=130.f;
    UPROPERTY(EditAnywhere,Category="Delivery|越障",meta=(ClampMin="25",ClampMax="100",Units="cm")) float ObstacleJumpMaxLandingRise=80.f;
    UPROPERTY(EditAnywhere,Category="Delivery|越障",meta=(ClampMin="0.2",ClampMax="3",Units="s")) float ObstacleJumpRetrySeconds=.85f;
protected:
    virtual void OnPossess(APawn* Pawn) override;
    virtual void OnUnPossess() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    friend class FDeliveryNpcObstacleTraversalTest;
    friend class FDeliveryCompanionRulesTest;
    void UpdateStandingSpace();
    void StopSpacingMove();
    bool HasRoomToStand(const ADeliveryNpcCharacter* Npc,const FVector& Destination) const;
    void UpdateObstacleTraversal();
    bool TryStartObstacleTraversal(ADeliveryNpcCharacter& Npc,UPathFollowingComponent& Path,double Now);
    bool CanTraverseObstacle(const ADeliveryNpcCharacter& Npc) const;
    bool HasClearObstacleJumpArc(const ADeliveryNpcCharacter& Npc,const FVector& LaunchVelocity,double FlightTime) const;
    static FVector ExtendObstacleRouteTarget(const FNavigationPath& Route,uint32 NextPathIndex,
        const FVector& Feet,const FVector& Direction,const FVector& CurrentTarget,const FNavLinkId& WalkThroughLink,bool& bStopsBeforeCustomLink);
    bool HasTriedObstacleRetreat(FAIRequestID Request,const UPrimitiveComponent& Blocker,const FVector& Feet) const;
    void StopObstacleTraversal();
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
    struct FObstacleRetreat
    {
        FAIRequestID Request=FAIRequestID::InvalidRequest;
        TWeakObjectPtr<UPrimitiveComponent> Blocker;
        FVector Feet=FVector::ZeroVector;
        FVector BlockerLocation=FVector::ZeroVector;
    };
    TArray<FObstacleRetreat,TInlineAllocator<2>> ObstacleRetreatAttempts;
    double NextObstacleProbeAt=0.;
    FAIRequestID SpacingMoveId=FAIRequestID::InvalidRequest;
    TWeakObjectPtr<ADeliveryNpcCharacter> SpacingNeighbor;
    double NextSpacingCheck=0;
    double SpacingMoveEndsAt=0;
    double NextSightRefresh=0;
    UPROPERTY() TObjectPtr<UAISenseConfig_Sight> Sight;
    UPROPERTY() TObjectPtr<UAISenseConfig_Hearing> Hearing;
    UFUNCTION() void OnTargetSeenOrHeard(AActor* Target,FAIStimulus Stimulus);
};
