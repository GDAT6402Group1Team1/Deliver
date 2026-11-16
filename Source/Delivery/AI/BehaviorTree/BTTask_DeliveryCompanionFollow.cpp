#include "AI/BehaviorTree/BTTask_DeliveryCompanionFollow.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "GameFramework/Pawn.h"

UBTTask_DeliveryCompanionFollow::UBTTask_DeliveryCompanionFollow()
{
    NodeName=TEXT("陪伴主人：跟随、避让或等候");
    bCreateNodeInstance=true; bNotifyTick=true; bNotifyTaskFinished=true;
}
EBTNodeResult::Type UBTTask_DeliveryCompanionFollow::ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory)
{
    const auto* AI=OwnerComp.GetAIOwner();
    Companion=AI && AI->GetPawn()?AI->GetPawn()->FindComponentByClass<UDeliveryCompanionComponent>():nullptr;
    if (!Companion.IsValid()) return EBTNodeResult::Failed;
    Companion->UpdateFollow();
    return EBTNodeResult::InProgress;
}
void UBTTask_DeliveryCompanionFollow::TickTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory,float DeltaSeconds)
{
    if (!Companion.IsValid()) { FinishLatentTask(OwnerComp,EBTNodeResult::Failed); return; }
    Companion->UpdateFollow();
}
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
