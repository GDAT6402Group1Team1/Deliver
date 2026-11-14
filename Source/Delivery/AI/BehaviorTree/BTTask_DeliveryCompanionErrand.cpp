#include "AI/BehaviorTree/BTTask_DeliveryCompanionErrand.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "GameFramework/Pawn.h"

UBTTask_DeliveryCompanionErrand::UBTTask_DeliveryCompanionErrand()
{
    NodeName=TEXT("执行主人委托：取件、帮送或交还");
    bCreateNodeInstance=true; bNotifyTick=true; bNotifyTaskFinished=true;
}
EBTNodeResult::Type UBTTask_DeliveryCompanionErrand::ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory)
{
    const auto* AI=OwnerComp.GetAIOwner();
    Companion=AI && AI->GetPawn()?AI->GetPawn()->FindComponentByClass<UDeliveryCompanionComponent>():nullptr;
    if (!Companion.IsValid() || !Companion->HasActiveErrand()) return EBTNodeResult::Failed;
    Companion->UpdateErrand();
    // 物品交接和任务通知允许同步销毁角色，返回后重新核对组件
    if (!Companion.IsValid()) return EBTNodeResult::Failed;
    return Companion->HasActiveErrand()?EBTNodeResult::InProgress:EBTNodeResult::Succeeded;
}
void UBTTask_DeliveryCompanionErrand::TickTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory,float DeltaSeconds)
{
    if (!Companion.IsValid()) { FinishLatentTask(OwnerComp,EBTNodeResult::Failed); return; }
    if (Companion->HasActiveErrand()) Companion->UpdateErrand();
    if (!Companion.IsValid()) { FinishLatentTask(OwnerComp,EBTNodeResult::Failed); return; }
    if (!Companion->HasActiveErrand()) FinishLatentTask(OwnerComp,EBTNodeResult::Succeeded);
}
EBTNodeResult::Type UBTTask_DeliveryCompanionErrand::AbortTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory)
{
    if (Companion.IsValid())
    {
        const auto* Agent=Companion->GetOwner()->FindComponentByClass<UDeliveryNpcAgentComponent>();
        // 被动对白接续同一委托，不能中止开箱、交接技能或本次导航
        if (!Agent || !Agent->CanContinueCompanionMovement()) Companion->StopErrandMovement();
    }
    return EBTNodeResult::Aborted;
}
void UBTTask_DeliveryCompanionErrand::OnTaskFinished(UBehaviorTreeComponent& OwnerComp,uint8* Memory,EBTNodeResult::Type Result)
{
    if (Companion.IsValid())
    {
        const auto* Agent=Companion->GetOwner()->FindComponentByClass<UDeliveryNpcAgentComponent>();
        // 被动对白接续同一委托，不能中止开箱、交接技能或本次导航
        if (!Agent || !Agent->CanContinueCompanionMovement()) Companion->StopErrandMovement();
    }
    Companion.Reset();
    Super::OnTaskFinished(OwnerComp,Memory,Result);
}
