#pragma once
#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_DeliveryCompanionFollow.generated.h"
class UDeliveryCompanionComponent;

// 树的默认分支持续执行主人的安排，倾听和已批准动作通过装饰器中断此分支
UCLASS()
class DELIVERY_API UBTTask_DeliveryCompanionFollow : public UBTTaskNode
{
    GENERATED_BODY()
public:
    UBTTask_DeliveryCompanionFollow();
    virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory) override;
    virtual void TickTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory,float DeltaSeconds) override;
    virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory) override;
    virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory,EBTNodeResult::Type Result) override;
private:
    TWeakObjectPtr<UDeliveryCompanionComponent> Companion;
};
