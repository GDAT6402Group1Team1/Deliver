#pragma once
#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_DeliveryCompanionErrand.generated.h"
class UDeliveryCompanionComponent;

/**
 * @brief 二狗替主人取件、帮送或交还东西时，行为树执行的节点
 *
 * 主人让二狗做的事由同伴组件 UDeliveryCompanionComponent 开始并保存，这个节点只负责每帧调用 UpdateErrand 推进它
 * ExecuteTask 记下主人命令的 CommandRevision，TickTask 推进，命令换了或委托结束时节点结束
 * 行为树打断这个节点时只停止二狗这次的移动，委托本身仍由同伴组件保留，之后回到这个节点接着做
 */
UCLASS()
class DELIVERY_API UBTTask_DeliveryCompanionErrand : public UBTTaskNode
{
    GENERATED_BODY()
public:
    UBTTask_DeliveryCompanionErrand();

    /// 为 true 时只在委托处于 SearchNearby（在附近找东西）阶段运行，找到后由另一个普通委托节点完成拾取和交还
    UPROPERTY(EditAnywhere,Category="委托") bool bSearchStageOnly=false;

    /// @brief 进入节点：记下 CommandRevision，先调用一次 UpdateErrand
    virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory) override;

    /// @brief 每帧调用 UpdateErrand，命令换了或委托结束时结束节点
    virtual void TickTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory,float DeltaSeconds) override;

    /// @brief 被行为树打断：按情况停止二狗的移动
    virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory) override;

    /// @brief 节点结束：按情况停止移动，清掉记录
    virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory,EBTNodeResult::Type Result) override;
private:
    /// @brief CommandRevision 没变且委托还在进行时返回 true
    bool CanRunStage() const;

    /// @brief 二狗不能边说话边继续移动时，调用 StopErrandMovement(true) 停下但不下车
    void StopUncontinuedMovement();

    /// 这个二狗的同伴组件
    TWeakObjectPtr<UDeliveryCompanionComponent> Companion;

    /// 进入节点时主人命令的版本号，和同伴组件当前的值不同说明主人换了命令
    int32 CommandRevision=INDEX_NONE;
};
