#include "AI/BehaviorTree/BTTask_DeliveryExecuteNpcAction.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "Inventory/Items/DeliveryHandheldItem.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Conversation/DeliveryNpcConversationTrace.h"
#include "AIController.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense_Sight.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"

namespace
{
// NPC 动作使用单独的朝向优先级，不覆盖导航或其他玩法设置的朝向
const EAIFocusPriority::Type ResidentActionFocus = EAIFocusPriority::LastFocusPriority + 1;

// 同伴的踱步是否仍是当前这一次
bool IsCurrentPace(const ADeliveryCompanionCharacter* Npc, const AAIController* AI, const FDeliveryNpcPublicState& Expected)
{
    if (!IsValid(Npc) || Npc->IsActorBeingDestroyed() || !IsValid(AI) || AI->GetPawn() != Npc || Npc->GetController() != AI)
        return false;
    const auto Current = Npc->Agent->GetPublicState();
    return Current.ActionId == Expected.ActionId && Current.PlanRevision == Expected.PlanRevision &&
           Current.CompanionCommandRevision == Expected.CompanionCommandRevision &&
           Current.Status == EDeliveryNpcActionStatus::Running && Npc->CanContinuePace(Current);
}
} // namespace

// 每个 NPC 各用一个节点实例，需要逐帧更新
UBTTask_DeliveryExecuteNpcAction::UBTTask_DeliveryExecuteNpcAction()
{
    NodeName = TEXT("Delivery 执行 NPC 动作");
    bCreateNodeInstance = true;
    bNotifyTick = true;
    bNotifyTaskFinished = true;
}

// 交谈流程第 16 步，服务器：向 Agent 领取已采纳的动作，交给角色启动，再按动作类型发起移动
EBTNodeResult::Type UBTTask_DeliveryExecuteNpcAction::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
    ReleaseFocus();
    bKeepsCompanionMovement = false;
    bStrikeStarted = false;
    StrikeFinishAt = 0;
    bReturningToOrigin = false;
    FightPunchId.Invalidate();
    bFightMoveRequested = false;
    NextFleePathUpdate = 0;
    FleeTravelled = 0;
    bReportedFlee = false;

    // 领取动作，Agent 会再检查一次目标和期限
    Controller = OwnerComp.GetAIOwner();
    APawn* Pawn = Controller.IsValid() ? Controller->GetPawn() : nullptr;
    Agent = Pawn ? Pawn->FindComponentByClass<UDeliveryNpcAgentComponent>() : nullptr;
    FDeliveryNpcPublicState Action;
    if (!Agent.IsValid() || !Agent->StartAction(Action))
        return EBTNodeResult::Failed;
    ActionId = Action.ActionId;
    FinishAt = Pawn->GetWorld()->GetTimeSeconds() + Action.Duration;
    const TWeakObjectPtr<AAIController> StartedController = Controller;
    const TWeakObjectPtr<UDeliveryNpcAgentComponent> StartedAgent = Agent;
    const TWeakObjectPtr<ADeliveryCompanionCharacter> ItemObserver = Cast<ADeliveryCompanionCharacter>(Pawn);
    const bool bCompanionPace = Action.Action == EDeliveryNpcAction::Pace && ItemObserver.IsValid();
    const TWeakObjectPtr<ADeliveryHandheldItem> ObservedItem = Cast<ADeliveryHandheldItem>(Action.Target);
    const bool bItemFocus = Action.Action == EDeliveryNpcAction::LookAt && ItemObserver.IsValid() &&
                            DeliveryHandheldTransfer::IsOrdinaryItem(ObservedItem.Get());
    bKeepsCompanionMovement = Agent->CanContinueCompanionMovement();
    if (!bKeepsCompanionMovement)
        Controller->StopMovement();
    if (bCompanionPace && !IsCurrentPace(ItemObserver.Get(), StartedController.Get(), Action))
    {
        if (StartedAgent.IsValid())
            StartedAgent->TryFinishAction(Action.ActionId, EDeliveryNpcActionStatus::Failed, TEXT("PaceStayUnavailable"));
        return EBTNodeResult::Failed;
    }
    if (bItemFocus)
    {
        // 停导航会同步广播，挂 live 焦点前重查同一动作及当前实见，不能用旧目标覆盖新动作
        const bool bSameAction = Agent.IsValid() && Agent->HasActiveAction() &&
                                 Agent->GetPublicState().ActionId == Action.ActionId &&
                                 Agent->GetPublicState().PlanRevision == Action.PlanRevision;
        if (!bSameAction || !Controller.IsValid() || !ItemObserver.IsValid() || ItemObserver->IsActorBeingDestroyed() ||
            Controller->GetPawn() != ItemObserver.Get() || !ObservedItem.IsValid() ||
            Agent->GetPublicState().Status != EDeliveryNpcActionStatus::Running ||
            Agent->GetPublicState().Target != ObservedItem.Get() ||
            !ItemObserver->Companion->CanUseObservedItemTarget(ObservedItem.Get()))
        {
            if (bSameAction)
                Agent->TryFinishAction(Action.ActionId, EDeliveryNpcActionStatus::Failed, TEXT("ItemTargetNoLongerVisible"));
            return EBTNodeResult::Failed;
        }
    }
    // 持续安排保留身体朝向，聊天目标由原交谈动画驱动头部注视
    if ((!bKeepsCompanionMovement && Action.Action == EDeliveryNpcAction::LookAt) ||
        Action.Action == EDeliveryNpcAction::StrikeOnce || Action.Action == EDeliveryNpcAction::Fight)
    {
        FocusTarget = Action.Target;
        Controller->SetFocus(Action.Target, ResidentActionFocus);
    }

    // 角色启动动作，再按类型发起寻路：MoveTo、StrikeOnce 靠近、Pace 和 MoveAside 走到落点、Flee 跑开
    if (auto* Resident = Cast<ADeliveryNpcCharacter>(Pawn))
    {
        if (Action.Action == EDeliveryNpcAction::StrikeOnce && Action.Duration < UDeliveryNpcCombatComponent::StrikeWindup)
        {
            Agent->TryFinishAction(ActionId, EDeliveryNpcActionStatus::Failed, TEXT("StrikeDurationTooShort"));
            return EBTNodeResult::Failed;
        }
        if (!Resident->ExecuteAction(Action) ||
            (bCompanionPace && !IsCurrentPace(ItemObserver.Get(), StartedController.Get(), Action)))
        {
            if (StartedAgent.IsValid())
                StartedAgent->TryFinishAction(Action.ActionId, EDeliveryNpcActionStatus::Failed, TEXT("StartRejected"));
            return EBTNodeResult::Failed;
        }
        if (DeliveryNpc::IsCompanionCommand(Action.Action))
        {
            // 这里只确认安排已接收，实际跑腿、家居和跟随由持续分支执行
            const auto* Companion = ItemObserver.IsValid() ? ItemObserver->Companion.Get() : nullptr;
            FName Reason = TEXT("CompanionModeChanged");
            if (Action.Action == EDeliveryNpcAction::JoinAssist)
                Reason = TEXT("AssistArrangementAccepted");
            else if (Action.Action == EDeliveryNpcAction::StopAssist)
                Reason = TEXT("AssistStoppedByOwner");
            else if (Action.Action == EDeliveryNpcAction::HomeAction)
                Reason = TEXT("HomeArrangementAccepted");
            else if (Action.Action == EDeliveryNpcAction::Follow && Companion &&
                     Companion->GetCommandRevision() == Action.CompanionCommandRevision)
                Reason = TEXT("CompanionFollowContinued");
            else if (DeliveryNpc::IsCompanionErrand(Action.Action))
                Reason = Companion && Companion->GetCommandRevision() == Action.CompanionCommandRevision
                             ? TEXT("CompanionErrandContinued")
                             : TEXT("CompanionErrandAccepted");
            if (StartedAgent.IsValid())
                StartedAgent->TryFinishAction(Action.ActionId, EDeliveryNpcActionStatus::Succeeded, Reason);
            return EBTNodeResult::Succeeded;
        }
        if (Action.Action == EDeliveryNpcAction::MoveTo)
        {
            const auto Result = Controller->MoveToActor(Action.Target, ADeliveryNpcCharacter::MoveAcceptanceRadius, true, true,
                                                        true, nullptr, false);
            if (Result == EPathFollowingRequestResult::Failed)
            {
                Agent->TryFinishAction(ActionId, EDeliveryNpcActionStatus::Failed, TEXT("PathUnavailable"));
                return EBTNodeResult::Failed;
            }
        }
        else if (Action.Action == EDeliveryNpcAction::StrikeOnce)
        {
            bStrikeStarted = Resident->Combat->GetStrikeStartTime(ActionId) >= 0;
            if (bStrikeStarted)
                StrikeFinishAt =
                    FMath::Min(FinishAt, Pawn->GetWorld()->GetTimeSeconds() + UDeliveryNpcCombatComponent::StrikeDuration);
            else
            {
                // 攻击按两人中心距离停步，不能再把双方胶囊半径加进到达距离
                FAIMoveRequest Request(Action.Target);
                Request.SetNavigationFilter(Controller->GetDefaultNavigationFilterClass());
                Request.SetAcceptanceRadius(FMath::Max(10.f, Resident->Combat->GetStrikeApproachDistance(Action.Target) - 1.f));
                Request.SetReachTestIncludesAgentRadius(false);
                Request.SetReachTestIncludesGoalRadius(false);
                Request.SetAllowPartialPath(false);
                if (Controller->MoveTo(Request).Code == EPathFollowingRequestResult::Failed)
                {
                    Agent->TryFinishAction(ActionId, EDeliveryNpcActionStatus::Failed, TEXT("PathUnavailable"));
                    return EBTNodeResult::Failed;
                }
            }
        }
        else if (Action.Action == EDeliveryNpcAction::Pace || Action.Action == EDeliveryNpcAction::MoveAside)
        {
            WalkOrigin = Resident->GetNavAgentLocation();
            const bool bFound = Action.Action == EDeliveryNpcAction::Pace
                                    ? Resident->FindPaceDestination(WalkDestination)
                                    : Resident->FindSideStepDestination(Action.Target, WalkDestination);
            if (!bFound || !StartNearbyMove(WalkDestination, bCompanionPace ? &Action : nullptr))
            {
                if (StartedAgent.IsValid())
                    StartedAgent->TryFinishAction(Action.ActionId, EDeliveryNpcActionStatus::Failed,
                                                  TEXT("NearbyPathUnavailable"));
                return EBTNodeResult::Failed;
            }
        }
        else if (Action.Action == EDeliveryNpcAction::Flee)
        {
            WalkOrigin = Resident->GetNavAgentLocation();
            PreviousFleePosition = WalkOrigin;
            PreviousThreatPosition = Action.Target->GetActorLocation();
            // 下一帧先检查受击姿势，再朝导航方向跑，逃跑时不盯着身后的威胁转身
        }
    }
    return EBTNodeResult::InProgress;
}

// 发起一次不允许部分路径的短距离寻路
bool UBTTask_DeliveryExecuteNpcAction::StartNearbyMove(const FVector& Destination, const FDeliveryNpcPublicState* CompanionPace)
{
    if (!Controller.IsValid())
        return false;
    const TWeakObjectPtr<AAIController> AI = Controller;
    const TWeakObjectPtr<ADeliveryCompanionCharacter> Pacer =
        CompanionPace ? Cast<ADeliveryCompanionCharacter>(AI->GetPawn()) : nullptr;
    FAIMoveRequest Request(Destination);
    Request.SetAcceptanceRadius(8.f);
    Request.SetReachTestIncludesAgentRadius(false);
    Request.SetReachTestIncludesGoalRadius(false);
    Request.SetAllowPartialPath(false);
    const auto Move = AI->MoveTo(Request);
    if (CompanionPace &&
        (ActionId != CompanionPace->ActionId || Controller != AI || !IsCurrentPace(Pacer.Get(), AI.Get(), *CompanionPace)))
    {
        // MoveTo 可以同步回调，旧动作只能取消自己刚交给寻路的编号
        auto* Path = AI.IsValid() ? AI->GetPathFollowingComponent() : nullptr;
        if (Path && Move.MoveId.IsValid() && Path->GetCurrentRequestId() == Move.MoveId)
            Path->AbortMove(*AI.Get(), FPathFollowingResultFlags::UserAbort, Move.MoveId);
        return false;
    }
    return Move.Code != EPathFollowingRequestResult::Failed;
}

// 每帧检查动作是否仍在执行，按动作类型推进，超时后结束
void UBTTask_DeliveryExecuteNpcAction::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
    if (!Agent.IsValid())
    {
        FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
        return;
    }

    // 动作已被替换或结束时退出
    const auto State = Agent->GetPublicState();
    if (State.ActionId != ActionId || State.Status != EDeliveryNpcActionStatus::Running)
    {
        if (State.ActionId.IsValid() && State.ActionId != ActionId && State.Status == EDeliveryNpcActionStatus::Accepted &&
            Controller.IsValid() && Controller.Get() == OwnerComp.GetAIOwner() && Controller->GetPawn() == Agent->GetOwner())
        {
            // 模型可在同一帧换动作，黑板仍为真时由正在运行的节点领取新动作
            // Agent 已经停止旧动作，这里只收回该节点的焦点，不再取消移动或结束旧编号
            ReleaseFocus(true);
            ActionId.Invalidate();
            const auto Result = ExecuteTask(OwnerComp, NodeMemory);
            if (Result != EBTNodeResult::InProgress)
                FinishLatentTask(OwnerComp, Result);
            return;
        }
        FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
        return;
    }

    // 交战和逃跑交给各自的函数
    if (State.Action == EDeliveryNpcAction::Fight)
    {
        TickFight(OwnerComp, Controller.IsValid() ? Cast<ADeliveryNpcCharacter>(Controller->GetPawn()) : nullptr, State);
        return;
    }
    if (State.Action == EDeliveryNpcAction::Flee)
    {
        TickFlee(OwnerComp, Controller.IsValid() ? Cast<ADeliveryNpcCharacter>(Controller->GetPawn()) : nullptr, State);
        return;
    }

    // 起身：等起身动作完成
    if (State.Action == EDeliveryNpcAction::GetUp)
    {
        const auto* Resident = Controller.IsValid() ? Cast<ADeliveryNpcCharacter>(Controller->GetPawn()) : nullptr;
        if (Resident && Resident->Combat->IsDeliberatelyProne() && Resident->Combat->IsRecovering())
            return;
        const bool bStanding = IsValid(Resident) && !Resident->IsKnockedDown();
        const bool bFinished =
            Agent->TryFinishAction(ActionId, bStanding ? EDeliveryNpcActionStatus::Succeeded : EDeliveryNpcActionStatus::Failed,
                                   bStanding ? NAME_None : FName(TEXT("DeliberateRecoveryInterrupted")));
        FinishLatentTask(OwnerComp, bStanding && bFinished ? EBTNodeResult::Succeeded : EBTNodeResult::Failed);
        return;
    }

    // 踱步和让开：走到落点后结束，踱步会走回原处
    if (State.Action == EDeliveryNpcAction::Pace || State.Action == EDeliveryNpcAction::MoveAside)
    {
        auto* Npc = Controller.IsValid() ? Cast<ADeliveryNpcCharacter>(Controller->GetPawn()) : nullptr;
        const auto* Companion = Cast<ADeliveryCompanionCharacter>(Npc);
        const bool bCompanionPace = State.Action == EDeliveryNpcAction::Pace && Companion;
        const bool bTimedOut = OwnerComp.GetWorld()->GetTimeSeconds() >= FinishAt;
        const bool bStoppedAtPaceReturn =
            bCompanionPace && bReturningToOrigin && Npc->GetVelocity().SizeSquared2D() <= FMath::Square(5.f);
        FName Failure;
        if (!Npc || Npc->IsKnockedDown())
            Failure = TEXT("WalkerUnavailable");
        else if (bCompanionPace && !IsCurrentPace(Companion, Controller.Get(), State))
            Failure = TEXT("PaceStayUnavailable");
        else if (State.Action == EDeliveryNpcAction::MoveAside && !IsValid(State.Target))
            Failure = TEXT("StepAsideTargetLost");
        else if (bTimedOut && !bCompanionPace)
            Failure = TEXT("NearbyMoveTimeout");
        else if (Controller->GetMoveStatus() == EPathFollowingStatus::Idle || (bTimedOut && bStoppedAtPaceReturn))
        {
            const FVector Feet = Npc->GetNavAgentLocation();
            const bool bReached = FVector::Dist2D(Feet, WalkDestination) <= 12 && FMath::Abs(Feet.Z - WalkDestination.Z) <= 60;
            if (!bReached)
                Failure = TEXT("NearbyPathInterrupted");
            else if (bCompanionPace && bReturningToOrigin && !bStoppedAtPaceReturn)
            {
                if (!bTimedOut)
                    return;
                Failure = TEXT("NearbyMoveTimeout");
            }
            else if (State.Action == EDeliveryNpcAction::Pace && !bReturningToOrigin)
            {
                if (bTimedOut)
                    Failure = TEXT("NearbyMoveTimeout");
                else
                {
                    // 返回本次记录的起点，让一次踱步停在附近
                    bReturningToOrigin = true;
                    WalkDestination = WalkOrigin;
                    const auto StartedAgent = Agent;
                    const bool bStarted = StartNearbyMove(WalkDestination, bCompanionPace ? &State : nullptr);
                    if (ActionId != State.ActionId || Agent != StartedAgent)
                        return;
                    if (!bStarted)
                        Failure = TEXT("PaceReturnBlocked");
                }
            }
            else
            {
                Agent->TryFinishAction(State.ActionId, EDeliveryNpcActionStatus::Succeeded, NAME_None);
                if (ActionId == State.ActionId)
                    FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
                return;
            }
        }
        else if (bTimedOut)
            Failure = TEXT("NearbyMoveTimeout");
        if (!Failure.IsNone())
        {
            Agent->TryFinishAction(State.ActionId, EDeliveryNpcActionStatus::Failed, Failure);
            if (ActionId == State.ActionId)
                FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
        }
        return;
    }

    // 单次出拳：靠近，够得着就出拳，等结果
    if (State.Action == EDeliveryNpcAction::StrikeOnce)
    {
        auto* Npc = Controller.IsValid() ? Cast<ADeliveryNpcCharacter>(Controller->GetPawn()) : nullptr;
        const double Now = OwnerComp.GetWorld()->GetTimeSeconds();
        FName Failure;
        if (!Npc || Npc->IsKnockedDown())
            Failure = TEXT("AttackerUnavailable");
        else if (bStrikeStarted)
        {
            if (Npc->Combat->GetStrikeStartTime(ActionId) < 0)
                Failure = TEXT("StrikeCancelled");
            else if (Now >= StrikeFinishAt)
            {
                bool bHit = false;
                const bool bFinished = Npc->Combat->GetStrikeResult(ActionId, bHit);
                Agent->TryFinishAction(ActionId,
                                       bFinished && bHit ? EDeliveryNpcActionStatus::Succeeded : EDeliveryNpcActionStatus::Failed,
                                       bFinished && bHit ? NAME_None : FName(TEXT("StrikeMissed")));
                FinishLatentTask(OwnerComp, bFinished && bHit ? EBTNodeResult::Succeeded : EBTNodeResult::Failed);
                return;
            }
        }
        else if (Now + UDeliveryNpcCombatComponent::StrikeWindup > FinishAt)
            Failure = TEXT("ApproachTimeout");
        else if (!Npc->Combat->CanContinueStrikeApproach(State.Target))
            Failure = TEXT("StrikeTargetUnavailable");
        else if (Npc->Combat->CanStrike(State.Target))
        {
            Controller->StopMovement();
            bStrikeStarted = Npc->RequestStrike(State.Target, ActionId);
            if (bStrikeStarted)
                StrikeFinishAt = FMath::Min(FinishAt, Now + UDeliveryNpcCombatComponent::StrikeDuration);
            else
                Failure = TEXT("StrikeStartRejected");
        }
        else if (Controller->GetMoveStatus() == EPathFollowingStatus::Idle)
            Failure = TEXT("PathInterrupted");
        if (!Failure.IsNone())
        {
            Agent->TryFinishAction(ActionId, EDeliveryNpcActionStatus::Failed, Failure);
            FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
        }
        return;
    }

    // 同伴的持续安排：对白结束后交还原执行分支
    if (bKeepsCompanionMovement)
    {
        // 原分支被对白临时替换时，由同一个 BT 执行者继续推进实际安排
        auto* Companion = Controller.IsValid() && Controller->GetPawn()
                              ? Controller->GetPawn()->FindComponentByClass<UDeliveryCompanionComponent>()
                              : nullptr;
        if (Companion)
        {
            if (Companion->HasActiveErrand())
                Companion->UpdateErrand();
            else
                Companion->UpdateFollow();
        }
        if (!Agent.IsValid() || Agent->GetPublicState().ActionId != ActionId ||
            Agent->GetPublicState().Status != EDeliveryNpcActionStatus::Running)
            return;
    }
    if (State.Action == EDeliveryNpcAction::MoveTo && Controller.IsValid() &&
        Controller->GetMoveStatus() == EPathFollowingStatus::Idle)
    {
        const auto* Npc = Cast<ADeliveryNpcCharacter>(Controller->GetPawn());
        const bool bNear = Npc && Npc->HasReachedMoveTarget(State.Target);
        Agent->TryFinishAction(ActionId, bNear ? EDeliveryNpcActionStatus::Succeeded : EDeliveryNpcActionStatus::Failed,
                               bNear ? NAME_None : FName(TEXT("PathInterrupted")));
        FinishLatentTask(OwnerComp, bNear ? EBTNodeResult::Succeeded : EBTNodeResult::Failed);
        return;
    }

    // 到达时长上限，按成功结束
    if (OwnerComp.GetWorld()->GetTimeSeconds() >= FinishAt)
    {
        const bool bTimedOut = State.Action == EDeliveryNpcAction::MoveTo;
        const bool bFinished =
            Agent->TryFinishAction(ActionId, bTimedOut ? EDeliveryNpcActionStatus::Failed : EDeliveryNpcActionStatus::Succeeded,
                                   bTimedOut ? FName(TEXT("MoveTimeout")) : NAME_None);
        FinishLatentTask(OwnerComp, bFinished && !bTimedOut ? EBTNodeResult::Succeeded : EBTNodeResult::Failed);
    }
}

// 用本次动作编号向 Agent 报告交战结果，结束行为树任务
void UBTTask_DeliveryExecuteNpcAction::FinishFight(UBehaviorTreeComponent& OwnerComp, FName Reason, bool bFailed)
{
    if (Agent.IsValid())
        Agent->TryFinishAction(ActionId, bFailed ? EDeliveryNpcActionStatus::Failed : EDeliveryNpcActionStatus::Succeeded,
                               Reason);
    FinishLatentTask(OwnerComp, bFailed ? EBTNodeResult::Failed : EBTNodeResult::Succeeded);
}

// 交战流程第 17 步，服务器每帧：判断是否停手，够得着就出拳，够不着就追
void UBTTask_DeliveryExecuteNpcAction::TickFight(UBehaviorTreeComponent& OwnerComp, ADeliveryNpcCharacter* Npc,
                                                 const FDeliveryNpcPublicState& State)
{
    if (!Npc || !Controller.IsValid())
    {
        FinishFight(OwnerComp, TEXT("AttackerUnavailable"), true);
        return;
    }
    if (State.Origin == EDeliveryNpcActionOrigin::GameplayRule &&
        !Npc->CanExecuteGameplayRule(State.Action, State.Target, State.RuleRevision))
    {
        FinishFight(OwnerComp, TEXT("GameplayRuleExpired"), true);
        return;
    }

    // 对方倒地、跑远或时间到时结束
    auto* Combat = Npc->Combat.Get();
    if (State.Origin == EDeliveryNpcActionOrigin::Model)
        FinishAt = OwnerComp.GetWorld()->GetTimeSeconds() + Agent->GetActionTimeRemaining();
    const FName Stop = Combat->GetFightStopReason(ActionId);
    if (!Stop.IsNone())
    {
        const bool bFinished = Stop == TEXT("TargetDown") || Stop == TEXT("TargetEscaped") || Stop == TEXT("FightTimeLimit");
        FinishFight(OwnerComp, Stop, !bFinished);
        return;
    }

    // 自己正在受击后仰：停下
    if (Combat->IsReactingToHit())
    {
        Controller->StopMovement();
        bFightMoveRequested = false;
        return;
    }
    const double Now = OwnerComp.GetWorld()->GetTimeSeconds();

    // 上一拳还在进行：等收拳和判定结果
    if (bStrikeStarted)
    {
        bool bHit = false;
        const bool bJudged = Combat->GetStrikeResult(FightPunchId, bHit);
        // 被轻拳打断的挥拳也有未命中的结果，后仰结束后继续当前交战
        if (Combat->GetStrikeStartTime(ActionId) < 0 && !bJudged)
        {
            FinishFight(OwnerComp, TEXT("StrikeCancelled"), true);
            return;
        }
        if (Combat->GetStrikeStartTime(ActionId) >= 0 && Now < StrikeFinishAt)
            return;
        if (!bJudged)
        {
            FinishFight(OwnerComp, TEXT("StrikeResultMissing"), true);
            return;
        }
        bStrikeStarted = false;
        FightPunchId.Invalidate();
    }
    if (State.Origin == EDeliveryNpcActionOrigin::GameplayRule && Combat->GetFightPunchCount() >= Combat->GetFightPunchLimit())
    {
        FinishFight(OwnerComp, TEXT("FightPunchLimit"), false);
        return;
    }

    // 够得着：停下并出拳
    if (Combat->CanStrike(State.Target))
    {
        Controller->StopMovement();
        bFightMoveRequested = false;
        // 剩余时间不够蓄力时先保持距离，仍可接收正在生成的决定
        if (!Combat->IsReadyToStrike() || Now + UDeliveryNpcCombatComponent::StrikeWindup > FinishAt)
            return;
        FightPunchId = FGuid::NewGuid();
        bStrikeStarted = Npc->RequestFightStrike(State.Target, ActionId, FightPunchId);
        if (!bStrikeStarted)
        {
            FinishFight(OwnerComp, TEXT("StrikeStartRejected"), true);
            return;
        }
        StrikeFinishAt = Now + UDeliveryNpcCombatComponent::StrikeDuration;
        return;
    }

    // 够不着：朝对方当前位置追过去
    if (Controller->GetMoveStatus() == EPathFollowingStatus::Idle)
    {
        // 对方在收拳时跑开，使用它当前的位置重新追赶；没有完整路径时本次请求会失败
        FAIMoveRequest Request(State.Target);
        Request.SetNavigationFilter(Controller->GetDefaultNavigationFilterClass());
        Request.SetAcceptanceRadius(FMath::Max(10.f, Combat->GetStrikeApproachDistance(State.Target) - 1.f));
        Request.SetReachTestIncludesAgentRadius(false);
        Request.SetReachTestIncludesGoalRadius(false);
        Request.SetAllowPartialPath(false);
        const auto Result = Controller->MoveTo(Request).Code;
        if (Result == EPathFollowingRequestResult::Failed)
        {
            FinishFight(OwnerComp, TEXT("PathUnavailable"), true);
            return;
        }
        bFightMoveRequested = true;
    }
}

// 服务器每帧：逃跑，按间隔重新找远离威胁的落点，跑够距离或时间后结束
void UBTTask_DeliveryExecuteNpcAction::TickFlee(UBehaviorTreeComponent& OwnerComp, ADeliveryNpcCharacter* Npc,
                                                const FDeliveryNpcPublicState& State)
{
    const auto Finish = [&](FName Reason, bool bFailed)
    {
        Agent->TryFinishAction(ActionId, bFailed ? EDeliveryNpcActionStatus::Failed : EDeliveryNpcActionStatus::Succeeded,
                               Reason);
        FinishLatentTask(OwnerComp, bFailed ? EBTNodeResult::Failed : EBTNodeResult::Succeeded);
    };
    if (!Npc || !Controller.IsValid() || Npc->IsKnockedDown())
    {
        Finish(TEXT("RunnerUnavailable"), true);
        return;
    }
    if (!IsValid(State.Target) || State.Target->GetWorld() != Npc->GetWorld())
    {
        Finish(TEXT("FleeTargetLost"), true);
        return;
    }

    // 累计跑过的距离
    const double Now = OwnerComp.GetWorld()->GetTimeSeconds();
    const FVector Position = Npc->GetNavAgentLocation();
    if (State.Origin == EDeliveryNpcActionOrigin::Model)
        FinishAt = Now + Agent->GetActionTimeRemaining();
    FleeTravelled += FVector::Dist2D(Position, PreviousFleePosition);
    PreviousFleePosition = Position;
    const float Remaining = FMath::Clamp(Npc->FleeTravelLimit, 200.f, 1200.f) - FleeTravelled;
    const bool bFarEnough =
        FVector::Dist2D(Position, State.Target->GetActorLocation()) >= FMath::Clamp(Npc->FleeSafeDistance, 200.f, 900.f);
    const bool bEnding = Now >= FinishAt || Remaining < 60 || bFarEnough;

    // 跑开 35 cm 后记下逃跑事件
    // 已经跑动并被对方看见后才记下逃跑事实，动作刚被选中时还不能声称跑开了
    if (!bReportedFlee && FVector::Dist2D(WalkOrigin, Position) >= 35 && (Now >= NextFleePathUpdate || bEnding))
    {
        if (auto* Other = Cast<ADeliveryNpcCharacter>(State.Target))
        {
            auto* OtherAI = Cast<AAIController>(Other->GetController());
            auto* Senses = OtherAI ? OtherAI->GetPerceptionComponent() : nullptr;
            TArray<AActor*> Seen;
            if (Senses)
                Senses->GetCurrentlyPerceivedActors(UAISense_Sight::StaticClass(), Seen);
            if (Seen.Contains(Npc) && DeliveryNpcConversationTrace::HasClearPath(Other, Npc, FVector(0, 0, 40)))
            {
                Other->ObserveFact(TEXT("FleeStarted"), Npc, TEXT("这个人正在远离我跑开"), EDeliveryNpcEvidenceSource::Seen);
                bReportedFlee = true;
            }
        }
    }

    // 结束条件：时间到、路程用完、已经足够远
    if (Now >= FinishAt)
    {
        Finish(TEXT("FleeTimeLimit"), false);
        return;
    }
    if (Remaining < 60)
    {
        Finish(TEXT("FleeTravelLimit"), false);
        return;
    }
    if (bFarEnough)
    {
        Finish(TEXT("FleeReachedDistance"), false);
        return;
    }
    if (Npc->Combat->IsReactingToHit())
    {
        Controller->StopMovement();
        return;
    }

    // 每 0.6 秒检查一次，对方移动后重新找落点
    if (Now < NextFleePathUpdate)
        return;
    NextFleePathUpdate = Now + .6;
    const bool bThreatMoved =
        FVector::DistSquared2D(PreviousThreatPosition, State.Target->GetActorLocation()) > FMath::Square(80.f);
    if (Controller->GetMoveStatus() != EPathFollowingStatus::Idle && !bThreatMoved)
        return;
    if (!Npc->FindFleeDestination(State.Target, Remaining, WalkDestination) || !StartNearbyMove(WalkDestination))
    {
        Finish(TEXT("FleePathUnavailable"), true);
        return;
    }
    PreviousThreatPosition = State.Target->GetActorLocation();
}

// 行为树中止节点时，按取消结束当前动作
EBTNodeResult::Type UBTTask_DeliveryExecuteNpcAction::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
    if (Agent.IsValid())
        Agent->TryFinishAction(ActionId, EDeliveryNpcActionStatus::Cancelled, TEXT("BehaviorTreeAbort"));
    ReleaseFocus();
    return EBTNodeResult::Aborted;
}

// 清除本节点设置的注视目标
void UBTTask_DeliveryExecuteNpcAction::ReleaseFocus(bool bReplacingOwnAction)
{
    if (Controller.IsValid() && ActionId.IsValid() && FocusTarget.IsValid() &&
        (bReplacingOwnAction || !Agent.IsValid() || Agent->GetPublicState().ActionId == ActionId) &&
        Controller->GetFocusActorForPriority(ResidentActionFocus) == FocusTarget.Get())
        Controller->ClearFocus(ResidentActionFocus);
    FocusTarget.Reset();
}

// 节点结束：清除注视，停止本动作发起的移动
void UBTTask_DeliveryExecuteNpcAction::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory,
                                                      EBTNodeResult::Type TaskResult)
{
    bool bCompanionPace = false;
    if (Agent.IsValid())
    {
        const auto State = Agent->GetPublicState();
        bCompanionPace = State.ActionId == ActionId && State.Action == EDeliveryNpcAction::Pace &&
                         Cast<ADeliveryCompanionCharacter>(Agent->GetOwner());
        Agent->TryFinishAction(ActionId, EDeliveryNpcActionStatus::Cancelled, TEXT("ExecutorStopped"));
    }
    ReleaseFocus();
    if (Controller.IsValid())
    {
        // 保留被动短动作、重复委托和已结算踱步之后的持续导航
        if (!bKeepsCompanionMovement && !bCompanionPace && Agent.IsValid() && Agent->GetPublicState().ActionId == ActionId)
            Controller->StopMovement();
        if (auto* Resident = Cast<ADeliveryNpcCharacter>(Controller->GetPawn()))
            Resident->StopAction(ActionId);
    }
    Agent.Reset();
    Controller.Reset();
    ActionId.Invalidate();
    bStrikeStarted = false;
    StrikeFinishAt = 0;
    bReturningToOrigin = false;
    FightPunchId.Invalidate();
    bFightMoveRequested = false;
    NextFleePathUpdate = 0;
    FleeTravelled = 0;
    bReportedFlee = false;
    Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}
