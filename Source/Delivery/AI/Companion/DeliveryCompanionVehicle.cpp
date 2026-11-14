#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "Vehicle/DeliveryCompanionMotorbike.h"
#include "AIController.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "Navigation/PathFollowingComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"

bool UDeliveryCompanionComponent::IsUsingErrandVehicle() const
{
    return IsValid(ErrandVehicle) && ErrandVehicle->GetReservedCompanion()==GetOwner() && ErrandVehicle->IsMounted();
}

ADeliveryCompanionMotorbike* UDeliveryCompanionComponent::GetFollowMotorbike() const
{
    return Mode==EDeliveryCompanionMode::Follow && bRideWhileFollowing && !HasActiveErrand() &&
        VehicleTravelPurpose==EDeliveryCompanionTravelPurpose::Follow && IsUsingErrandVehicle()?ErrandVehicle.Get():nullptr;
}

bool UDeliveryCompanionComponent::EnsureOnFoot()
{
    if (!IsUsingErrandVehicle()) { bVehicleNeedsDismount=false; return true; }
    if (!GetOwner() || !GetOwner()->HasAuthority()) return false;
    bVehicleHasDriveGoal=false;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const bool bExited=ErrandVehicle->TryDismount();
    if (!WeakThis.IsValid()) return false;
    bVehicleNeedsDismount=!bExited;
    return bExited;
}

void UDeliveryCompanionComponent::StopVehicleApproach()
{
    const FAIRequestID OldId=VehicleApproachMoveId;
    const bool bOwned=bVehicleOwnsApproach;
    bVehicleOwnsApproach=false;
    VehicleApproachMoveId=FAIRequestID::InvalidRequest;
    const auto* Npc=GetCompanion();
    if (const auto* AI=Npc?Cast<AAIController>(Npc->GetController()):nullptr)
        if (auto* Path=AI->GetPathFollowingComponent(); Path && bOwned && OldId.IsValid() && Path->GetCurrentRequestId()==OldId)
            Path->AbortMove(*this,FPathFollowingResultFlags::UserAbort,OldId);
}

void UDeliveryCompanionComponent::StopWalkingForVehicle()
{
    // 接管前先清自己保存的步行编号，迟到的 BT 清理不能误停走向车的新请求
    const FAIRequestID OldFollow=bOwnsNavigation?FollowMoveId:FAIRequestID::InvalidRequest;
    const FAIRequestID OldErrand=bErrandOwnsNavigation?ErrandMoveId:FAIRequestID::InvalidRequest;
    bOwnsNavigation=false; FollowMoveId=FAIRequestID::InvalidRequest;
    bErrandOwnsNavigation=false; ErrandMoveId=FAIRequestID::InvalidRequest;
    bHasFollowDestination=false; bHasErrandDestination=false;
    const auto* Npc=GetCompanion();
    const auto* AI=Npc?Cast<AAIController>(Npc->GetController()):nullptr;
    if (auto* Path=AI?AI->GetPathFollowingComponent():nullptr)
    {
        const FAIRequestID Current=Path->GetCurrentRequestId();
        if (Current.IsValid() && (Current==OldFollow || Current==OldErrand))
            Path->AbortMove(*this,FPathFollowingResultFlags::UserAbort,Current);
    }
}

void UDeliveryCompanionComponent::StopVehicleTravel(EDeliveryCompanionTravelPurpose Purpose)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || Purpose==EDeliveryCompanionTravelPurpose::None || VehicleTravelPurpose!=Purpose) return;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const int32 Revision=CommandRevision;
    bVehicleHasDriveGoal=false;
    StopVehicleApproach();
    if (!WeakThis.IsValid() || CommandRevision!=Revision || VehicleTravelPurpose!=Purpose) return;
    EnsureOnFoot();
}

void UDeliveryCompanionComponent::ReleaseErrandVehicle()
{
    if (!GetOwner() || !GetOwner()->HasAuthority()) return;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const int32 Revision=CommandRevision;
    VehicleTravelPurpose=EDeliveryCompanionTravelPurpose::None;
    VehicleLegRevision=-1;
    bVehicleLegComplete=false;
    bVehicleHasDriveGoal=false;
    FollowRouteBike.Reset();
    FollowRoute.Reset();
    NextVehicleNavigationAt=0;
    StopVehicleApproach();
    if (!WeakThis.IsValid() || CommandRevision!=Revision || VehicleTravelPurpose!=EDeliveryCompanionTravelPurpose::None) return;
    EnsureOnFoot();
}

void UDeliveryCompanionComponent::SampleFollowBikeRoute(ADeliveryMotorbike* Bike)
{
    if (FollowRouteBike.Get()!=Bike)
    {
        FollowRoute.Reset();
        FollowRouteBike=Bike;
    }
    if (!IsValid(Bike) || Bike->IsActorBeingDestroyed()) { FollowRoute.Reset(); return; }
    const FVector Position=Bike->GetActorLocation();
    if (Position.ContainsNaN()) return;
    if (!FollowRoute.IsEmpty())
    {
        const double Distance=FVector::Dist(Position,FollowRoute.Last());
        if (Distance<180.) return;
        // 换车或位置突变后的记录不能和旧轨迹连接
        if (Distance>2400.) FollowRoute.Reset();
        else if (FollowRoute.Num()==32) FollowRoute.RemoveAt(0,1,EAllowShrinking::No);
    }
    FollowRoute.Add(Position);
}

void UDeliveryCompanionComponent::ReportVehiclePause(FName Reason)
{
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const int32 Revision=CommandRevision;
    const auto Purpose=VehicleTravelPurpose;
    if (Purpose==EDeliveryCompanionTravelPurpose::Errand) ReportErrandPause(Reason,IsUsingErrandVehicle());
    else
    {
        // 跟车受阻时先停车，保留当前骑乘，等待下一次路线检查
        if (Purpose==EDeliveryCompanionTravelPurpose::Follow &&
            IsUsingErrandVehicle())
        {
            bVehicleHasDriveGoal=false;
            StopVehicleApproach();
            if (!WeakThis.IsValid() || CommandRevision!=Revision || VehicleTravelPurpose!=Purpose) return;
            if (IsUsingErrandVehicle()) ErrandVehicle->StopDriving();
        }
        else StopVehicleTravel(VehicleTravelPurpose);
        if (!WeakThis.IsValid() || CommandRevision!=Revision || VehicleTravelPurpose!=Purpose) return;
        SetSuspension(Reason);
    }
    if (!WeakThis.IsValid() || CommandRevision!=Revision || VehicleTravelPurpose!=Purpose) return;
    NextVehicleNavigationAt=GetWorld()->GetTimeSeconds()+GetTuning().BlockedRetrySeconds;
    if (Purpose==EDeliveryCompanionTravelPurpose::Follow && IsUsingErrandVehicle()) ErrandVehicle->ReportFollowBlocked();
}

bool UDeliveryCompanionComponent::UpdateVehicleTravel(const FVector& Goal,float ReachRadius,EDeliveryCompanionTravelPurpose Purpose)
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority() || !CanCommand(OwnerPlayerState) || Purpose==EDeliveryCompanionTravelPurpose::None ||
        Goal.ContainsNaN() || !FMath::IsFinite(ReachRadius)) return false;
    const bool bForErrand=Purpose==EDeliveryCompanionTravelPurpose::Errand;
    const bool bFollowing=Purpose==EDeliveryCompanionTravelPurpose::Follow;
    const bool bKeepMountedFollow=bFollowing && bRideWhileFollowing;
    if (bForErrand && (!HasActiveErrand() || Errand.CommandRevision!=CommandRevision)) return false;
    const auto Phase=bForErrand?Errand.Phase:EDeliveryCompanionErrandPhase::Idle;
    const int32 Revision=CommandRevision;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const auto IsCurrent=[&]()
    {
        return WeakThis.IsValid() && WeakNpc.IsValid() && !Npc->IsActorBeingDestroyed() && CommandRevision==Revision &&
            (!bForErrand || (HasActiveErrand() && Errand.CommandRevision==Revision && Errand.Phase==Phase));
    };
    // 未指定出行时先步行跟随，远程跑腿等待安排
    const bool bUnspecified=TravelMode==EDeliveryCompanionTravelMode::Unspecified;
    const bool bTransferNearby=bForErrand && CanTransferNearbyPackage(Errand.Action);
    const bool bLocalItemTransfer=bForErrand && Errand.bLocalItemTransfer;
    if (bUnspecified && Purpose!=EDeliveryCompanionTravelPurpose::Follow &&
        Purpose!=EDeliveryCompanionTravelPurpose::ReturnToStay && !bTransferNearby && !bLocalItemTransfer)
    {
        StopWalkingForVehicle();
        if (!IsCurrent()) return false;
        if (bForErrand) ReportErrandPause(TEXT("TravelModeNeeded"));
        else SetSuspension(TEXT("TravelModeNeeded"));
        return false;
    }
    const bool bOwnVehicleAvailable=IsValid(ErrandVehicle) && ErrandVehicle->GetWorld()==GetWorld() &&
        (!ErrandVehicle->GetReservedCompanion() || ErrandVehicle->GetReservedCompanion()==Npc);
    const bool bWalk=bFollowing?!bKeepMountedFollow:
        bLocalItemTransfer || TravelMode==EDeliveryCompanionTravelMode::Walk || bUnspecified ||
        (TravelMode==EDeliveryCompanionTravelMode::Automatic && !bOwnVehicleAvailable);
    if (bWalk)
    {
        StopVehicleApproach();
        if (!IsCurrent()) return false;
        const bool bOnFoot=EnsureOnFoot();
        if (!IsCurrent()) return false;
        if (!bOnFoot) SetSuspension(TEXT("CompanionDismountBlocked"));
        else if (!bForErrand || SuspensionReason==TEXT("CompanionDismountBlocked")) SetSuspension(NAME_None);
        return bOnFoot;
    }
    if (VehicleTravelPurpose!=Purpose || VehicleLegRevision!=CommandRevision || VehicleLegPhase!=Phase)
    {
        ReleaseErrandVehicle();
        if (!IsCurrent()) return false;
        // 新阶段先完成旧车的下车，不能在受阻时给同一辆车重新派发目的地
        const bool bOnFoot=EnsureOnFoot();
        if (!IsCurrent()) return false;
        if (!bOnFoot) { SetSuspension(TEXT("CompanionDismountBlocked")); return false; }
        VehicleTravelPurpose=Purpose;
        VehicleLegRevision=CommandRevision;
        VehicleLegPhase=Phase;
        bVehicleLegComplete=false;
        bVehicleHasDriveGoal=false;
    }
    if (bVehicleNeedsDismount)
    {
        const bool bOnFoot=EnsureOnFoot();
        if (!IsCurrent()) return false;
        if (!bOnFoot) { SetSuspension(TEXT("CompanionDismountBlocked")); return false; }
    }
    if (bKeepMountedFollow) SampleFollowBikeRoute(Cast<ADeliveryMotorbike>(GetOwnerPawn()));
    const float Distance=FVector::Dist2D(Npc->GetActorLocation(),Goal);
    // 跟车使用近距离停车间距，独立出行采用车辆的默认起骑距离
    const float PreferredDistance=(bKeepMountedFollow || TravelMode==EDeliveryCompanionTravelMode::OwnVehicle)?
        FMath::Max(450.f,ReachRadius+180.f):(IsValid(ErrandVehicle)?ErrandVehicle->GetPreferredTravelDistance():1200.f);
    const double Now=GetWorld()->GetTimeSeconds();
    // 骑乘中的近停检查沿原导航间隔执行，停车后也复查高度和遮挡变化
    if (bKeepMountedFollow && IsUsingErrandVehicle() && Now<NextVehicleNavigationAt) return false;
    const auto IsAtDrivenFollowParking=[&]()
    {
        return IsUsingErrandVehicle() && (bVehicleHasDriveGoal || bVehicleLegComplete) &&
            ErrandVehicle->HasReachedDestination() &&
            FVector::Dist(Goal,VehicleLegGoal)<=GetTuning().RepathDistance &&
            FVector::Dist2D(ErrandVehicle->GetActorLocation(),VehicleParkingGoal)<=ErrandVehicle->GetParkingAcceptanceRadius();
    };
    const auto CanWaitMountedNearOwner=[&]()
    {
        const auto* OwnerPawn=GetOwnerPawn();
        if (!IsUsingErrandVehicle() || !IsValid(OwnerPawn) || OwnerPawn->IsActorBeingDestroyed() ||
            OwnerPawn->GetWorld()!=GetWorld()) return false;
        if (const auto* Bike=Cast<ADeliveryMotorbike>(OwnerPawn); Bike && FMath::Abs(Bike->GetCurrentSpeed())>30.f)
            return false;
        const FVector Here=Npc->GetActorLocation();
        if (FMath::Abs(Here.Z-Goal.Z)>180.f ||
            (FVector::Dist2D(Here,Goal)>PreferredDistance && !IsAtDrivenFollowParking())) return false;
        FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionVehicleFollowWait),false,Npc);
        Query.AddIgnoredActor(ErrandVehicle);
        Query.AddIgnoredActor(OwnerPawn);
        FCollisionObjectQueryParams Objects;
        for (const auto Channel:{ECC_WorldStatic,ECC_WorldDynamic,ECC_Pawn,ECC_Vehicle,ECC_PhysicsBody})
            Objects.AddObjectTypesToQuery(Channel);
        FHitResult Hit;
        return !GetWorld()->LineTraceSingleByObjectType(Hit,Here,Goal,Objects,Query) &&
            ErrandVehicle->CanParkAt(ErrandVehicle->GetActorLocation());
    };
    if (bVehicleLegComplete && !bKeepMountedFollow)
    {
        const bool bOnFoot=EnsureOnFoot();
        if (!IsCurrent()) return false;
        if (!bOnFoot) { SetSuspension(TEXT("CompanionDismountBlocked")); return false; }
        // 主人完成这段行程后又走远，可以再次去真实停车处取车
        if (Distance>PreferredDistance && FVector::Dist2D(Goal,VehicleLegGoal)>GetTuning().RepathDistance)
            bVehicleLegComplete=false;
        else
        {
            if (!bForErrand || SuspensionReason==TEXT("CompanionDismountBlocked")) SetSuspension(NAME_None);
            return true;
        }
    }
    // 最后一段步行进入交互点，较远处只使用地图指定的专属车
    if (!bKeepMountedFollow && !IsUsingErrandVehicle() && Distance<=PreferredDistance)
    {
        // 目标已走到附近时放弃自己发起的上车路径，不取消其他执行者的新路径
        StopVehicleApproach();
        return IsCurrent();
    }
    StopWalkingForVehicle();
    if (!IsCurrent()) return false;
    if (!IsValid(ErrandVehicle) || ErrandVehicle->GetWorld()!=GetWorld() || !ErrandVehicle->InitializeForCompanion(Npc))
    {
        ReportVehiclePause(TEXT("CompanionVehicleUnavailable"));
        return false;
    }
    auto* AI=Cast<AAIController>(Npc->GetController());
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!AI || !Nav) { ReportVehiclePause(TEXT("CompanionVehicleNavigationUnavailable")); return false; }
    if (!IsUsingErrandVehicle())
    {
        if (Now<NextVehicleNavigationAt && !bVehicleOwnsApproach) return false;
        const FVector Here=Npc->GetActorLocation();
        if (ErrandVehicle->CanBoardFromLocation(Here))
        {
            StopVehicleApproach();
            if (!IsCurrent()) return false;
            if (!ErrandVehicle->TryMount(Npc))
            {
                if (!IsCurrent() || !IsValid(ErrandVehicle)) return false;
                const FName Failure=ErrandVehicle->GetDriveFailureReason();
                ReportVehiclePause(Failure.IsNone()?FName(TEXT("CompanionBoardingBlocked")):Failure);
                return false;
            }
            if (!IsCurrent() || !IsUsingErrandVehicle()) return false;
        }
        else
        {
            const auto* Following=AI->GetPathFollowingComponent();
            if (bVehicleOwnsApproach && Following && Following->GetCurrentRequestId()==VehicleApproachMoveId &&
                AI->GetMoveStatus()!=EPathFollowingStatus::Idle) return false;
            if (Now<NextVehicleNavigationAt) return false;
            NextVehicleNavigationAt=Now+GetTuning().NavigationInterval;
            FVector NearSide=ErrandVehicle->GetBoardingLocation();
            FVector FarSide=ErrandVehicle->GetOppositeBoardingLocation();
            if (FVector::DistSquared2D(Here,NearSide)>FVector::DistSquared2D(Here,FarSide)) Swap(NearSide,FarSide);
            const FVector FeetToCenter(0,0,Npc->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
            // 站在近侧却隔着墙时，继续寻找能真正上车的另一侧
            for (const FVector& Boarding:{NearSide,FarSide})
            {
                FVector Toward=(Here-Boarding).GetSafeNormal2D();
                if (Toward.IsNearlyZero()) Toward=ErrandVehicle->GetActorRightVector();
                for (const float Angle:{0.f,45.f,-45.f,90.f,-90.f,180.f})
                {
                    FNavLocation Board;
                    const FVector Proposed=Boarding+Toward.RotateAngleAxis(Angle,FVector::UpVector)*30.f;
                    if (!Nav->ProjectPointToNavigation(Proposed,Board,FVector(90,90,250),&Npc->GetNavAgentPropertiesRef()) ||
                        FVector::Dist2D(Board.Location,Boarding)>ErrandVehicle->GetBoardingRadius()-15.f ||
                        !IsDestinationClear(Board.Location) || !ErrandVehicle->CanBoardFromLocation(Board.Location+FeetToCenter)) continue;
                    const auto* WalkPath=Nav->FindPathToLocationSynchronously(GetWorld(),Npc->GetNavAgentLocation(),Board.Location,Npc);
                    if (!WalkPath || !WalkPath->IsValid() || WalkPath->IsPartial()) continue;
                    FAIMoveRequest Request(Board.Location);
                    Request.SetAcceptanceRadius(15); Request.SetAllowPartialPath(false);
                    Request.SetReachTestIncludesAgentRadius(false); Request.SetReachTestIncludesGoalRadius(false);
                    const auto Result=AI->MoveTo(Request);
                    if (!IsCurrent()) return false;
                    // 当前位置已确认无法上车，导航坐标容差返回已到时继续试其他候选
                    if (Result.Code!=EPathFollowingRequestResult::RequestSuccessful) continue;
                    VehicleApproachMoveId=Result.MoveId;
                    bVehicleOwnsApproach=true;
                    if (bForErrand && !bErrandVehicleApproachBudgetAdded)
                    {
                        const double Length=WalkPath->GetPathLength();
                        const float Speed=Npc->GetCharacterMovement()->GetMaxSpeed();
                        if (FMath::IsFinite(Length) && Length>=0 && FMath::IsFinite(Speed) && Speed>0)
                        {
                            ErrandTimeLimitSeconds+=Length/Speed;
                            bErrandVehicleApproachBudgetAdded=true;
                        }
                    }
                    SetSuspension(NAME_None);
                    return false;
                }
            }
            ReportVehiclePause(TEXT("CompanionVehicleApproachBlocked"));
            return false;
        }
    }

    if (bKeepMountedFollow)
    {
        NextVehicleNavigationAt=Now+GetTuning().NavigationInterval;
        // 保留这段真实到站目标，不能每次等候都刷新它而漏掉主人累积走远
        const bool bKeepParkingGoal=IsAtDrivenFollowParking();
        if (CanWaitMountedNearOwner())
        {
            ErrandVehicle->StopDriving();
            if (!IsCurrent() || !IsUsingErrandVehicle()) return false;
            bVehicleHasDriveGoal=false;
            bVehicleLegComplete=true;
            if (!bKeepParkingGoal) VehicleLegGoal=Goal;
            SetSuspension(NAME_None);
            return false;
        }
        // 旧到站只表示上一停车点，主人换楼层或走到墙后时不能沿用完成状态
        bVehicleLegComplete=false;
    }
    if (bVehicleHasDriveGoal && ErrandVehicle->HasReachedDestination())
    {
        // 到站事实先落到本阶段，EnsureOnFoot 会清驾驶目标；堵住出口只重试下车
        bVehicleLegComplete=true;
        bHasErrandDestination=false;
        bHasFollowDestination=false;
        if (bKeepMountedFollow)
        {
            ErrandVehicle->StopDriving();
            if (!IsCurrent() || !IsUsingErrandVehicle()) return false;
            bVehicleHasDriveGoal=false;
            // 当前近停条件不成立，继续按真实路径找停车点，不把当前主人位置记为已到
            bVehicleLegComplete=false;
        }
        else
        {
            const bool bOnFoot=EnsureOnFoot();
            if (!IsCurrent()) return false;
            if (!bOnFoot) { SetSuspension(TEXT("CompanionDismountBlocked")); return false; }
            SetSuspension(NAME_None);
            return true;
        }
    }
    if (bKeepMountedFollow)
        ErrandVehicle->SetFollowBike(Cast<ADeliveryMotorbike>(GetOwnerPawn()),PreferredDistance);
    if (bVehicleHasDriveGoal && !ErrandVehicle->GetDriveFailureReason().IsNone())
    {
        ReportVehiclePause(ErrandVehicle->GetDriveFailureReason());
        return false;
    }
    const float OwnerGoalShift=bKeepMountedFollow?FVector::Dist(Goal,VehicleLegGoal):FVector::Dist2D(Goal,VehicleLegGoal);
    if (bVehicleHasDriveGoal && OwnerGoalShift<GetTuning().RepathDistance) return false;
    if (!bKeepMountedFollow)
    {
        if (Now<NextVehicleNavigationAt) return false;
        NextVehicleNavigationAt=Now+GetTuning().NavigationInterval;
    }
    if (bKeepMountedFollow)
    {
        const FVector Here=ErrandVehicle->GetActorLocation();
        const auto* OwnerBike=Cast<ADeliveryMotorbike>(GetOwnerPawn());
        FVector TowardBike=(Here-Goal).GetSafeNormal2D();
        if (TowardBike.IsNearlyZero()) TowardBike=-ErrandVehicle->GetActorForwardVector().GetSafeNormal2D();
        // 玩家在车上时先沿通畅近路跟在车尾，复杂路段才查导航
        FVector FollowOffset=TowardBike;
        if (IsValid(OwnerBike) && FMath::Abs(OwnerBike->GetCurrentSpeed())>30.f)
        {
            // 车辆由扫掠位移驱动，跟车方向按服务器车速和车头计算
            const float Direction=OwnerBike->GetCurrentSpeed()>0.f?-1.f:1.f;
            FollowOffset=OwnerBike->GetActorForwardVector().GetSafeNormal2D()*Direction;
        }
        const FVector Behind=Goal+FollowOffset*PreferredDistance;
        const FVector ToBehind=(Behind-Here).GetSafeNormal2D();
        const auto TryFollowSegment=[&](const FVector& Destination)
        {
            if (!ErrandVehicle->TrySetDirectFollowDestination(Destination)) return false;
            if (!IsCurrent() || !IsUsingErrandVehicle()) return true;
            VehicleLegGoal=Goal;
            // 中途目标到达时还要继续接近主人，仍以实际停车目标判断跟到
            VehicleParkingGoal=Behind;
            bVehicleHasDriveGoal=true;
            SetSuspension(NAME_None);
            return true;
        };
        const double Remaining=FVector::Dist2D(Here,Behind);
        const double MinimumStep=ErrandVehicle->GetParkingAcceptanceRadius()+60.;
        if (Remaining>MinimumStep && TryFollowSegment(Here+ToBehind*FMath::Min(Remaining,2200.))) return false;

        // 直追车尾受阻时优先沿车已经通过的位置，避开弯道内侧的墙
        double Traveled=0.;
        FVector Later=Goal;
        int32 Attempts=0;
        for (int32 Index=FollowRoute.Num()-1;Index>=0;--Index)
        {
            const FVector Point=FollowRoute[Index];
            Traveled+=FVector::Dist2D(Later,Point);
            Later=Point;
            if (Traveled<PreferredDistance || FVector::Dist2D(Here,Point)<=MinimumStep ||
                FVector::Dist2D(Here,Point)>2200.) continue;
            // 已偏离路线时允许向后接入一个点，但不能向旧车位一路倒追
            if (Traveled>FVector::Dist2D(Here,Goal)+2200. || Attempts==6) break;
            ++Attempts;
            if (TryFollowSegment(Point))
            {
                if (IsCurrent() && IsUsingErrandVehicle()) FollowRoute.RemoveAt(0,Index,EAllowShrinking::No);
                return false;
            }
        }

        // 远处的停车点还没法确认时，可以先跑完一段已扫掠的近路，下次再继续跟车
        double PreviousStep=0.;
        for (const double Limit:{2200.,1100.,550.})
        {
            const double Step=FMath::Min(Remaining,Limit);
            if (Step<=MinimumStep) break;
            const bool bAlreadyTriedStraight=PreviousStep==0.;
            if (Step==PreviousStep) continue;
            PreviousStep=Step;
            // 先试直行，再试仍能缩短车距的两侧路线
            const bool bRoomToBypass=Step>=550. && Remaining>=PreferredDistance;
            for (const float Angle:{0.f,25.f,-25.f,50.f,-50.f})
            {
                if (Angle==0.f && bAlreadyTriedStraight) continue;
                if (Angle!=0.f && !bRoomToBypass) break;
                const FVector Nearby=Here+ToBehind.RotateAngleAxis(Angle,FVector::UpVector)*Step;
                if (TryFollowSegment(Nearby)) return false;
            }
        }
        // 主人可能站在台阶或路缘，找他周围可安全停车的位置
        const float ParkingTolerance=ErrandVehicle->GetParkingAcceptanceRadius();
        const float FollowStopRadius=FMath::Max(ReachRadius+90.f,PreferredDistance-ParkingTolerance);
        FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionVehicleFollowParking),false,Npc);
        Query.AddIgnoredActor(ErrandVehicle);
        Query.AddIgnoredActor(GetOwnerPawn());
        FCollisionObjectQueryParams Objects;
        for (const auto Channel:{ECC_WorldStatic,ECC_WorldDynamic,ECC_Pawn,ECC_Vehicle,ECC_PhysicsBody})
            Objects.AddObjectTypesToQuery(Channel);

        bool bFoundGround=false;
        bool bFoundVisibleCandidate=false;
        for (const float Angle:{0.f,45.f,-45.f,90.f,-90.f,135.f,-135.f,180.f})
        {
            const FVector Desired=Goal+TowardBike.RotateAngleAxis(Angle,FVector::UpVector)*FollowStopRadius;
            FNavLocation Parking;
            if (!Nav->ProjectPointToNavigation(Desired,Parking,FVector(100,100,180),&Npc->GetNavAgentPropertiesRef()) ||
                FVector::Dist2D(Parking.Location,Goal)>PreferredDistance ||
                FVector::Dist2D(Parking.Location,Goal)<ReachRadius+40.f ||
                FMath::Abs(Parking.Location.Z-Goal.Z)>180.f) continue;
            bFoundGround=true;
            FHitResult Hit;
            if (GetWorld()->LineTraceSingleByObjectType(Hit,Parking.Location+FVector(0,0,100),Goal,Objects,Query)) continue;
            bFoundVisibleCandidate=true;
            if (!ErrandVehicle->SetDriveDestination(Parking.Location, true))
            {
                if (!IsCurrent() || !IsUsingErrandVehicle()) return false;
                continue;
            }
            if (!IsCurrent() || !IsUsingErrandVehicle()) return false;
            VehicleLegGoal=Goal;
            VehicleParkingGoal=Parking.Location;
            bVehicleHasDriveGoal=true;
            SetSuspension(NAME_None);
            return false;
        }
        // 主人移动后的新目标暂时不可达时，先走完仍有效的当前路段
        if (ErrandVehicle->GetWorld()==GetWorld() && bVehicleHasDriveGoal && ErrandVehicle->HasActiveDriveRoute())
            return false;
        const FName Failure=!bFoundGround?FName(TEXT("CompanionVehicleFollowGroundMissing")):
            !bFoundVisibleCandidate?FName(TEXT("CompanionVehicleFollowParkingBlocked")):
            FName(TEXT("CompanionVehicleNoRoute"));
        ReportVehiclePause(Failure);
        return false;
    }
    // 取件点锚点可以在建筑内，终点先落在实际交互半径内的可走地面
    const float ProjectionRadius=FMath::Clamp(ReachRadius,30.f,1200.f);
    FNavLocation Approach;
    if (!Nav->ProjectPointToNavigation(Goal,Approach,FVector(ProjectionRadius,ProjectionRadius,250),&Npc->GetNavAgentPropertiesRef()) ||
        FVector::Dist2D(Approach.Location,Goal)>ProjectionRadius)
    {
        if (!bVehicleHasDriveGoal || !ErrandVehicle->HasActiveDriveRoute()) ReportVehiclePause(TEXT("CompanionVehicleNoRoute"));
        return false;
    }
    const auto* Path=Nav->FindPathToLocationSynchronously(GetWorld(),ErrandVehicle->GetActorLocation(),Approach.Location,Npc);
    if (!Path || !Path->IsValid() || Path->IsPartial() || Path->PathPoints.Num()<2)
    {
        if (!bVehicleHasDriveGoal || !ErrandVehicle->HasActiveDriveRoute()) ReportVehiclePause(TEXT("CompanionVehicleNoRoute"));
        return false;
    }
    // 从路线末端向外找停车点，最近一个挡住时尝试同一路线的上游位置
    const double MinimumSetback=FMath::Max(450.f,ReachRadius+180.f);
    const double ParkingInterval=200.;
    const double MaxSetback=MinimumSetback+3.*ParkingInterval;
    const FVector Start=ErrandVehicle->GetActorLocation();
    double Covered=0., NextSetback=MinimumSetback;
    FVector Parking=FVector::ZeroVector;
    bool bHasParking=false;
    for (int32 Index=Path->PathPoints.Num()-1;Index>0;--Index)
    {
        const FVector End=Path->PathPoints[Index];
        const FVector Segment=Path->PathPoints[Index-1]-End;
        const double Length=Segment.Size2D();
        if (Length<=UE_KINDA_SMALL_NUMBER) continue;
        for (;NextSetback<=MaxSetback && NextSetback<=Covered+Length;NextSetback+=ParkingInterval)
        {
            const FVector Candidate=End+Segment*((NextSetback-Covered)/Length);
            if (FMath::Abs(Candidate.Z-Start.Z)<=150. &&
                FVector::Dist2D(Candidate,Start)<=ErrandVehicle->GetParkingAcceptanceRadius()) continue;
            if (!ErrandVehicle->SetDriveDestination(Candidate,true)) continue;
            Parking=Candidate;
            bHasParking=true;
            break;
        }
        if (bHasParking || NextSetback>MaxSetback) break;
        Covered+=Length;
    }
    if (!IsCurrent() || !IsUsingErrandVehicle()) return false;
    if (!bHasParking)
    {
        // 两段目标间距太短时，只要当前位置可停车，就地转步行
        if (Path->GetPathLength()<=MinimumSetback && ErrandVehicle->CanParkAt(Start))
        {
            bVehicleLegComplete=true;
            VehicleLegGoal=Goal;
            const bool bOnFoot=EnsureOnFoot();
            if (!IsCurrent()) return false;
            SetSuspension(bOnFoot?NAME_None:FName(TEXT("CompanionDismountBlocked")));
            return bOnFoot;
        }
        if (bVehicleHasDriveGoal && ErrandVehicle->HasActiveDriveRoute()) return false;
        ReportVehiclePause(TEXT("CompanionVehicleParkingBlocked"));
        return false;
    }
    VehicleLegGoal=Goal;
    VehicleParkingGoal=Parking;
    bVehicleHasDriveGoal=true;
    SetSuspension(NAME_None);
    if (bForErrand) LastErrandPause=NAME_None;
    if (bForErrand && !bErrandTravelBudgetAdded)
    {
        const float WalkingSpeed=Npc->GetCharacterMovement()->MaxWalkSpeed;
        const double Length=Path->GetPathLength();
        if (FMath::IsFinite(WalkingSpeed) && WalkingSpeed>0 && FMath::IsFinite(Length) && Length>=0)
        {
            ErrandTimeLimitSeconds+=Length/WalkingSpeed;
            bErrandTravelBudgetAdded=true;
        }
    }
    return false;
}
