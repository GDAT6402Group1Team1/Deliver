#include "AI/Characters/DeliveryNpcAIController.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NavLinkCustomComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "World/Home/DeliveryHome.h"
#include "World/DeliveryNavigationAreas.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeliveryNpcTraversal, Log, All);

namespace
{
/// @brief 在接下来几段路径中找可以直接走过的门洞连接
FNavLinkId FindOpenObstacleWalkThroughLink(const UNavigationSystemV1& Nav, const FNavigationPath& Route, uint32 NextPathIndex)
{
    const auto& Points = Route.GetPathPoints();
    if (NextPathIndex >= static_cast<uint32>(Points.Num()))
        return FNavLinkId::Invalid;
    const int32 FirstIndex = static_cast<int32>(NextPathIndex);
    for (int32 Index = FirstIndex; Index < Points.Num() && Index <= FirstIndex + 2; ++Index)
    {
        const FNavLinkId LinkId = Points[Index].CustomNavLinkId;
        if (!LinkId.IsValid())
            continue;
        // 房屋启用的门洞由角色自己走过，其余自定义链接保留各自的移动规则
        const auto* Home = ADeliveryHome::FindSharedHome(&Nav);
        const auto* Doorway = IsValid(Home) ? Home->DoorwayLink.Get() : nullptr;
        if (!IsValid(Doorway) || !Doorway->IsRegistered() || !Doorway->IsEnabled() || Doorway->GetWorld() != Nav.GetWorld() ||
            Doorway->GetLinkAreaClass() != UDeliveryPedestrianLinkArea::StaticClass())
            return FNavLinkId::Invalid;
        return Nav.GetCustomLink(LinkId) == Doorway ? LinkId : FNavLinkId::Invalid;
    }
    return FNavLinkId::Invalid;
}
} // namespace

// ── 越障 ──
// AI 控制器每帧调用 UpdateObstacleTraversal：正在跳时检查落地、超时或换了寻路；没在跳时每隔一段时间调用 TryStartObstacleTraversal 探前方
// TryStartObstacleTraversal 找到挡路的矮障碍后算起跳速度，调用 HasClearObstacleJumpArc 确认抛物线不撞东西，再调用 LaunchCharacter 起跳

/// @brief 编号为 Request 的这次寻路正在越障时返回 true
bool ADeliveryNpcAIController::IsTraversingObstacle(FAIRequestID Request) const
{
    const auto* Npc = ObstacleTraversal.Pawn.Get();
    const auto* Path = GetPathFollowingComponent();
    return Request.IsValid() && ObstacleTraversal.Request == Request && Npc && GetPawn() == Npc && Path &&
           Path->GetCurrentRequestId() == Request && Path->GetStatus() == EPathFollowingStatus::Paused &&
           CanTraverseObstacle(*Npc);
}

/// @brief 越障前提：开关打开、角色可见、有碰撞、在地面上
bool ADeliveryNpcAIController::CanTraverseObstacle(const ADeliveryNpcCharacter& Npc) const
{
    const auto* Movement = Npc.GetCharacterMovement();
    if (!bAutoJumpLowObstacles || Npc.IsActorBeingDestroyed() || Npc.IsHidden() || !Npc.GetActorEnableCollision() || !Movement ||
        Movement->HasAnimRootMotion() || Movement->HasRootMotionSources() || Npc.IsKnockedDown() || Npc.GetHeldVehicle() ||
        !Npc.Combat || Npc.Combat->IsReactingToHit() || Npc.Combat->IsStriking())
        return false;
    const auto* Companion = Cast<ADeliveryCompanionCharacter>(&Npc);
    return !Companion || (!Companion->IsMountedOnCompanionBike() && !Companion->IsEncounterAirborne() && Companion->Companion &&
                          !Companion->Companion->HasActiveHomeArrangement() && !Companion->Companion->GetHomeState().CarriedProp);
}

/// @brief 结束越障，撤销还没生效的起跳速度
void ADeliveryNpcAIController::StopObstacleTraversal()
{
    const auto Previous = ObstacleTraversal;
    ObstacleTraversal = {};
    auto* Npc = Previous.Pawn.Get();
    auto* Movement = Npc && Npc->GetController() == this ? Npc->GetCharacterMovement() : nullptr;
    // 尚未进入物理更新时只撤销本次待起跳速度，保留后来受击或其他系统写入的速度
    if (Movement && !Previous.bBeganFalling && Movement->PendingLaunchVelocity == Previous.LaunchVelocity)
        Movement->PendingLaunchVelocity = FVector::ZeroVector;
    auto* Path = GetPathFollowingComponent();
    if (Previous.Request.IsValid() && IsValid(Npc) && GetPawn() == Npc && IsValid(Path) &&
        Path->GetCurrentRequestId() == Previous.Request && Path->GetStatus() == EPathFollowingStatus::Paused)
        Path->AbortMove(*this, FPathFollowingResultFlags::UserAbort, Previous.Request, EPathFollowingVelocityMode::Keep);
}

/// @brief 每帧：跟踪正在进行的跳跃，按间隔探测前方低障碍
void ADeliveryNpcAIController::UpdateObstacleTraversal()
{
    auto* Npc = Cast<ADeliveryNpcCharacter>(GetPawn());
    auto* Path = GetPathFollowingComponent();
    auto* Movement = Npc ? Npc->GetCharacterMovement() : nullptr;
    const double Now = GetWorld()->GetTimeSeconds();
    if (!ObstacleRetreatAttempts.IsEmpty() && (!Path || Path->GetCurrentRequestId() != ObstacleRetreatAttempts[0].Request))
        ObstacleRetreatAttempts.Reset();

    // 正在越障：检查是否落地、超时或换了寻路
    if (ObstacleTraversal.Request.IsValid())
    {
        if (!IsValid(Npc) || ObstacleTraversal.Pawn.Get() != Npc || !Path ||
            Path->GetCurrentRequestId() != ObstacleTraversal.Request || Path->GetStatus() != EPathFollowingStatus::Paused)
        {
            StopObstacleTraversal();
            return;
        }
        if (!Movement || !CanTraverseObstacle(*Npc))
        {
            StopObstacleTraversal();
            return;
        }
        if (ObstacleTraversal.bBackingUp)
        {
            if (!Movement->IsMovingOnGround())
            {
                StopObstacleTraversal();
                return;
            }
            const FVector BackTo = ObstacleTraversal.LandingFeet - Npc->GetNavAgentLocation();
            if (BackTo.SizeSquared2D() > FMath::Square(14.) && Now < ObstacleTraversal.ExpiresAt)
            {
                Movement->RequestDirectMove(BackTo.GetSafeNormal2D() * FMath::Min(Movement->GetMaxSpeed(), 150.f), false);
                return;
            }
            const FAIRequestID Request = ObstacleTraversal.Request;
            Movement->StopMovementImmediately();
            // 退到真实起跳位置后重新验一次完整轨迹，同一请求只允许后退一次
            if (BackTo.SizeSquared2D() <= FMath::Square(26.) && TryStartObstacleTraversal(*Npc, *Path, Now))
                return;
            if (IsTraversingObstacle(Request))
            {
                ObstacleTraversal = {};
                NextObstacleProbeAt = Now + FMath::Clamp(ObstacleJumpRetrySeconds, .2f, 3.f);
                Path->ResumeMove(Request);
            }
            return;
        }
        if (Now >= ObstacleTraversal.ExpiresAt)
        {
            StopObstacleTraversal();
            return;
        }
        if (Movement->IsFalling())
        {
            ObstacleTraversal.bBeganFalling = true;
            return;
        }
        if (!Movement->IsMovingOnGround())
        {
            StopObstacleTraversal();
            return;
        }
        if (!ObstacleTraversal.bBeganFalling)
            return;

        const FAIRequestID Request = ObstacleTraversal.Request;
        const FVector Feet = Npc->GetNavAgentLocation();
        const bool bReachedLanding = Movement->CurrentFloor.IsWalkableFloor() &&
                                     FVector::DistSquared2D(Feet, ObstacleTraversal.LandingFeet) <= FMath::Square(55.) &&
                                     FMath::Abs(Feet.Z - ObstacleTraversal.LandingFeet.Z) <= 30.;
        ObstacleTraversal = {};
        NextObstacleProbeAt = Now + FMath::Clamp(ObstacleJumpRetrySeconds, .2f, 3.f);
        UE_LOG(LogDeliveryNpcTraversal, Log, TEXT("NpcObstacleJumpLanded npc=%s move=%u reached=%d feet=%s"), *GetNameSafe(Npc),
               Request.GetID(), bReachedLanding, *Feet.ToCompactString());
        // 实际落地后恢复原 BT 路线，动作成功与任务结果仍由原执行者确认
        Path->ResumeMove(Request);
        return;
    }

    // 按间隔探测是否需要起跳
    if (Now < NextObstacleProbeAt || !IsValid(Npc) || !Path || !Movement || !Movement->IsMovingOnGround() ||
        !Movement->CurrentFloor.IsWalkableFloor() || Path->GetStatus() != EPathFollowingStatus::Moving ||
        !Path->GetCurrentRequestId().IsValid() || Path->GetCurrentRequestId() == SpacingMoveId || !CanTraverseObstacle(*Npc))
        return;
    NextObstacleProbeAt = Now + .18;
    TryStartObstacleTraversal(*Npc, *Path, Now);
}

/// @brief 沿跳跃抛物线逐段扫掠胶囊，确认不会撞墙
bool ADeliveryNpcAIController::HasClearObstacleJumpArc(const ADeliveryNpcCharacter& Npc, const FVector& LaunchVelocity,
                                                       double FlightTime) const
{
    const auto* Capsule = Npc.GetCapsuleComponent();
    const auto* Movement = Npc.GetCharacterMovement();
    const auto* World = GetWorld();
    if (!Capsule || !Movement || !World || LaunchVelocity.ContainsNaN() || !FMath::IsFinite(FlightTime) || FlightTime <= 0.)
        return false;
    const FVector Start = Capsule->GetComponentLocation();
    const FCollisionShape Shape =
        FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight());
    const FCollisionResponseParams Response(Capsule->GetCollisionResponseToChannels());
    const FCollisionQueryParams Query(SCENE_QUERY_STAT(NpcObstacleArc), false, &Npc);
    FVector Previous = Start;
    const int32 Samples = FMath::Clamp(FMath::CeilToInt(FlightTime / .035), 6, 32);
    for (int32 Index = 1; Index <= Samples; ++Index)
    {
        const double Time = FlightTime * Index / Samples;
        const FVector Next = Start + LaunchVelocity * Time + FVector(0, 0, .5 * Movement->GetGravityZ() * FMath::Square(Time));
        if (World->SweepTestByChannel(Previous, Next, FQuat::Identity, Capsule->GetCollisionObjectType(), Shape, Query, Response))
            return false;
        Previous = Next;
    }
    return true;
}

/// @brief 把当前路径点沿路线往前延伸，作为跳跃落点的参考
FVector ADeliveryNpcAIController::ExtendObstacleRouteTarget(const FNavigationPath& Route, uint32 NextPathIndex,
                                                            const FVector& Feet, const FVector& Direction,
                                                            const FVector& CurrentTarget, const FNavLinkId& WalkThroughLink,
                                                            bool& bStopsBeforeCustomLink)
{
    bStopsBeforeCustomLink = false;
    const auto& Points = Route.GetPathPoints();
    if (NextPathIndex >= static_cast<uint32>(Points.Num()))
        return CurrentTarget;
    const int32 CurrentIndex = static_cast<int32>(NextPathIndex);
    constexpr double MaxLookAhead = 260.;
    constexpr double MaxSideOffset = 45.;
    constexpr double MinDirectionDot = .866;
    FVector End = CurrentTarget;
    double Forward = FVector::DotProduct(End - Feet, Direction);
    const auto BlocksLookAhead = [&](int32 Index)
    {
        const FNavLinkId LinkId = Points[Index].CustomNavLinkId;
        return LinkId.IsValid() && LinkId != WalkThroughLink;
    };
    bStopsBeforeCustomLink = BlocksLookAhead(CurrentIndex);
    if (bStopsBeforeCustomLink)
        return End;
    for (int32 Index = CurrentIndex + 1; Index < Points.Num() && Index <= CurrentIndex + 2 && Forward < MaxLookAhead; ++Index)
    {
        // 前瞻停在下一条自定义链接之前，也限制导航投影后的落点
        if (BlocksLookAhead(Index))
        {
            bStopsBeforeCustomLink = true;
            break;
        }
        const FVector Next = *Route.GetPathPointLocation(Index);
        const FVector Offset = Next - End;
        if (Offset.IsNearlyZero(1.))
            continue;
        if (Next.ContainsNaN() || FVector::DotProduct(Offset.GetSafeNormal2D(), Direction) < MinDirectionDot)
            break;
        const double NextForward = FVector::DotProduct(Next - Feet, Direction);
        if (NextForward <= Forward)
            break;
        const FVector Candidate = FMath::Lerp(End, Next, FMath::Min(1., (MaxLookAhead - Forward) / (NextForward - Forward)));
        const FVector FromFeet = Candidate - Feet;
        const FVector Lateral = FromFeet - Direction * FVector::DotProduct(FromFeet, Direction);
        if (Lateral.SizeSquared2D() > FMath::Square(MaxSideOffset))
            break;
        End = Candidate;
        Forward = FMath::Min(NextForward, MaxLookAhead);
    }
    return End;
}

/// @brief 这次寻路是否已在同一障碍前后退过
bool ADeliveryNpcAIController::HasTriedObstacleRetreat(FAIRequestID Request, const UPrimitiveComponent& Blocker,
                                                       const FVector& Feet) const
{
    return ObstacleRetreatAttempts.ContainsByPredicate(
        [&](const FObstacleRetreat& Attempt)
        {
            return Attempt.Request == Request && Attempt.Blocker.Get() == &Blocker &&
                   FVector::DistSquared(Feet, Attempt.Feet) <= FMath::Square(120.) &&
                   FVector::DistSquared(Blocker.GetComponentLocation(), Attempt.BlockerLocation) <= FMath::Square(45.);
        });
}

/// @brief 前方有低障碍时计算起跳速度，检查落点和抛物线，必要时先后退半步
bool ADeliveryNpcAIController::TryStartObstacleTraversal(ADeliveryNpcCharacter& Npc, UPathFollowingComponent& Path, double Now)
{
    const auto Route = Path.GetPath();
    auto* World = GetWorld();
    auto* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
    auto* Movement = Npc.GetCharacterMovement();
    const auto* Capsule = Npc.GetCapsuleComponent();
    if (!Route.IsValid() || !Route->IsValid() || Route->IsPartial() || Path.GetCurrentCustomLinkOb() || !Nav || !Movement ||
        !Capsule || !Movement->PendingLaunchVelocity.IsZero())
        return false;

    // 确定前进方向
    const FVector Start = Capsule->GetComponentLocation();
    const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
    const float Radius = Capsule->GetScaledCapsuleRadius();
    const FVector StartFeet = Start - FVector(0, 0, HalfHeight);
    const FVector CurrentTarget = Path.GetCurrentTargetLocation();
    if (CurrentTarget.ContainsNaN())
        return false;
    const FVector TowardTarget = (CurrentTarget - StartFeet).GetSafeNormal2D();
    const FVector Direction = TowardTarget.IsNearlyZero() ? Path.GetCurrentDirection().GetSafeNormal2D() : TowardTarget;
    if (Direction.IsNearlyZero())
        return false;
    const FNavLinkId WalkThroughLink = FindOpenObstacleWalkThroughLink(*Nav, *Route, Path.GetNextPathIndex());
    bool bStopsBeforeCustomLink = false;
    const FVector Target = ExtendObstacleRouteTarget(*Route, Path.GetNextPathIndex(), StartFeet, Direction, CurrentTarget,
                                                     WalkThroughLink, bStopsBeforeCustomLink);
    const double LandingLimit =
        bStopsBeforeCustomLink ? FVector::DotProduct(Target - StartFeet, Direction) : FVector::Dist2D(StartFeet, Target) + Radius;

    // 沿前进方向扫掠胶囊，找到挡路的静态物体
    const FCollisionShape Shape = FCollisionShape::MakeCapsule(Radius, HalfHeight);
    const ECollisionChannel Channel = Capsule->GetCollisionObjectType();
    const FCollisionResponseParams Response(Capsule->GetCollisionResponseToChannels());
    FCollisionQueryParams Query(SCENE_QUERY_STAT(NpcObstacleJump), false, &Npc);
    const double ProbeDistance =
        FMath::Min<double>(FMath::Clamp(ObstacleJumpProbeDistance, 60.f, 180.f), FVector::Dist2D(StartFeet, Target));
    FHitResult Barrier;
    if (ProbeDistance < 12. ||
        !World->SweepSingleByChannel(Barrier, Start, Start + Direction * ProbeDistance, FQuat::Identity, Channel, Shape, Query,
                                     Response) ||
        Barrier.bStartPenetrating || Movement->IsWalkable(Barrier) || !IsValid(Barrier.GetComponent()))
        return false;
    auto* Blocker = Barrier.GetComponent();
    const auto ObjectType = Blocker->GetCollisionObjectType();
    if (ObjectType != ECC_WorldStatic && ObjectType != ECC_WorldDynamic)
        return false;

    // 依次尝试三个落点距离，检查落点导航、支撑、高度和抛物线
    const double Gravity = -Movement->GetGravityZ();
    const double MaxJumpSpeed = Movement->JumpZVelocity;
    if (!FMath::IsFinite(Gravity) || !FMath::IsFinite(MaxJumpSpeed) || Gravity <= 0. || MaxJumpSpeed <= 0. ||
        !Movement->GetGravityDirection().Equals(FVector::DownVector))
        return false;
    const double MaxRise = FMath::Clamp(ObstacleJumpMaxLandingRise, 25.f, 100.f);
    const double MinimumLandingDistance = Barrier.Distance + 2. * Radius + 12.;
    const FAIRequestID Request = Path.GetCurrentRequestId();
    const auto Filter = Route->GetFilter();
    const bool bAfterBackingUp = IsTraversingObstacle(Request);
    const auto KeepCurrentRequest = [&]()
    {
        return CanTraverseObstacle(Npc) && GetPawn() == &Npc && Path.GetCurrentRequestId() == Request &&
               Path.GetPath() == Route &&
               Path.GetStatus() == (bAfterBackingUp ? EPathFollowingStatus::Paused : EPathFollowingStatus::Moving);
    };
    for (const double Extra : {0., 35., 70.})
    {
        const double Distance = MinimumLandingDistance + Extra;
        if (Distance > LandingLimit)
            break;
        FVector DesiredFeet = StartFeet + Direction * Distance;
        DesiredFeet.Z = FMath::Clamp(Target.Z, StartFeet.Z - 80., StartFeet.Z + MaxRise);
        FNavLocation LandingNav;
        if (!Nav->ProjectPointToNavigation(DesiredFeet, LandingNav, FVector(35, 35, 110), &Npc.GetNavAgentPropertiesRef(),
                                           Filter) ||
            FVector::DistSquared2D(DesiredFeet, LandingNav.Location) > FMath::Square(35.))
            continue;
        const FVector FloorProbe = LandingNav.Location + FVector(0, 0, HalfHeight + 14.);
        FFindFloorResult Floor;
        Movement->FindFloor(FloorProbe, Floor, false);
        if (!Floor.IsWalkableFloor() || Floor.HitResult.bStartPenetrating ||
            FMath::Abs(Floor.HitResult.ImpactPoint.Z - LandingNav.Location.Z) > 25.)
            continue;
        const FVector LandingFeet(LandingNav.Location.X, LandingNav.Location.Y, Floor.HitResult.ImpactPoint.Z + 3.);
        const FVector Delta = LandingFeet - StartFeet;
        const double ForwardLanding = FVector::DotProduct(Delta, Direction);
        if (Delta.Z > MaxRise || Delta.Z < -80. || ForwardLanding < Barrier.Distance + Radius + 15. ||
            (bStopsBeforeCustomLink && ForwardLanding > LandingLimit))
            continue;
        const double LowJumpSpeed = FMath::Min(MaxJumpSpeed, FMath::Sqrt(2. * Gravity * FMath::Max(58., Delta.Z + 16.)));
        for (const double JumpSpeed : {LowJumpSpeed, MaxJumpSpeed})
        {
            const double Discriminant = FMath::Square(JumpSpeed) - 2. * Gravity * Delta.Z;
            if (Discriminant <= 0.)
                continue;
            const double FlightTime = (JumpSpeed + FMath::Sqrt(Discriminant)) / Gravity;
            const FVector LaunchVelocity(Delta.X / FlightTime, Delta.Y / FlightTime, JumpSpeed);
            if (LaunchVelocity.SizeSquared2D() > FMath::Square(300.) || !HasClearObstacleJumpArc(Npc, LaunchVelocity, FlightTime))
                continue;
            if (!KeepCurrentRequest())
                return false;
            if (!bAfterBackingUp)
                Path.PauseMove(Request, EPathFollowingVelocityMode::Keep);
            if (!IsValid(&Npc) || GetPawn() != &Npc || Path.GetCurrentRequestId() != Request ||
                Path.GetStatus() != EPathFollowingStatus::Paused)
                return false;
            ObstacleTraversal = {};
            ObstacleTraversal.Pawn = &Npc;
            ObstacleTraversal.Request = Request;
            ObstacleTraversal.LaunchVelocity = LaunchVelocity;
            ObstacleTraversal.LandingFeet = LandingFeet;
            ObstacleTraversal.ExpiresAt = Now + FlightTime + .55;
            // 实际开始和完整胶囊扫弧使用同一个初速，不依赖下一帧补水平速度
            Npc.LaunchCharacter(LaunchVelocity, true, true);
            UE_LOG(LogDeliveryNpcTraversal, Log,
                   TEXT("NpcObstacleJumpStarted npc=%s move=%u obstacle=%s openDoorwayAhead=%d from=%s landing=%s"),
                   *GetNameSafe(&Npc), Request.GetID(), *GetNameSafe(Barrier.GetActor()), WalkThroughLink.IsValid(),
                   *StartFeet.ToCompactString(), *LandingFeet.ToCompactString());
            return true;
        }
    }
    if (bAfterBackingUp || Barrier.Distance > 25. || !KeepCurrentRequest() ||
        HasTriedObstacleRetreat(Request, *Blocker, StartFeet))
        return false;
    ObstacleRetreatAttempts.RemoveAll(
        [&](const FObstacleRetreat& Attempt)
        {
            return !Attempt.Blocker.IsValid() || FVector::DistSquared(StartFeet, Attempt.Feet) > FMath::Square(120.) ||
                   FVector::DistSquared(Attempt.Blocker->GetComponentLocation(), Attempt.BlockerLocation) > FMath::Square(45.);
        });
    ObstacleRetreatAttempts.Add({Request, Blocker, StartFeet, Blocker->GetComponentLocation()});

    // 落点都不行时后退半步再试
    // 门槛已贴脚时允许在原来一侧退半步，每个脚点都核对导航、支撑和整个胶囊
    FVector Prev = Start;
    for (const double Distance : {35., 70.})
    {
        const FVector Behind = StartFeet - Direction * Distance;
        FNavLocation Retreat;
        if (!Nav->ProjectPointToNavigation(Behind, Retreat, FVector(18, 18, 24), &Npc.GetNavAgentPropertiesRef(), Filter) ||
            FVector::DistSquared2D(Behind, Retreat.Location) > FMath::Square(15.))
            return false;
        FFindFloorResult Floor;
        Movement->FindFloor(Retreat.Location + FVector(0, 0, HalfHeight + 14.), Floor, false);
        if (!Floor.IsWalkableFloor() || Floor.HitResult.bStartPenetrating ||
            FMath::Abs(Floor.HitResult.ImpactPoint.Z - StartFeet.Z) > 18.)
            return false;
        const FVector Center(Retreat.Location.X, Retreat.Location.Y, Floor.HitResult.ImpactPoint.Z + HalfHeight + 3.);
        if (World->SweepTestByChannel(Prev, Center, FQuat::Identity, Channel, Shape, Query, Response))
            return false;
        Prev = Center;
    }
    if (!KeepCurrentRequest())
        return false;
    Path.PauseMove(Request, EPathFollowingVelocityMode::Reset);
    if (!IsValid(&Npc) || GetPawn() != &Npc || Path.GetCurrentRequestId() != Request ||
        Path.GetStatus() != EPathFollowingStatus::Paused)
        return false;
    ObstacleTraversal = {};
    ObstacleTraversal.Pawn = &Npc;
    ObstacleTraversal.Request = Request;
    ObstacleTraversal.LandingFeet = Prev - FVector(0, 0, HalfHeight);
    ObstacleTraversal.ExpiresAt = Now + .85;
    ObstacleTraversal.bBackingUp = true;
    return true;
}
