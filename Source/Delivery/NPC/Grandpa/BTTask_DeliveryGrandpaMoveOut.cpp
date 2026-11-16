#include "NPC/Grandpa/BTTask_DeliveryGrandpaMoveOut.h"
#include "NPC/Grandpa/DeliveryGrandpa.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"

UBTTask_DeliveryGrandpaMoveOut::UBTTask_DeliveryGrandpaMoveOut()
{
    NodeName=TEXT("爷爷搬离共用房屋");
    bCreateNodeInstance=true;
    bNotifyTick=true;
    bNotifyTaskFinished=true;
}

EBTNodeResult::Type UBTTask_DeliveryGrandpaMoveOut::ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8*)
{
    const auto* Controller=OwnerComp.GetAIOwner();
    Resident=Controller?Cast<ADeliveryGrandpa>(Controller->GetPawn()):nullptr;
    if (!Resident.IsValid() || !Resident->BeginMoveOut()) return EBTNodeResult::Failed;
    return Resident->HasMovedOut()?EBTNodeResult::Succeeded:EBTNodeResult::InProgress;
}

void UBTTask_DeliveryGrandpaMoveOut::TickTask(UBehaviorTreeComponent& OwnerComp,uint8*,float DeltaSeconds)
{
    if (!Resident.IsValid() || OwnerComp.GetAIOwner()==nullptr || OwnerComp.GetAIOwner()->GetPawn()!=Resident.Get())
    { FinishLatentTask(OwnerComp,EBTNodeResult::Failed); return; }
    Resident->UpdateMoveOut(DeltaSeconds);
    if (Resident->HasMovedOut()) FinishLatentTask(OwnerComp,EBTNodeResult::Succeeded);
}

EBTNodeResult::Type UBTTask_DeliveryGrandpaMoveOut::AbortTask(UBehaviorTreeComponent&,uint8*)
{
    if (Resident.IsValid()) Resident->CancelMoveOut();
    return EBTNodeResult::Aborted;
}

void UBTTask_DeliveryGrandpaMoveOut::OnTaskFinished(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory,EBTNodeResult::Type Result)
{
    if (Resident.IsValid()) Resident->CancelMoveOut();
    Resident.Reset();
    Super::OnTaskFinished(OwnerComp,NodeMemory,Result);
}
