#include "AI/BehaviorTree/BTTask_DeliveryCompanionFollow.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "GameFramework/Pawn.h"

// ── 跟随 ──
// 二狗没有别的事做时，行为树的默认分支进入这个节点，TickTask 每帧调用 UpdateFollow 执行主人的安排：跟随、避让或原地等候
// 倾听、执行模型选中的动作时，行为树的装饰器打断这个分支；AbortTask 和 OnTaskFinished 按情况停止二狗的移动

/// @brief 每个二狗各有一份节点实例，打开 TickTask 和 OnTaskFinished
UBTTask_DeliveryCompanionFollow::UBTTask_DeliveryCompanionFollow()
{
    NodeName=TEXT("陪伴主人：跟随、避让或等候");
    bCreateNodeInstance=true; bNotifyTick=true; bNotifyTaskFinished=true;
}

/// @brief 行为树进入节点时调用：找到同伴组件，先调用一次 UpdateFollow，返回 InProgress
EBTNodeResult::Type UBTTask_DeliveryCompanionFollow::ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory)
{
    const auto* AI=OwnerComp.GetAIOwner();
    Companion=AI && AI->GetPawn()?AI->GetPawn()->FindComponentByClass<UDeliveryCompanionComponent>():nullptr;
    if (!Companion.IsValid()) return EBTNodeResult::Failed;
    Companion->UpdateFollow();
    return EBTNodeResult::InProgress;
}

/// @brief 每帧调用 UpdateFollow；二狗没了就返回 Failed
void UBTTask_DeliveryCompanionFollow::TickTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory,float DeltaSeconds)
{
    if (!Companion.IsValid()) { FinishLatentTask(OwnerComp,EBTNodeResult::Failed); return; }
    Companion->UpdateFollow();
}

/// @brief 行为树打断这个节点时调用：CanContinueCompanionMovement 为 false 时调用 StopFollowMovement 停下
/// StopFollowMovement 的参数是 CanKeepVehicleSeatWhilePaused 的结果，为 true 时停下但留在车上
EBTNodeResult::Type UBTTask_DeliveryCompanionFollow::AbortTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory)
{
    if (Companion.IsValid())
    {
        const auto* Agent=Companion->GetOwner()->FindComponentByClass<UDeliveryNpcAgentComponent>();
        if (!Agent || !Agent->CanContinueCompanionMovement())
            Companion->StopFollowMovement(Companion->CanKeepVehicleSeatWhilePaused());
    }
    return EBTNodeResult::Aborted;
}

/// @brief 节点结束时和 AbortTask 一样按情况调用 StopFollowMovement，再清掉 Companion
void UBTTask_DeliveryCompanionFollow::OnTaskFinished(UBehaviorTreeComponent& OwnerComp,uint8* Memory,EBTNodeResult::Type Result)
{
    if (Companion.IsValid())
    {
        const auto* Agent=Companion->GetOwner()->FindComponentByClass<UDeliveryNpcAgentComponent>();
        if (!Agent || !Agent->CanContinueCompanionMovement())
            Companion->StopFollowMovement(Companion->CanKeepVehicleSeatWhilePaused());
    }
    Companion.Reset();
    Super::OnTaskFinished(OwnerComp,Memory,Result);
}
