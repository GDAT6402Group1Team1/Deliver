#include "AI/Characters/DeliveryNpcAIController.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISense_Sight.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "Character/DeliveryCharacter.h"
#include "Engine/World.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BehaviorTree.h"
#include "Navigation/PathFollowingComponent.h"
#include "Engine/OverlapResult.h"

// ── 生命周期 ──
// 构造函数建好视觉和听觉感知；OnPossess 控制 NPC 后订阅感知通知并启动行为树
// Tick 每帧在服务器上调用 UpdateObstacleTraversal（越障）和 UpdateStandingSpace（站位）；OnUnPossess 和 EndPlay 停掉这两种移动

/// @brief 建好感知组件：视觉看 1200 cm、前方左右各 100 度，走出 1400 cm 才算看不见；听觉 900 cm
/// 感知记录 6 秒后过期，敌方、友方、中立都能感知到
ADeliveryNpcAIController::ADeliveryNpcAIController()
{
    PrimaryActorTick.bCanEverTick = true;
    auto* Senses = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("ResidentPerception"));
    SetPerceptionComponent(*Senses);
    Sight = CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("Sight"));
    Sight->SightRadius = 1200;
    Sight->LoseSightRadius = 1400;
    Sight->PeripheralVisionAngleDegrees = 100;
    Sight->SetMaxAge(6);
    Sight->DetectionByAffiliation.bDetectEnemies = true;
    Sight->DetectionByAffiliation.bDetectFriendlies = true;
    Sight->DetectionByAffiliation.bDetectNeutrals = true;
    Hearing = CreateDefaultSubobject<UAISenseConfig_Hearing>(TEXT("Hearing"));
    Hearing->HearingRange = 900;
    Hearing->SetMaxAge(6);
    Hearing->DetectionByAffiliation = Sight->DetectionByAffiliation;
    Senses->ConfigureSense(*Sight);
    Senses->ConfigureSense(*Hearing);
    Senses->SetDominantSense(Sight->GetSenseImplementation());
}

/// @brief 服务器每帧先调用 UpdateObstacleTraversal 处理越过矮障碍，再调用 UpdateStandingSpace 处理站位
void ADeliveryNpcAIController::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (HasAuthority())
    {
        UpdateObstacleTraversal();
        UpdateStandingSpace();
    }
}

// ── 站位 ──
// NPC 空闲时，UpdateStandingSpace 每 0.4 秒看一次身边，和别的 NPC 身体间隔小于 MinStandingGap 就挪开
// 挪到间隔至少 ComfortableStandingGap 的位置；挪动最多 3 秒，对方忙起来或者已经够远就停，StopSpacingMove 停止这次移动

/// @brief 停止站位用的移动，清掉 SpacingMoveId 和 SpacingNeighbor
void ADeliveryNpcAIController::StopSpacingMove()
{
    const FAIRequestID OldId = SpacingMoveId;
    SpacingMoveId = FAIRequestID::InvalidRequest;
    SpacingNeighbor.Reset();
    SpacingMoveEndsAt = 0;
    auto* Path = GetPathFollowingComponent();

    // 路径跟随组件当前的请求编号还是 OldId 才调用 AbortMove；已经被新动作换掉的移动不取消
    if (OldId.IsValid() && Path && Path->GetCurrentRequestId() == OldId)
        Path->AbortMove(*this, FPathFollowingResultFlags::UserAbort, OldId);
}

/// @brief 检查 Destination 周围放不放得下这位 NPC：半径加 MinStandingGap 的范围内没有别的可见 NPC 返回 true
bool ADeliveryNpcAIController::HasRoomToStand(const ADeliveryNpcCharacter* Npc, const FVector& Destination) const
{
    const float Gap = FMath::Clamp(MinStandingGap, 20.f, 150.f);
    TArray<FOverlapResult> Nearby;
    const FVector Center = Destination + FVector(0, 0, Npc->GetSimpleCollisionHalfHeight());
    GetWorld()->OverlapMultiByObjectType(Nearby, Center, FQuat::Identity, FCollisionObjectQueryParams(ECC_Pawn),
                                         FCollisionShape::MakeSphere(Npc->GetSimpleCollisionRadius() + Gap),
                                         FCollisionQueryParams(SCENE_QUERY_STAT(NpcStandingSpace), false, Npc));
    for (const auto& Hit : Nearby)
    {
        const auto* Other = Cast<ADeliveryNpcCharacter>(Hit.GetActor());
        if (IsValid(Other) && Other != Npc && !Other->IsHidden() &&
            FMath::Abs(Center.Z - Other->GetActorLocation().Z) <
                Npc->GetSimpleCollisionHalfHeight() + Other->GetSimpleCollisionHalfHeight() &&
            FVector::Dist2D(Center, Other->GetActorLocation()) <
                Npc->GetSimpleCollisionRadius() + Other->GetSimpleCollisionRadius() + Gap)
            return false;
    }
    return true;
}

/// @brief 空闲时和最近的 NPC 拉开距离；正在挪的话检查要不要停
/// bKeepDistanceWhenIdle 为 false、NPC 隐藏或者 CanAdjustStandingPosition 为 false（正在做别的事）时，调用 StopSpacingMove 并返回
void ADeliveryNpcAIController::UpdateStandingSpace()
{
    auto* Npc = Cast<ADeliveryNpcCharacter>(GetPawn());
    auto* Path = GetPathFollowingComponent();
    const double Now = GetWorld()->GetTimeSeconds();
    if (!bKeepDistanceWhenIdle || !Npc || !Path || Npc->IsHidden() || !Npc->CanAdjustStandingPosition())
    {
        StopSpacingMove();
        return;
    }
    const float StartGap = FMath::Clamp(MinStandingGap, 20.f, 150.f);
    const float StopGap = FMath::Max(StartGap + 30.f, FMath::Clamp(ComfortableStandingGap, 40.f, 200.f));

    // SpacingMoveId 有效说明正在挪：移动结束、邻居没了、邻居开始做别的事、距离已经够远（StopGap 再留 8 cm 余量）或超过 SpacingMoveEndsAt，就停下
    // 停下后 1.2 秒内不再检查
    if (SpacingMoveId.IsValid())
    {
        const auto* Other = SpacingNeighbor.Get();
        const bool bFinished = Path->GetCurrentRequestId() != SpacingMoveId || GetMoveStatus() == EPathFollowingStatus::Idle;
        const bool bOtherBusy = Other && !Other->IsKnockedDown() && !Other->CanAdjustStandingPosition();
        const bool bFarEnough = Other && FVector::Dist2D(Npc->GetActorLocation(), Other->GetActorLocation()) >=
                                             Npc->GetSimpleCollisionRadius() + Other->GetSimpleCollisionRadius() + StopGap - 8.f;
        if (bFinished || !Other || bOtherBusy || bFarEnough || Now >= SpacingMoveEndsAt)
        {
            StopSpacingMove();
            NextSpacingCheck = Now + 1.2;
        }
        return;
    }

    // 没到检查时间、正在走路或者身体还在动，这一帧不检查；检查过后 0.4 秒再查下一次
    if (Now < NextSpacingCheck || GetMoveStatus() != EPathFollowingStatus::Idle ||
        Npc->GetVelocity().SizeSquared2D() > FMath::Square(20.f))
        return;
    NextSpacingCheck = Now + .4;

    // 在 StartGap 范围内找身体间隔最小的邻居：邻居要站着不动、没在做别的事、上下没错开，而且两人之间没有墙挡着
    TArray<FOverlapResult> Nearby;
    GetWorld()->OverlapMultiByObjectType(Nearby, Npc->GetActorLocation(), FQuat::Identity, FCollisionObjectQueryParams(ECC_Pawn),
                                         FCollisionShape::MakeSphere(Npc->GetSimpleCollisionRadius() + StartGap),
                                         FCollisionQueryParams(SCENE_QUERY_STAT(NpcStandingNeighbors), false, Npc));
    ADeliveryNpcCharacter* Neighbor = nullptr;
    float NearestGap = StartGap;
    for (const auto& Hit : Nearby)
    {
        auto* Other = Cast<ADeliveryNpcCharacter>(Hit.GetActor());
        if (!IsValid(Other) || Other == Npc || Other->IsHidden())
            continue;
        const auto& Action = Other->Agent->GetPublicState();

        // 邻居正对着这位 NPC 出拳或交战时直接返回，不挪开，要不要逃跑由模型决定
        if (Other->Agent->HasActiveAction() && Action.Target == Npc &&
            (Action.Action == EDeliveryNpcAction::Fight || Action.Action == EDeliveryNpcAction::StrikeOnce))
            return;
        if (!Other->IsKnockedDown() && !Other->CanAdjustStandingPosition())
            continue;
        const auto* OtherController = Cast<AAIController>(Other->GetController());
        if (Other->GetVelocity().SizeSquared2D() > FMath::Square(20.f) ||
            (OtherController && OtherController->GetMoveStatus() != EPathFollowingStatus::Idle))
            continue;
        const float Gap = FVector::Dist2D(Npc->GetActorLocation(), Other->GetActorLocation()) - Npc->GetSimpleCollisionRadius() -
                          Other->GetSimpleCollisionRadius();
        if (Gap >= NearestGap || FMath::Abs(Npc->GetActorLocation().Z - Other->GetActorLocation().Z) >=
                                     Npc->GetSimpleCollisionHalfHeight() + Other->GetSimpleCollisionHalfHeight())
            continue;
        FHitResult Obstacle;
        if (GetWorld()->LineTraceSingleByChannel(Obstacle, Npc->GetActorLocation(), Other->GetActorLocation(), ECC_Visibility,
                                                 FCollisionQueryParams(SCENE_QUERY_STAT(NpcStandingSight), false, Npc)) &&
            Obstacle.GetActor() != Other)
            continue;
        NearestGap = Gap;
        Neighbor = Other;
    }
    if (!Neighbor)
        return;

    // FindStandingSpace 算出离开邻居的新站位，HasRoomToStand 确认那里放得下，再调用 MoveTo；找不到或寻路失败，1.5 秒后再试
    // 寻路成功后记下 SpacingMoveId 和 SpacingNeighbor，最多挪 3 秒
    FVector Destination;
    if (!Npc->FindStandingSpace(Neighbor, StopGap, Destination) || !HasRoomToStand(Npc, Destination))
    {
        NextSpacingCheck = Now + 1.5;
        return;
    }
    FAIMoveRequest Request(Destination);
    Request.SetNavigationFilter(GetDefaultNavigationFilterClass());
    Request.SetAcceptanceRadius(8.f);
    Request.SetReachTestIncludesAgentRadius(false);
    Request.SetReachTestIncludesGoalRadius(false);
    Request.SetAllowPartialPath(false);
    const auto Result = MoveTo(Request);
    if (Result.Code == EPathFollowingRequestResult::RequestSuccessful)
    {
        SpacingMoveId = Result.MoveId;
        SpacingNeighbor = Neighbor;
        SpacingMoveEndsAt = Now + 3;
    }
    else
        NextSpacingCheck = Now + 1.5;
}

/// @brief 控制 NPC 时调用：服务器上订阅 OnTargetPerceptionUpdated，运行 NPC 配置的 BehaviorTree，再调用一次 RefreshBlackboard
void ADeliveryNpcAIController::OnPossess(APawn* InPawn)
{
    Super::OnPossess(InPawn);
    if (!HasAuthority())
        return;
    GetPerceptionComponent()->OnTargetPerceptionUpdated.AddUniqueDynamic(this, &ADeliveryNpcAIController::OnTargetSeenOrHeard);
    if (const auto* Resident = Cast<ADeliveryNpcCharacter>(InPawn))
        if (Resident->BehaviorTree)
            RunBehaviorTree(Resident->BehaviorTree);
    RefreshBlackboard();
}

/// @brief 不再控制 NPC 时调用：停止越障和站位移动，清掉受击朝向，取消感知订阅，再调用 Agent->CancelPlan 取消当前计划
void ADeliveryNpcAIController::OnUnPossess()
{
    StopObstacleTraversal();
    ObstacleRetreatAttempts.Reset();
    if (auto* Npc = Cast<ADeliveryNpcCharacter>(GetPawn()); Npc && Npc->Combat)
        Npc->Combat->ResetHitFacing();
    StopSpacingMove();
    GetPerceptionComponent()->OnTargetPerceptionUpdated.RemoveDynamic(this, &ADeliveryNpcAIController::OnTargetSeenOrHeard);
    if (auto* Resident = Cast<ADeliveryNpcCharacter>(GetPawn()))
        Resident->Agent->CancelPlan(TEXT("ControllerLost"));
    Super::OnUnPossess();
}

/// @brief 控制器结束时停止越障和站位移动
void ADeliveryNpcAIController::EndPlay(const EEndPlayReason::Type Reason)
{
    StopObstacleTraversal();
    ObstacleRetreatAttempts.Reset();
    StopSpacingMove();
    Super::EndPlay(Reason);
}

// ── 感知 ──
// UE 的感知组件看见或听见某个角色时调用 OnTargetSeenOrHeard，这里写成一条 FDeliveryNpcObservation 交给 Agent->ObserveEvent
// 看见写成 Perceived，看不见写成 LostSight；ObserveEvent 返回 true（这件事值得回应）时调用 WakeDecision

/// @brief 感知组件的回调：只处理视觉和听觉，听觉只处理听见，不处理听觉记录过期
/// 事件保存 45 秒，描述里写对方是谁、当时离多远；看不见时写明可能只是转头或被挡住
void ADeliveryNpcAIController::OnTargetSeenOrHeard(AActor* Target, FAIStimulus Stimulus)
{
    auto* Resident = Cast<ADeliveryNpcCharacter>(GetPawn());
    if (!Resident || !HasAuthority() || !IsValid(Target) || Target == Resident || !Stimulus.Type.IsValid())
        return;
    const bool bSight = Stimulus.Type == Sight->GetSenseID();
    const bool bHearing = Stimulus.Type == Hearing->GetSenseID();
    const bool bSensed = Stimulus.WasSuccessfullySensed();

    // 听觉记录过期不代表说话的人走了，所以听觉只处理 bSensed 为 true 的回调
    if ((!bSight && !bHearing) || (bHearing && !bSensed))
        return;

    FDeliveryNpcObservation Event;
    Event.Kind = bSensed ? TEXT("Perceived") : TEXT("LostSight");
    Event.Subject = Target;
    Event.Source = bHearing ? EDeliveryNpcEvidenceSource::Heard : EDeliveryNpcEvidenceSource::Seen;

    // 位置用 StimulusLocation：看不见的回调里它是最后一次看见的位置，不读 Target 当前的坐标
    Event.bHasObservedLocation = FAISystem::IsValidLocation(Stimulus.StimulusLocation);
    if (Event.bHasObservedLocation)
        Event.ObservedLocation = Stimulus.StimulusLocation;
    Event.Lifetime = 45;

    // 按对象类型写名字：NPC 用 DisplayName，摩托车写电瓶车，玩家角色写快递员，其他写行人
    const auto* OtherResident = Cast<ADeliveryNpcCharacter>(Target);
    const FString Subject = OtherResident                       ? OtherResident->DisplayName.ToString()
                            : Target->IsA<ADeliveryMotorbike>() ? TEXT("电瓶车")
                            : Target->IsA<ADeliveryCharacter>() ? TEXT("快递员")
                                                                : TEXT("行人");
    if (bSensed)
    {
        Event.Description =
            bSight ? FString::Printf(TEXT("看见附近的%s"), *Subject) : FString::Printf(TEXT("听见附近的%s发出声音"), *Subject);
        if (Event.bHasObservedLocation && FAISystem::IsValidLocation(Stimulus.ReceiverLocation))
            Event.Description += FString::Printf(TEXT("，当时距离约 %.0f 厘米"),
                                                 FVector::Dist(Stimulus.StimulusLocation, Stimulus.ReceiverLocation));
    }
    else
    {
        Event.Description = FString::Printf(TEXT("暂时看不见%s，可能只是转头或被遮挡，不能据此判断对方已经走远"), *Subject);
        if (Event.bHasObservedLocation)
            Event.Description += TEXT("；记录的位置是最后一次看见的位置");
    }
    if (Resident->Agent->ObserveEvent(Event))
        Resident->WakeDecision();
}

// ── 行为树 ──
// NPC 角色每帧调用 RefreshBlackboard，把 Agent 和 NPC 的状态写进黑板，行为树按黑板键选择分支

/// @brief 写黑板键：Unavailable、Listening、HasApprovedAction、HasCompanionErrand、HasNearbyItemSearch、ActionTarget、PlanRevision
/// 另外每 15 秒把视线里最多 8 个角色写成 VisibleNow 事件交给 Agent，模型请求还在等回复时不写
void ADeliveryNpcAIController::RefreshBlackboard()
{
    const auto* Resident = Cast<ADeliveryNpcCharacter>(GetPawn());
    auto* BB = GetBlackboardComponent();
    if (!Resident || !BB)
        return;

    // Unavailable：被打倒且不是故意躺下时为 true；Listening 键存在时写 ShouldPauseForConversation
    const auto State = Resident->Agent->GetPublicState();
    BB->SetValueAsBool(TEXT("Unavailable"), Resident->IsKnockedDown() && !Resident->Combat->IsDeliberatelyProne());
    if (BB->GetKeyID(TEXT("Listening")) != FBlackboard::InvalidKey)
        BB->SetValueAsBool(TEXT("Listening"), Resident->Agent->ShouldPauseForConversation());

    // HasApprovedAction 写 HasActiveAction；函数改过名字，但已有的黑板资产还用这个键名，所以键名不改
    BB->SetValueAsBool(TEXT("HasApprovedAction"), Resident->Agent->HasActiveAction());

    // 二狗的黑板有 HasCompanionErrand 键，普通 NPC 的黑板没有，所以先用 GetKeyID 查键在不在
    // 委托暂停时 HasActiveErrand 仍为 true，黑板不清掉这个委托
    if (BB->GetKeyID(TEXT("HasCompanionErrand")) != FBlackboard::InvalidKey)
    {
        const auto* Companion = Resident->FindComponentByClass<UDeliveryCompanionComponent>();
        BB->SetValueAsBool(TEXT("HasCompanionErrand"), Companion && Companion->HasActiveErrand());
        if (BB->GetKeyID(TEXT("HasNearbyItemSearch")) != FBlackboard::InvalidKey)
            BB->SetValueAsBool(TEXT("HasNearbyItemSearch"), Companion && Companion->HasNearbyItemSearch());
    }
    BB->SetValueAsObject(TEXT("ActionTarget"), State.Target);
    BB->SetValueAsInt(TEXT("PlanRevision"), State.PlanRevision);

    // 每 15 秒把视线里最多 8 个角色写成 VisibleNow 事件，让 NPC 记得谁还在眼前
    // 这里只调用 ObserveEvent，不调用 WakeDecision；要不要问模型由之后的交谈或别的事件决定
    const double Now = GetWorld()->GetTimeSeconds();
    if (Now >= NextSightRefresh && !Resident->Agent->IsDecisionPending())
    {
        NextSightRefresh = Now + 15;
        TArray<AActor*> Visible;
        GetPerceptionComponent()->GetCurrentlyPerceivedActors(UAISense_Sight::StaticClass(), Visible);
        for (int32 Index = 0; Index < FMath::Min(Visible.Num(), 8); ++Index)
        {
            auto* Target = Visible[Index];
            if (!IsValid(Target) || Target == Resident)
                continue;
            FDeliveryNpcObservation Event;
            Event.Kind = TEXT("VisibleNow");
            Event.Subject = Target;
            Event.Source = EDeliveryNpcEvidenceSource::Seen;
            Event.Lifetime = 45;
            Event.bHasObservedLocation = true;
            Event.ObservedLocation = Target->GetActorLocation();
            const auto* Other = Cast<ADeliveryNpcCharacter>(Target);
            Event.Description = Other ? FString::Printf(TEXT("%s 仍在视线中，距离约 %.0f 厘米"), *Other->DisplayName.ToString(),
                                                        FVector::Dist(Target->GetActorLocation(), Resident->GetActorLocation()))
                                      : TEXT("此前看见的对象仍在视线中");
            Resident->Agent->ObserveEvent(Event);
        }
    }
}
