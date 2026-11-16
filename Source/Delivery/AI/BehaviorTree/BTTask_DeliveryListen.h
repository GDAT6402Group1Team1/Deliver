#pragma once
#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_DeliveryListen.generated.h"
class AAIController;

/**
 * @brief 行为树倾听分支：有人对 NPC 说话时停下来看着他，直到模型回答或交谈被打断
 *
 * BT_Resident 在 Agent 正在倾听时进入这个分支；ExecuteTask 让控制器看向说话的人，TickTask 等倾听结束
 */
UCLASS(Blueprintable)
class DELIVERY_API UBTTask_DeliveryListen : public UBTTaskNode
{
    GENERATED_BODY()
public:
    UBTTask_DeliveryListen();

    // ── 倾听 ──

    /// @brief 停下脚步，看向说话的人
    virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory) override;

    /// @brief 每帧看倾听结束没有，结束就成功返回
    virtual void TickTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory,float DeltaSeconds) override;

    /// @brief 撤掉看向
    virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp,uint8* Memory,EBTNodeResult::Type Result) override;
private:
    /// 设置看向的 AI 控制器
    TWeakObjectPtr<AAIController> Controller;
};
