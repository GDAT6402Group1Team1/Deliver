#include "AI/BTTask_DeliveryExecuteApprovedAction.h"
#include "AI/Residents/DeliveryResidentCharacter.h"
#include "Navigation/PathFollowingComponent.h"
#include "AI/DeliveryNpcAgentComponent.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"

namespace
{
    // Resident 动作执行器独占该槽位，不覆盖导航或 Gameplay 的关注状态
    const EAIFocusPriority::Type ResidentActionFocus = EAIFocusPriority::LastFocusPriority + 1;
}

UBTTask_DeliveryExecuteApprovedAction::UBTTask_DeliveryExecuteApprovedAction()
{
    NodeName = TEXT("Delivery 执行已批准动作");
    bCreateNodeInstance = true;
    bNotifyTick = true;
    bNotifyTaskFinished = true;
}

EBTNodeResult::Type UBTTask_DeliveryExecuteApprovedAction::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
    ReleaseFocus();
    Controller = OwnerComp.GetAIOwner();
    APawn* Pawn = Controller.IsValid() ? Controller->GetPawn() : nullptr;
    Agent = Pawn ? Pawn->FindComponentByClass<UDeliveryNpcAgentComponent>() : nullptr;
    FDeliveryNpcPublicState Action;
    if (!Agent.IsValid() || !Agent->StartApprovedAction(Action)) return EBTNodeResult::Failed;
    ActionId = Action.ActionId;
    FinishAt = Pawn->GetWorld()->GetTimeSeconds() + Action.Duration;
    Controller->StopMovement();
    if (Action.Action == EDeliveryNpcAction::LookAt)
    {
        FocusTarget = Action.Target;
        Controller->SetFocus(Action.Target, ResidentActionFocus);
    }
    if (auto* Resident=Cast<ADeliveryResidentCharacter>(Pawn))
    {
        if (!Resident->BeginApprovedAction(Action))
        { Agent->TryFinishAction(ActionId,EDeliveryNpcActionStatus::Failed,TEXT("StartRejected")); return EBTNodeResult::Failed; }
        if (Action.Action==EDeliveryNpcAction::MoveTo)
        {
            const auto Result=Controller->MoveToActor(Action.Target,140.f,true,true,true,nullptr,true);
            if (Result==EPathFollowingRequestResult::Failed)
            { Agent->TryFinishAction(ActionId,EDeliveryNpcActionStatus::Failed,TEXT("PathUnavailable")); return EBTNodeResult::Failed; }
        }
    }
    return EBTNodeResult::InProgress;
}

void UBTTask_DeliveryExecuteApprovedAction::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
    if (!Agent.IsValid()) { FinishLatentTask(OwnerComp, EBTNodeResult::Failed); return; }
    const auto State = Agent->GetPublicState();
    if (State.ActionId != ActionId || State.Status != EDeliveryNpcActionStatus::Running)
    {
        FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
        return;
    }
    if (State.Action==EDeliveryNpcAction::MoveTo && Controller.IsValid() && Controller->GetMoveStatus()==EPathFollowingStatus::Idle)
    {
        const bool bNear=IsValid(State.Target) && FVector::DistSquared(Controller->GetPawn()->GetActorLocation(),State.Target->GetActorLocation())<FMath::Square(210.f);
        Agent->TryFinishAction(ActionId,bNear?EDeliveryNpcActionStatus::Succeeded:EDeliveryNpcActionStatus::Failed,bNear?NAME_None:FName(TEXT("PathInterrupted")));
        FinishLatentTask(OwnerComp,bNear?EBTNodeResult::Succeeded:EBTNodeResult::Failed); return;
    }
    if (OwnerComp.GetWorld()->GetTimeSeconds() >= FinishAt)
    {
        const bool bTimedOut = State.Action == EDeliveryNpcAction::MoveTo;
        const bool bFinished = Agent->TryFinishAction(ActionId,
            bTimedOut ? EDeliveryNpcActionStatus::Failed : EDeliveryNpcActionStatus::Succeeded,
            bTimedOut ? FName(TEXT("MoveTimeout")) : NAME_None);
        FinishLatentTask(OwnerComp, bFinished && !bTimedOut ? EBTNodeResult::Succeeded : EBTNodeResult::Failed);
    }
}

EBTNodeResult::Type UBTTask_DeliveryExecuteApprovedAction::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
    if (Agent.IsValid()) Agent->TryFinishAction(ActionId, EDeliveryNpcActionStatus::Cancelled, TEXT("BehaviorTreeAbort"));
    ReleaseFocus();
    return EBTNodeResult::Aborted;
}

void UBTTask_DeliveryExecuteApprovedAction::ReleaseFocus()
{
    if (Controller.IsValid() && ActionId.IsValid() && Controller->GetFocusActorForPriority(ResidentActionFocus) == FocusTarget.Get())
        Controller->ClearFocus(ResidentActionFocus);
    FocusTarget.Reset();
}

void UBTTask_DeliveryExecuteApprovedAction::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult)
{
    if (Agent.IsValid()) Agent->TryFinishAction(ActionId, EDeliveryNpcActionStatus::Cancelled, TEXT("ExecutorStopped"));
    ReleaseFocus();
    if (Controller.IsValid())
    {
        Controller->StopMovement();
        if (auto* Resident=Cast<ADeliveryResidentCharacter>(Controller->GetPawn())) Resident->EndApprovedAction(ActionId);
    }
    Agent.Reset();
    Controller.Reset();
    ActionId.Invalidate();
    Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}
