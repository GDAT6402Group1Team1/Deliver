#pragma once
#include "CoreMinimal.h"
#include "AIController.h"
#include "Perception/AIPerceptionTypes.h"
#include "DeliveryResidentAIController.generated.h"
class UAISenseConfig_Sight;
class UAISenseConfig_Hearing;

// 内置 AI Perception 提供视听事实，黑板驱动保存的行为树，不在控制器生成模型动作
UCLASS()
class DELIVERY_API ADeliveryResidentAIController : public AAIController
{
    GENERATED_BODY()
public:
    ADeliveryResidentAIController();
    void RefreshBlackboard();
protected:
    virtual void OnPossess(APawn* Pawn) override;
    virtual void OnUnPossess() override;
private:
    double NextSightRefresh=0;
    UPROPERTY() TObjectPtr<UAISenseConfig_Sight> Sight;
    UPROPERTY() TObjectPtr<UAISenseConfig_Hearing> Hearing;
    UFUNCTION() void OnTargetPerceived(AActor* Target,FAIStimulus Stimulus);
};
