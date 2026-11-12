#pragma once
#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_DeliveryListen.generated.h"
class AAIController;

// 玩家说话后立即看向玩家，直到模型回复或交谈被中断
UCLASS(Blueprintable)
class DELIVERY_API UBTTask_DeliveryListen : public UBTTaskNode
{
    GENERATED_BODY()
public:
    UBTTask_DeliveryListen();
    virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory) override;
    virtual void TickTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory,float DeltaSeconds) override;
    virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp,uint8* Memory,EBTNodeResult::Type Result) override;
private:
    TWeakObjectPtr<AAIController> Controller;
};
