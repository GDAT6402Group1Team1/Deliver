#include "AI/BehaviorTree/BTTask_DeliveryExecuteNpcAction.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "Navigation/PathFollowingComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"

namespace
{
    // NPC 动作使用单独的朝向优先级，不覆盖导航或其他玩法设置的朝向
    const EAIFocusPriority::Type ResidentActionFocus = EAIFocusPriority::LastFocusPriority + 1;
}

UBTTask_DeliveryExecuteNpcAction::UBTTask_DeliveryExecuteNpcAction()
{
    NodeName = TEXT("Delivery 执行 NPC 动作");
    bCreateNodeInstance = true;
    bNotifyTick = true;
    bNotifyTaskFinished = true;
}

EBTNodeResult::Type UBTTask_DeliveryExecuteNpcAction::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
    ReleaseFocus();
    Controller = OwnerComp.GetAIOwner();
    APawn* Pawn = Controller.IsValid() ? Controller->GetPawn() : nullptr;
    Agent = Pawn ? Pawn->FindComponentByClass<UDeliveryNpcAgentComponent>() : nullptr;
    FDeliveryNpcPublicState Action;
    if (!Agent.IsValid() || !Agent->StartAction(Action)) return EBTNodeResult::Failed;
    ActionId = Action.ActionId;
    FinishAt = Pawn->GetWorld()->GetTimeSeconds() + Action.Duration;
    Controller->StopMovement();
    if (Action.Action == EDeliveryNpcAction::LookAt)
    {
        FocusTarget = Action.Target;
        Controller->SetFocus(Action.Target, ResidentActionFocus);
    }
    if (auto* Resident=Cast<ADeliveryNpcCharacter>(Pawn))
    {
        if (!Resident->ExecuteAction(Action))
        { Agent->TryFinishAction(ActionId,EDeliveryNpcActionStatus::Failed,TEXT("StartRejected")); return EBTNodeResult::Failed; }
        if (Action.Action==EDeliveryNpcAction::MoveTo)
        {
            const auto Result=Controller->MoveToActor(Action.Target,ADeliveryNpcCharacter::MoveAcceptanceRadius,true,true,true,nullptr,true);
            if (Result==EPathFollowingRequestResult::Failed)
            { Agent->TryFinishAction(ActionId,EDeliveryNpcActionStatus::Failed,TEXT("PathUnavailable")); return EBTNodeResult::Failed; }
        }
    }
    return EBTNodeResult::InProgress;
}

void UBTTask_DeliveryExecuteNpcAction::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
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
        const auto* Npc=Cast<ADeliveryNpcCharacter>(Controller->GetPawn());
        const bool bNear=Npc && Npc->HasReachedMoveTarget(State.Target);
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

EBTNodeResult::Type UBTTask_DeliveryExecuteNpcAction::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
    if (Agent.IsValid()) Agent->TryFinishAction(ActionId, EDeliveryNpcActionStatus::Cancelled, TEXT("BehaviorTreeAbort"));
    ReleaseFocus();
    return EBTNodeResult::Aborted;
}

void UBTTask_DeliveryExecuteNpcAction::ReleaseFocus()
{
    if (Controller.IsValid() && ActionId.IsValid() && Controller->GetFocusActorForPriority(ResidentActionFocus) == FocusTarget.Get())
        Controller->ClearFocus(ResidentActionFocus);
    FocusTarget.Reset();
}

void UBTTask_DeliveryExecuteNpcAction::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult)
{
    if (Agent.IsValid()) Agent->TryFinishAction(ActionId, EDeliveryNpcActionStatus::Cancelled, TEXT("ExecutorStopped"));
    ReleaseFocus();
    if (Controller.IsValid())
    {
        Controller->StopMovement();
        if (auto* Resident=Cast<ADeliveryNpcCharacter>(Controller->GetPawn())) Resident->StopAction(ActionId);
    }
    Agent.Reset();
    Controller.Reset();
    ActionId.Invalidate();
    Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}
