#pragma once
#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_DeliveryCompanionFollow.generated.h"
class UDeliveryCompanionComponent;

/**
 * @brief 二狗没有别的事做时，行为树执行这个节点，按主人的安排跟随、避让或原地等候
 *
 * 这是行为树的默认分支，TickTask 每帧调用同伴组件的 UpdateFollow
 * 倾听和执行模型选中的动作时，行为树的装饰器打断这个分支；打断时二狗只是说话的话继续移动，否则停下
 */
UCLASS()
class DELIVERY_API UBTTask_DeliveryCompanionFollow : public UBTTaskNode
{
    GENERATED_BODY()
public:
    UBTTask_DeliveryCompanionFollow();

    /// @brief 进入节点：先调用一次 UpdateFollow
    virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory) override;

    /// @brief 每帧调用 UpdateFollow
    virtual void TickTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory,float DeltaSeconds) override;

    /// @brief 被行为树打断：CanContinueCompanionMovement 为 false 时调用 StopFollowMovement
    virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory) override;

    /// @brief 节点结束：和 AbortTask 一样按情况停止移动，再清掉 Companion
    virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory,EBTNodeResult::Type Result) override;
private:
    /// 这个二狗的同伴组件
    TWeakObjectPtr<UDeliveryCompanionComponent> Companion;
};
