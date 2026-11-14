#pragma once
#include "CoreMinimal.h"
#include "AIController.h"
#include "Perception/AIPerceptionTypes.h"
#include "DeliveryNpcAIController.generated.h"
class UAISenseConfig_Sight;
class UAISenseConfig_Hearing;
class ADeliveryNpcCharacter;

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
    UPROPERTY(EditAnywhere,Category="Delivery|站位") bool bKeepDistanceWhenIdle=true;
    // 身体边缘靠近到这个距离才挪步，胶囊半径另外计算
    UPROPERTY(EditAnywhere,Category="Delivery|站位",meta=(ClampMin="20",ClampMax="150",Units="cm")) float MinStandingGap=80;
    // 挪开后留出更宽间隔，避免刚停下又开始移动
    UPROPERTY(EditAnywhere,Category="Delivery|站位",meta=(ClampMin="40",ClampMax="200",Units="cm")) float ComfortableStandingGap=120;
protected:
    virtual void OnPossess(APawn* Pawn) override;
    virtual void OnUnPossess() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    void UpdateStandingSpace();
    void StopSpacingMove();
    bool HasRoomToStand(const ADeliveryNpcCharacter* Npc,const FVector& Destination) const;
    FAIRequestID SpacingMoveId=FAIRequestID::InvalidRequest;
    TWeakObjectPtr<ADeliveryNpcCharacter> SpacingNeighbor;
    double NextSpacingCheck=0;
    double SpacingMoveEndsAt=0;
    double NextSightRefresh=0;
    UPROPERTY() TObjectPtr<UAISenseConfig_Sight> Sight;
    UPROPERTY() TObjectPtr<UAISenseConfig_Hearing> Hearing;
    UFUNCTION() void OnTargetSeenOrHeard(AActor* Target,FAIStimulus Stimulus);
};
