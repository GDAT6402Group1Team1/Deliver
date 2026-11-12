#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_DeliveryExecuteApprovedAction.generated.h"

class UDeliveryNpcAgentComponent;
class AAIController;

// 固定行为树中的动作执行节点，Agent 审核通过后才领取，不在节点里调用模型
UCLASS()
class DELIVERY_API UBTTask_DeliveryExecuteApprovedAction : public UBTTaskNode
{
    GENERATED_BODY()
public:
    UBTTask_DeliveryExecuteApprovedAction();
    virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
    virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
    virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
    virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) override;

private:
    void ReleaseFocus();
    // 节点实例化后每个 NPC 独享这些状态，不写入共享 BT 模板
    TWeakObjectPtr<UDeliveryNpcAgentComponent> Agent;
    TWeakObjectPtr<AAIController> Controller;
    TWeakObjectPtr<AActor> FocusTarget;
    FGuid ActionId;
    double FinishAt = 0;
};
