#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_DeliveryGrandpaMoveOut.generated.h"

class ADeliveryGrandpa;

// 可编辑蓝图任务的执行基类，行为树中断时停止爷爷本次离开
UCLASS(Blueprintable)
class DELIVERY_API UBTTask_DeliveryGrandpaMoveOut : public UBTTaskNode
{
    GENERATED_BODY()
public:
    UBTTask_DeliveryGrandpaMoveOut();
    virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory) override;
    virtual void TickTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory,float DeltaSeconds) override;
    virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory) override;
    virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory,EBTNodeResult::Type Result) override;
private:
    TWeakObjectPtr<ADeliveryGrandpa> Resident;
};
