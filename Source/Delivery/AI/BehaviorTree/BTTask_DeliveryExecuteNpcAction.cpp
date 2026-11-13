#include "AI/BehaviorTree/BTTask_DeliveryExecuteNpcAction.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
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
    bStrikeStarted=false;
    StrikeFinishAt=0;
    bReturningToOrigin=false;
    FightPunchId.Invalidate();
    bFightMoveRequested=false;
    Controller = OwnerComp.GetAIOwner();
    APawn* Pawn = Controller.IsValid() ? Controller->GetPawn() : nullptr;
    Agent = Pawn ? Pawn->FindComponentByClass<UDeliveryNpcAgentComponent>() : nullptr;
    FDeliveryNpcPublicState Action;
    if (!Agent.IsValid() || !Agent->StartAction(Action)) return EBTNodeResult::Failed;
    ActionId = Action.ActionId;
    FinishAt = Pawn->GetWorld()->GetTimeSeconds() + Action.Duration;
    Controller->StopMovement();
    if (Action.Action == EDeliveryNpcAction::LookAt || Action.Action == EDeliveryNpcAction::StrikeOnce || Action.Action == EDeliveryNpcAction::Fight)
    {
        FocusTarget = Action.Target;
        Controller->SetFocus(Action.Target, ResidentActionFocus);
    }
    if (auto* Resident=Cast<ADeliveryNpcCharacter>(Pawn))
    {
        if (Action.Action==EDeliveryNpcAction::StrikeOnce && Action.Duration<UDeliveryNpcCombatComponent::StrikeWindup)
        { Agent->TryFinishAction(ActionId,EDeliveryNpcActionStatus::Failed,TEXT("StrikeDurationTooShort")); return EBTNodeResult::Failed; }
        if (!Resident->ExecuteAction(Action))
        { Agent->TryFinishAction(ActionId,EDeliveryNpcActionStatus::Failed,TEXT("StartRejected")); return EBTNodeResult::Failed; }
        if (Action.Action==EDeliveryNpcAction::MoveTo)
        {
            const auto Result=Controller->MoveToActor(Action.Target,ADeliveryNpcCharacter::MoveAcceptanceRadius,true,true,true,nullptr,false);
            if (Result==EPathFollowingRequestResult::Failed)
            { Agent->TryFinishAction(ActionId,EDeliveryNpcActionStatus::Failed,TEXT("PathUnavailable")); return EBTNodeResult::Failed; }
        }
        else if (Action.Action==EDeliveryNpcAction::StrikeOnce)
        {
            bStrikeStarted=Resident->Combat->GetStrikeStartTime(ActionId)>=0;
            if (bStrikeStarted)
                StrikeFinishAt=FMath::Min(FinishAt,Pawn->GetWorld()->GetTimeSeconds()+UDeliveryNpcCombatComponent::StrikeDuration);
            else
            {
                // 攻击按两人中心距离停步，不能再把双方胶囊半径加进到达距离
                FAIMoveRequest Request(Action.Target);
                Request.SetNavigationFilter(Controller->GetDefaultNavigationFilterClass());
                Request.SetAcceptanceRadius(FMath::Max(10.f,Resident->Combat->StrikeRange-25.f));
                Request.SetReachTestIncludesAgentRadius(false);
                Request.SetReachTestIncludesGoalRadius(false);
                Request.SetAllowPartialPath(false);
                if (Controller->MoveTo(Request).Code==EPathFollowingRequestResult::Failed)
                { Agent->TryFinishAction(ActionId,EDeliveryNpcActionStatus::Failed,TEXT("PathUnavailable")); return EBTNodeResult::Failed; }
            }
        }
        else if (Action.Action==EDeliveryNpcAction::Pace || Action.Action==EDeliveryNpcAction::MoveAside)
        {
            WalkOrigin=Resident->GetNavAgentLocation();
            const bool bFound=Action.Action==EDeliveryNpcAction::Pace?Resident->FindPaceDestination(WalkDestination):
                Resident->FindSideStepDestination(Action.Target,WalkDestination);
            if (!bFound || !StartNearbyMove(WalkDestination))
            { Agent->TryFinishAction(ActionId,EDeliveryNpcActionStatus::Failed,TEXT("NearbyPathUnavailable")); return EBTNodeResult::Failed; }
        }
    }
    return EBTNodeResult::InProgress;
}

bool UBTTask_DeliveryExecuteNpcAction::StartNearbyMove(const FVector& Destination)
{
    if (!Controller.IsValid()) return false;
    FAIMoveRequest Request(Destination);
    Request.SetAcceptanceRadius(8.f);
    Request.SetReachTestIncludesAgentRadius(false);
    Request.SetReachTestIncludesGoalRadius(false);
    Request.SetAllowPartialPath(false);
    return Controller->MoveTo(Request).Code!=EPathFollowingRequestResult::Failed;
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
    if (State.Action==EDeliveryNpcAction::Fight)
    {
        TickFight(OwnerComp,Controller.IsValid()?Cast<ADeliveryNpcCharacter>(Controller->GetPawn()):nullptr,State);
        return;
    }
    if (State.Action==EDeliveryNpcAction::Pace || State.Action==EDeliveryNpcAction::MoveAside)
    {
        auto* Npc=Controller.IsValid()?Cast<ADeliveryNpcCharacter>(Controller->GetPawn()):nullptr;
        FName Failure;
        if (!Npc || Npc->IsKnockedDown()) Failure=TEXT("WalkerUnavailable");
        else if (State.Action==EDeliveryNpcAction::MoveAside && !IsValid(State.Target)) Failure=TEXT("StepAsideTargetLost");
        else if (OwnerComp.GetWorld()->GetTimeSeconds()>=FinishAt) Failure=TEXT("NearbyMoveTimeout");
        else if (Controller->GetMoveStatus()==EPathFollowingStatus::Idle)
        {
            const FVector Feet=Npc->GetNavAgentLocation();
            const bool bReached=FVector::Dist2D(Feet,WalkDestination)<=12 && FMath::Abs(Feet.Z-WalkDestination.Z)<=60;
            if (!bReached) Failure=TEXT("NearbyPathInterrupted");
            else if (State.Action==EDeliveryNpcAction::Pace && !bReturningToOrigin)
            {
                // 返回本次记录的起点，让一次踱步停在附近
                bReturningToOrigin=true;
                WalkDestination=WalkOrigin;
                if (!StartNearbyMove(WalkDestination)) Failure=TEXT("PaceReturnBlocked");
            }
            else
            {
                Agent->TryFinishAction(ActionId,EDeliveryNpcActionStatus::Succeeded,NAME_None);
                FinishLatentTask(OwnerComp,EBTNodeResult::Succeeded);
                return;
            }
        }
        if (!Failure.IsNone())
        {
            Agent->TryFinishAction(ActionId,EDeliveryNpcActionStatus::Failed,Failure);
            FinishLatentTask(OwnerComp,EBTNodeResult::Failed);
        }
        return;
    }
    if (State.Action==EDeliveryNpcAction::StrikeOnce)
    {
        auto* Npc=Controller.IsValid()?Cast<ADeliveryNpcCharacter>(Controller->GetPawn()):nullptr;
        const double Now=OwnerComp.GetWorld()->GetTimeSeconds();
        FName Failure;
        if (!Npc || Npc->IsKnockedDown()) Failure=TEXT("AttackerUnavailable");
        else if (bStrikeStarted)
        {
            if (Npc->Combat->GetStrikeStartTime(ActionId)<0) Failure=TEXT("StrikeCancelled");
            else if (Now>=StrikeFinishAt)
            {
                bool bHit=false;
                const bool bFinished=Npc->Combat->GetStrikeResult(ActionId,bHit);
                Agent->TryFinishAction(ActionId,bFinished && bHit?EDeliveryNpcActionStatus::Succeeded:EDeliveryNpcActionStatus::Failed,
                    bFinished && bHit?NAME_None:FName(TEXT("StrikeMissed")));
                FinishLatentTask(OwnerComp,bFinished && bHit?EBTNodeResult::Succeeded:EBTNodeResult::Failed);
                return;
            }
        }
        else if (Now+UDeliveryNpcCombatComponent::StrikeWindup>FinishAt) Failure=TEXT("ApproachTimeout");
        else if (!Npc->Combat->CanContinueStrikeApproach(State.Target)) Failure=TEXT("StrikeTargetUnavailable");
        else if (Npc->Combat->CanStrike(State.Target))
        {
            Controller->StopMovement();
            bStrikeStarted=Npc->Combat->StartStrike(State.Target,ActionId);
            if (bStrikeStarted) StrikeFinishAt=FMath::Min(FinishAt,Now+UDeliveryNpcCombatComponent::StrikeDuration);
            else Failure=TEXT("StrikeStartRejected");
        }
        else if (Controller->GetMoveStatus()==EPathFollowingStatus::Idle) Failure=TEXT("PathInterrupted");
        if (!Failure.IsNone())
        {
            Agent->TryFinishAction(ActionId,EDeliveryNpcActionStatus::Failed,Failure);
            FinishLatentTask(OwnerComp,EBTNodeResult::Failed);
        }
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

void UBTTask_DeliveryExecuteNpcAction::FinishFight(UBehaviorTreeComponent& OwnerComp,FName Reason,bool bFailed)
{
    if (Agent.IsValid()) Agent->TryFinishAction(ActionId,
        bFailed?EDeliveryNpcActionStatus::Failed:EDeliveryNpcActionStatus::Succeeded,Reason);
    FinishLatentTask(OwnerComp,bFailed?EBTNodeResult::Failed:EBTNodeResult::Succeeded);
}

void UBTTask_DeliveryExecuteNpcAction::TickFight(UBehaviorTreeComponent& OwnerComp,ADeliveryNpcCharacter* Npc,const FDeliveryNpcPublicState& State)
{
    if (!Npc || !Controller.IsValid()) { FinishFight(OwnerComp,TEXT("AttackerUnavailable"),true); return; }
    auto* Combat=Npc->Combat.Get();
    const FName Stop=Combat->GetFightStopReason(ActionId);
    if (!Stop.IsNone())
    {
        const bool bFinished=Stop==TEXT("TargetDown") || Stop==TEXT("TargetEscaped") || Stop==TEXT("FightTimeLimit");
        FinishFight(OwnerComp,Stop,!bFinished);
        return;
    }
    if (Combat->IsReactingToHit())
    {
        Controller->StopMovement();
        bFightMoveRequested=false;
        return;
    }
    const double Now=OwnerComp.GetWorld()->GetTimeSeconds();
    if (bStrikeStarted)
    {
        bool bHit=false;
        const bool bJudged=Combat->GetStrikeResult(FightPunchId,bHit);
        // 轻拳打断时已保存未命中结果，后仰结束后可以接下一拳
        if (Combat->GetStrikeStartTime(ActionId)<0 && !bJudged)
        { FinishFight(OwnerComp,TEXT("StrikeCancelled"),true); return; }
        if (Combat->GetStrikeStartTime(ActionId)>=0 && Now<StrikeFinishAt) return;
        if (!bJudged) { FinishFight(OwnerComp,TEXT("StrikeResultMissing"),true); return; }
        bStrikeStarted=false;
        FightPunchId.Invalidate();
    }
    if (Combat->GetFightPunchCount()>=Combat->GetFightPunchLimit())
    { FinishFight(OwnerComp,TEXT("FightPunchLimit"),false); return; }
    if (!Combat->IsReadyToStrike()) return;
    if (Now+UDeliveryNpcCombatComponent::StrikeWindup>FinishAt)
    { FinishFight(OwnerComp,TEXT("FightTimeLimit"),false); return; }
    if (Combat->CanStrike(State.Target))
    {
        Controller->StopMovement(); bFightMoveRequested=false;
        FightPunchId=FGuid::NewGuid();
        bStrikeStarted=Combat->StartFightStrike(State.Target,ActionId,FightPunchId);
        if (!bStrikeStarted) { FinishFight(OwnerComp,TEXT("StrikeStartRejected"),true); return; }
        StrikeFinishAt=Now+UDeliveryNpcCombatComponent::StrikeDuration;
        return;
    }
    if (Controller->GetMoveStatus()==EPathFollowingStatus::Idle)
    {
        if (bFightMoveRequested) { FinishFight(OwnerComp,TEXT("PathInterrupted"),true); return; }
        FAIMoveRequest Request(State.Target);
        Request.SetNavigationFilter(Controller->GetDefaultNavigationFilterClass());
        Request.SetAcceptanceRadius(FMath::Max(10.f,Combat->StrikeRange-25.f));
        Request.SetReachTestIncludesAgentRadius(false);
        Request.SetReachTestIncludesGoalRadius(false);
        Request.SetAllowPartialPath(false);
        const auto Result=Controller->MoveTo(Request).Code;
        if (Result!=EPathFollowingRequestResult::RequestSuccessful)
        { FinishFight(OwnerComp,TEXT("PathUnavailable"),true); return; }
        bFightMoveRequested=true;
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
    bStrikeStarted=false;
    StrikeFinishAt=0;
    bReturningToOrigin=false;
    FightPunchId.Invalidate();
    bFightMoveRequested=false;
    Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}
