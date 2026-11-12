#pragma once
#include "CoreMinimal.h"
#include "AIController.h"
#include "Perception/AIPerceptionTypes.h"
#include "DeliveryNpcAIController.generated.h"
class UAISenseConfig_Sight;
class UAISenseConfig_Hearing;

// 使用 UE 的视听感知记录附近事件，并更新行为树需要的条件
// 模型回复由 Agent 处理，控制器负责启动行为树和控制 NPC 朝向
UCLASS()
class DELIVERY_API ADeliveryNpcAIController : public AAIController
{
    GENERATED_BODY()
public:
    ADeliveryNpcAIController();
    void RefreshBlackboard();
protected:
    virtual void OnPossess(APawn* Pawn) override;
    virtual void OnUnPossess() override;
private:
    double NextSightRefresh=0;
    UPROPERTY() TObjectPtr<UAISenseConfig_Sight> Sight;
    UPROPERTY() TObjectPtr<UAISenseConfig_Hearing> Hearing;
    UFUNCTION() void OnTargetSeenOrHeard(AActor* Target,FAIStimulus Stimulus);
};
