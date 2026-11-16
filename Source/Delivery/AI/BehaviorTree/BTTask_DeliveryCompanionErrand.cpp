#include "AI/BehaviorTree/BTTask_DeliveryCompanionErrand.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "GameFramework/Pawn.h"

// ── 委托 ──
// 主人让二狗取件、帮送或交还东西时，同伴组件开始一项委托，行为树进入这个节点
// ExecuteTask 记下进入时的 CommandRevision（主人命令的版本号），之后 TickTask 每帧调用 UpdateErrand 推进委托
// 主人换了命令（CommandRevision 变了）或者委托结束时，CanRunStage 返回 false，节点结束；结束或被打断时调用 StopUncontinuedMovement

/// @brief 每个二狗各有一份节点实例，打开 TickTask 和 OnTaskFinished
UBTTask_DeliveryCompanionErrand::UBTTask_DeliveryCompanionErrand()
{
    NodeName=TEXT("执行主人委托：取件、帮送或交还");
    bCreateNodeInstance=true; bNotifyTick=true; bNotifyTaskFinished=true;
}

/// @brief 进入节点时的 CommandRevision 没变、委托还在进行，返回 true
/// bSearchStageOnly 为 true 的节点只用在寻找阶段，委托还要处于 SearchNearby 阶段
bool UBTTask_DeliveryCompanionErrand::CanRunStage() const
{
    const auto* Component=Companion.Get();
    return Component && Component->GetCommandRevision()==CommandRevision && Component->HasActiveErrand() &&
        (!bSearchStageOnly || Component->HasNearbyItemSearch());
}

/// @brief 行为树进入节点时调用：找到同伴组件，记下 CommandRevision，先调用一次 UpdateErrand
/// 委托在这一次里已经结束就返回 Succeeded，否则返回 InProgress
EBTNodeResult::Type UBTTask_DeliveryCompanionErrand::ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory)
{
    const auto* AI=OwnerComp.GetAIOwner();
    Companion=AI && AI->GetPawn()?AI->GetPawn()->FindComponentByClass<UDeliveryCompanionComponent>():nullptr;
    CommandRevision=Companion.IsValid()?Companion->GetCommandRevision():INDEX_NONE;
    if (!CanRunStage()) return EBTNodeResult::Failed;
    Companion->UpdateErrand();

    // UpdateErrand 里交接物品、发任务通知时，收到通知的代码可能销毁二狗，所以返回后再检查一次 Companion
    if (!Companion.IsValid()) return EBTNodeResult::Failed;
    return CanRunStage()?EBTNodeResult::InProgress:EBTNodeResult::Succeeded;
}

/// @brief 每帧调用：CanRunStage 为 true 时调用 UpdateErrand；二狗没了返回 Failed，委托结束或命令换了返回 Succeeded
void UBTTask_DeliveryCompanionErrand::TickTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory,float DeltaSeconds)
{
    if (!Companion.IsValid()) { FinishLatentTask(OwnerComp,EBTNodeResult::Failed); return; }
    if (CanRunStage()) Companion->UpdateErrand();
    if (!Companion.IsValid()) { FinishLatentTask(OwnerComp,EBTNodeResult::Failed); return; }
    if (!CanRunStage()) FinishLatentTask(OwnerComp,EBTNodeResult::Succeeded);
}

/// @brief 行为树打断这个节点（例如开始倾听）时调用 StopUncontinuedMovement，委托本身由同伴组件保留
EBTNodeResult::Type UBTTask_DeliveryCompanionErrand::AbortTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory)
{
    StopUncontinuedMovement();
    return EBTNodeResult::Aborted;
}

/// @brief 节点结束时调用 StopUncontinuedMovement，再清掉 Companion 和 CommandRevision
void UBTTask_DeliveryCompanionErrand::OnTaskFinished(UBehaviorTreeComponent& OwnerComp,uint8* Memory,EBTNodeResult::Type Result)
{
    StopUncontinuedMovement();
    Companion.Reset();
    CommandRevision=INDEX_NONE;
    Super::OnTaskFinished(OwnerComp,Memory,Result);
}

/// @brief 委托还在进行、命令也没换时，按情况停止二狗的移动
/// CanContinueCompanionMovement 为 true（二狗只是边走边说话）时不停；否则调用 StopErrandMovement(true)，停下但不下车
void UBTTask_DeliveryCompanionErrand::StopUncontinuedMovement()
{
    auto* Component=Companion.Get();
    if (!Component || Component->GetCommandRevision()!=CommandRevision || !Component->HasActiveErrand() ||
        !IsValid(Component->GetOwner())) return;
    const auto* Agent=Component->GetOwner()->FindComponentByClass<UDeliveryNpcAgentComponent>();

    // CanContinueCompanionMovement 为 true 说明这次打断只是说话，二狗继续原来的移动；否则调用 StopErrandMovement，参数 true 表示停下但留在车上
    if (!Agent || !Agent->CanContinueCompanionMovement()) Component->StopErrandMovement(true);
}
