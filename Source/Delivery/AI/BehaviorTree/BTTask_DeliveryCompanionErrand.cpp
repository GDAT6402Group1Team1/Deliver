#include "AI/BehaviorTree/BTTask_DeliveryCompanionErrand.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "GameFramework/Pawn.h"

UBTTask_DeliveryCompanionErrand::UBTTask_DeliveryCompanionErrand()
{
    NodeName=TEXT("执行主人委托：取件、帮送或交还");
    bCreateNodeInstance=true; bNotifyTick=true; bNotifyTaskFinished=true;
}
bool UBTTask_DeliveryCompanionErrand::CanRunStage() const
{
    const auto* Component=Companion.Get();
    return Component && Component->GetCommandRevision()==CommandRevision && Component->HasActiveErrand() &&
        (!bSearchStageOnly || Component->HasNearbyItemSearch());
}
EBTNodeResult::Type UBTTask_DeliveryCompanionErrand::ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory)
{
    const auto* AI=OwnerComp.GetAIOwner();
    Companion=AI && AI->GetPawn()?AI->GetPawn()->FindComponentByClass<UDeliveryCompanionComponent>():nullptr;
    CommandRevision=Companion.IsValid()?Companion->GetCommandRevision():INDEX_NONE;
    if (!CanRunStage()) return EBTNodeResult::Failed;
    Companion->UpdateErrand();
    // 物品交接和任务通知允许同步销毁角色，返回后重新核对组件
    if (!Companion.IsValid()) return EBTNodeResult::Failed;
    return CanRunStage()?EBTNodeResult::InProgress:EBTNodeResult::Succeeded;
}
void UBTTask_DeliveryCompanionErrand::TickTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory,float DeltaSeconds)
{
    if (!Companion.IsValid()) { FinishLatentTask(OwnerComp,EBTNodeResult::Failed); return; }
    if (CanRunStage()) Companion->UpdateErrand();
    if (!Companion.IsValid()) { FinishLatentTask(OwnerComp,EBTNodeResult::Failed); return; }
    if (!CanRunStage()) FinishLatentTask(OwnerComp,EBTNodeResult::Succeeded);
}
EBTNodeResult::Type UBTTask_DeliveryCompanionErrand::AbortTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory)
{
    StopUncontinuedMovement();
    return EBTNodeResult::Aborted;
}
void UBTTask_DeliveryCompanionErrand::OnTaskFinished(UBehaviorTreeComponent& OwnerComp,uint8* Memory,EBTNodeResult::Type Result)
{
    StopUncontinuedMovement();
    Companion.Reset();
    CommandRevision=INDEX_NONE;
    Super::OnTaskFinished(OwnerComp,Memory,Result);
}
void UBTTask_DeliveryCompanionErrand::StopUncontinuedMovement()
{
    auto* Component=Companion.Get();
    if (!Component || Component->GetCommandRevision()!=CommandRevision || !Component->HasActiveErrand() ||
        !IsValid(Component->GetOwner())) return;
    const auto* Agent=Component->GetOwner()->FindComponentByClass<UDeliveryNpcAgentComponent>();
    // 被动对白可接续原动作，短后仰由当前委托核对骑乘后停车
    if (!Agent || !Agent->CanContinueCompanionMovement()) Component->StopErrandMovement(true);
}
