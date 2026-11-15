#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "Character/DeliveryCharacter.h"
#include "Delivery.h"
#include "Vehicle/DeliveryCompanionMotorbike.h"
#include "AIController.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "Navigation/PathFollowingComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "NPC/DeliveryGrandpa.h"
#include "World/DeliveryHome.h"

namespace
{
    constexpr float FollowBikeSampleDistance=180.f;

    bool IsFollowBikeRouteFailure(FName Reason)
    {
        return Reason==TEXT("VehicleBlocked") || Reason==TEXT("PathInvalidated") || Reason==TEXT("WalkOnlyDoorway") ||
            Reason==TEXT("ParkingBlocked") || Reason==TEXT("CompanionVehicleNoRoute") ||
            Reason==TEXT("CompanionVehicleFollowGroundMissing") || Reason==TEXT("CompanionVehicleFollowParkingBlocked");
    }

    bool IsOwnerDrivingBike(const ADeliveryMotorbike* Bike,const APlayerState* Owner)
    {
        const auto* Driver=IsValid(Bike) && !Bike->IsActorBeingDestroyed()?Bike->GetDriver():nullptr;
        return Owner && IsValid(Driver) && !Driver->IsActorBeingDestroyed() &&
            Driver->GetTaskPlayerState()==Owner && Driver->GetCurrentVehicle()==Bike;
    }

    TOptional<FVector> FindFollowWalkingGoal(const ADeliveryCompanionCharacter& Npc,
        const ADeliveryCharacter& Owner,UNavigationSystemV1& Navigation)
    {
        const auto& Agent=Npc.GetNavAgentPropertiesRef();
        FNavLocation Goal;
        const FVector OwnerFeet=Owner.GetActorLocation()-FVector(0,0,Owner.GetSimpleCollisionHalfHeight());
        if (!Navigation.ProjectPointToNavigation(OwnerFeet,Goal,FVector(45,45,75),&Agent)) return {};
        return Goal.Location;
    }
}

bool UDeliveryCompanionComponent::IsUsingErrandVehicle() const
{
    return IsValid(ErrandVehicle) && ErrandVehicle->GetReservedCompanion()==GetOwner() && ErrandVehicle->IsMounted();
}

ADeliveryCompanionMotorbike* UDeliveryCompanionComponent::GetFollowMotorbike() const
{
    return ShouldRideForFollow(GetOwnerPawn()) && !HasActiveErrand() &&
        VehicleTravelPurpose==EDeliveryCompanionTravelPurpose::Follow && IsUsingErrandVehicle()?ErrandVehicle.Get():nullptr;
}

bool UDeliveryCompanionComponent::ShouldRideForFollow(const APawn* OwnerPawn) const
{
    if (Mode!=EDeliveryCompanionMode::Follow || FollowTravelMode==EDeliveryCompanionTravelMode::Walk ||
        FollowTravelMode==EDeliveryCompanionTravelMode::Unspecified ||
        !IsValid(OwnerPawn) || OwnerPawn->IsActorBeingDestroyed() || OwnerPawn->GetWorld()!=GetWorld() ||
        OwnerPawn!=GetOwnerPawn()) return false;
    if (bFollowWalkAfterOwnerRide && OwnerPawn->IsA<ADeliveryCharacter>() && !IsUsingErrandVehicle()) return false;
    // 主人下车后，正在骑行的二狗先沿原车辆分支找能继续步行的停车点
    return FollowTravelMode==EDeliveryCompanionTravelMode::OwnVehicle || OwnerPawn->IsA<ADeliveryMotorbike>() ||
        (VehicleTravelPurpose==EDeliveryCompanionTravelPurpose::Follow && IsUsingErrandVehicle());
}

void UDeliveryCompanionComponent::RefreshFollowOwnerTravel(const APawn* Pawn)
{
    // 交接控制器期间可能短暂没有 Pawn，等同一主人出现实际步行对象再记录下车
    if (!IsValid(Pawn)) return;
    if (IsOwnerDrivingBike(Cast<ADeliveryMotorbike>(Pawn),OwnerPlayerState))
    {
        bFollowOwnerWasRiding=true;
        bFollowWalkAfterOwnerRide=false;
    }
    else if (Pawn->IsA<ADeliveryCharacter>())
    {
        if (bFollowOwnerWasRiding) bFollowWalkAfterOwnerRide=true;
        bFollowOwnerWasRiding=false;
    }
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

bool UDeliveryCompanionComponent::IsVehicleBoardingSpotUsable(const FVector& Feet) const
{
    const auto* Npc=GetCompanion();
    if (!IsValid(Npc) || !IsValid(ErrandVehicle) || ErrandVehicle->IsActorBeingDestroyed() ||
        ErrandVehicle->GetWorld()!=GetWorld() || ErrandVehicle->GetReservedCompanion()!=Npc || Feet.ContainsNaN()) return false;
    const FVector FeetToCenter(0,0,Npc->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
    return IsDestinationClear(Feet) && ErrandVehicle->CanBoardFromLocation(Feet+FeetToCenter);
}

bool UDeliveryCompanionComponent::KeepVehicleApproach(UPathFollowingComponent& Path,double Now)
{
    if (!bVehicleOwnsApproach || !VehicleApproachMoveId.IsValid() || Path.GetCurrentRequestId()!=VehicleApproachMoveId ||
        Path.GetStatus()==EPathFollowingStatus::Idle) return false;
    if (Now<NextVehicleNavigationAt) return true;
    NextVehicleNavigationAt=Now+GetTuning().NavigationInterval;
    if (IsVehicleBoardingSpotUsable(VehicleApproachFeet)) return true;

    // 停车点被占用或车已移走时，当次重选仍能接近车座的落脚点
    UE_LOG(LogDelivery,Log,TEXT("CompanionVehicleApproachReplan npc=%s revision=%d move=%u feet=%s"),
        *GetNameSafe(GetOwner()),CommandRevision,VehicleApproachMoveId.GetID(),*VehicleApproachFeet.ToCompactString());
    NextVehicleNavigationAt=0;
    StopVehicleApproach();
    return false;
}

void UDeliveryCompanionComponent::StopVehicleApproach()
{
    const FAIRequestID OldId=VehicleApproachMoveId;
    const bool bOwned=bVehicleOwnsApproach;
    bVehicleOwnsApproach=false;
    VehicleApproachMoveId=FAIRequestID::InvalidRequest;
    VehicleApproachFeet=FVector::ZeroVector;
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
    ClearFollowMoveWatch();
    ClearDeliveryApproachWatch();
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

void UDeliveryCompanionComponent::StopVehicleTravel(EDeliveryCompanionTravelPurpose Purpose,bool bKeepVehicleSeat)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || Purpose==EDeliveryCompanionTravelPurpose::None || VehicleTravelPurpose!=Purpose) return;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionMotorbike> ExpectedBike(ErrandVehicle.Get());
    const int32 Revision=CommandRevision;
    bVehicleHasDriveGoal=false;
    if (Purpose==EDeliveryCompanionTravelPurpose::Follow)
    {
        FollowVehicleBlockedAt=-1.0;
        FollowVehicleRoutePause={};
    }
    StopVehicleApproach();
    if (!WeakThis.IsValid() || CommandRevision!=Revision || VehicleTravelPurpose!=Purpose) return;
    if (bKeepVehicleSeat)
    {
        bVehicleLegComplete=false;
        // 轻击或主人换控制对象时停车，恢复后从原座位继续处理跟随
        if (ExpectedBike.IsValid() && ExpectedBike.Get()==ErrandVehicle && IsUsingErrandVehicle()) ErrandVehicle->StopDriving();
        return;
    }
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
    VehicleFollowOwnerPawn.Reset();
    FollowRoute.Reset();
    RejectedFollowParking.Reset();
    RejectedErrandParking.Reset();
    NextVehicleNavigationAt=0;
    FollowOnFootRevision=INDEX_NONE;
    FollowVehicleBlockedAt=-1.0;
    FollowVehicleRoutePause={};
    StopVehicleApproach();
    if (!WeakThis.IsValid() || CommandRevision!=Revision || VehicleTravelPurpose!=EDeliveryCompanionTravelPurpose::None) return;
    EnsureOnFoot();
}

void UDeliveryCompanionComponent::SampleFollowBikeRoute(ADeliveryMotorbike* Bike)
{
    if (!IsValid(Bike) || Bike->IsActorBeingDestroyed()) { FollowRoute.Reset(); return; }
    const FVector Position=Bike->GetActorLocation();
    if (Position.ContainsNaN()) return;
    if (!FollowRoute.IsEmpty())
    {
        const double Distance=FVector::Dist(Position,FollowRoute.Last());
        if (Distance<FollowBikeSampleDistance) return;
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
    auto* OwnerBike=Purpose==EDeliveryCompanionTravelPurpose::Follow && IsFollowBikeRouteFailure(Reason)?
        Cast<ADeliveryMotorbike>(GetOwnerPawn()):nullptr;
    const TWeakObjectPtr<ADeliveryMotorbike> PausedOwnerBike(OwnerBike);
    const FVector PausedOwnerGoal=IsValid(OwnerBike)?OwnerBike->GetActorLocation():FVector::ZeroVector;
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
    const double Now=GetWorld()->GetTimeSeconds();
    NextVehicleNavigationAt=Now+GetTuning().BlockedRetrySeconds;
    if (PausedOwnerBike.IsValid() && GetOwnerPawn()==OwnerBike && VehicleFollowOwnerPawn.Get()==OwnerBike &&
        IsUsingErrandVehicle() && CanCommand(OwnerPlayerState) && IsOwnerDrivingBike(OwnerBike,OwnerPlayerState) &&
        !PausedOwnerGoal.ContainsNaN())
    {
        if (FollowVehicleRoutePause.bUsedEarlyRetry && FollowVehicleRoutePause.Revision==Revision &&
            FollowVehicleRoutePause.OwnerBike.Get()==OwnerBike && Now<FollowVehicleRoutePause.RetryAt)
        {
            // 提前重算也失败时等原定时刻，不能重开机会或延长原有等待
            NextVehicleNavigationAt=FollowVehicleRoutePause.RetryAt;
        }
        else FollowVehicleRoutePause={PausedOwnerBike,PausedOwnerGoal,Revision,Now,NextVehicleNavigationAt,false};
    }
    else FollowVehicleRoutePause={};
    if (Purpose==EDeliveryCompanionTravelPurpose::Follow && IsUsingErrandVehicle()) ErrandVehicle->ReportFollowBlocked();
}

bool UDeliveryCompanionComponent::UpdateVehicleTravel(const FVector& RequestedGoal,float ReachRadius,EDeliveryCompanionTravelPurpose Purpose,
    const FVector* RequiredWalkGoal)
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority() || !CanCommand(OwnerPlayerState) || Purpose==EDeliveryCompanionTravelPurpose::None ||
        !FMath::IsFinite(ReachRadius)) return false;
    const bool bForErrand=Purpose==EDeliveryCompanionTravelPurpose::Errand;
    const bool bFollowing=Purpose==EDeliveryCompanionTravelPurpose::Follow;
    auto* FollowOwnerPawn=bFollowing?GetOwnerPawn():nullptr;
    if (bFollowing && (Mode!=EDeliveryCompanionMode::Follow || !IsValid(FollowOwnerPawn) ||
        FollowOwnerPawn->IsActorBeingDestroyed() || FollowOwnerPawn->GetWorld()!=GetWorld())) return false;
    if (bFollowing) RefreshFollowOwnerTravel(FollowOwnerPawn);
    const TWeakObjectPtr<APawn> ExpectedOwnerPawn(FollowOwnerPawn);
    // 目标坐标和跟车快照从同一个 Pawn 读取
    const FVector Goal=FollowOwnerPawn?FollowOwnerPawn->GetActorLocation():RequestedGoal;
    if (Goal.ContainsNaN()) return false;
    if (bForErrand && (!HasActiveErrand() || Errand.CommandRevision!=CommandRevision)) return false;
    const auto Phase=bForErrand?Errand.Phase:EDeliveryCompanionErrandPhase::Idle;
    const bool bRequiresWalkExit=RequiredWalkGoal && Phase==EDeliveryCompanionErrandPhase::GoToDelivery;
    if (bRequiresWalkExit && RequiredWalkGoal->ContainsNaN()) return false;
    const int32 Revision=CommandRevision;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const auto IsCurrent=[&]()
    {
        return WeakThis.IsValid() && WeakNpc.IsValid() && !Npc->IsActorBeingDestroyed() && CommandRevision==Revision &&
            (!bFollowing || (Mode==EDeliveryCompanionMode::Follow && ExpectedOwnerPawn.IsValid() &&
                !FollowOwnerPawn->IsActorBeingDestroyed() &&
                TWeakObjectPtr<APawn>(GetOwnerPawn()).HasSameIndexAndSerialNumber(ExpectedOwnerPawn))) &&
            (!bForErrand || (HasActiveErrand() && Errand.CommandRevision==Revision && Errand.Phase==Phase));
    };
    // 跑腿出行方式未确认时等待安排，跟随单独依据主人当前控制对象
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
    const bool bOwnVehicleAvailable=IsValid(ErrandVehicle) && !ErrandVehicle->IsActorBeingDestroyed() &&
        ErrandVehicle->GetWorld()==GetWorld() &&
        (!ErrandVehicle->GetReservedCompanion() || ErrandVehicle->GetReservedCompanion()==Npc);
    const bool bHandoffTravel=bFollowing && OwnerHandoffFollowRevision==Revision && !bUnspecified;
    const auto FollowPolicy=bHandoffTravel?TravelMode:FollowTravelMode;
    const bool bRideForFollow=bHandoffTravel?(FollowPolicy==EDeliveryCompanionTravelMode::OwnVehicle ||
        FollowPolicy==EDeliveryCompanionTravelMode::Automatic):ShouldRideForFollow(FollowOwnerPawn);
    const bool bKeepMountedFollow=bFollowing && bRideForFollow &&
        (bOwnVehicleAvailable || FollowPolicy==EDeliveryCompanionTravelMode::OwnVehicle);
    if (bFollowing && VehicleTravelPurpose==EDeliveryCompanionTravelPurpose::Follow &&
        !VehicleFollowOwnerPawn.HasSameIndexAndSerialNumber(ExpectedOwnerPawn))
    {
        // 主人换控制对象后清掉原车尾、停车状态和值守计时，包括还未上车的情况
        VehicleFollowOwnerPawn=ExpectedOwnerPawn;
        FollowRoute.Reset();
        RejectedFollowParking.Reset();
        bVehicleHasDriveGoal=false;
        bVehicleLegComplete=false;
        NextVehicleNavigationAt=0;
        NextNavigationAt=0;
        FollowOnFootRevision=INDEX_NONE;
        FollowVehicleBlockedAt=-1.0;
        FollowVehicleRoutePause={};
        if (IsUsingErrandVehicle()) ErrandVehicle->StopDriving();
        if (!IsCurrent()) return false;
    }
    const bool bFollowOnFoot=bKeepMountedFollow && FollowOnFootRevision==Revision &&
        VehicleFollowOwnerPawn.HasSameIndexAndSerialNumber(ExpectedOwnerPawn);
    // 能当面交接时先下车办完当前交接，后续路段仍使用主人选择的出行方式
    const bool bWalk=bFollowing?(!bKeepMountedFollow || bFollowOnFoot):
        bTransferNearby || bLocalItemTransfer || TravelMode==EDeliveryCompanionTravelMode::Walk || bUnspecified ||
        (TravelMode==EDeliveryCompanionTravelMode::Automatic && !bOwnVehicleAvailable);
    if (bWalk)
    {
        if (bFollowing) FollowVehicleRoutePause={};
        StopVehicleApproach();
        if (!IsCurrent()) return false;
        const bool bOnFoot=EnsureOnFoot();
        if (!IsCurrent()) return false;
        if (!bOnFoot) SetSuspension(TEXT("CompanionDismountBlocked"));
        // 下车完成只能解除下车暂停，步行受阻由步行分支确认恢复
        else if (SuspensionReason==TEXT("CompanionDismountBlocked")) SetSuspension(NAME_None);
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
    auto* OwnerBike=bKeepMountedFollow?Cast<ADeliveryMotorbike>(FollowOwnerPawn):nullptr;
    if (bKeepMountedFollow)
    {
        VehicleFollowOwnerPawn=ExpectedOwnerPawn;
        SampleFollowBikeRoute(OwnerBike);
    }
    const float Distance=FVector::Dist2D(Npc->GetActorLocation(),Goal);
    // 跟车使用近距离停车间距，独立出行采用车辆的默认起骑距离
    const float PreferredDistance=(bKeepMountedFollow || TravelMode==EDeliveryCompanionTravelMode::OwnVehicle)?
        FMath::Max(450.f,ReachRadius+180.f):(IsValid(ErrandVehicle)?ErrandVehicle->GetPreferredTravelDistance():1200.f);
    const double Now=GetWorld()->GetTimeSeconds();
    if (bRequiresWalkExit && (Now>=RejectedErrandParkingUntil ||
        FVector::DistSquared(*RequiredWalkGoal,ErrandParkingWalkGoal)>=FMath::Square(GetTuning().RepathDistance)))
        RejectedErrandParking.Reset();
    const auto IsSameParking=[&](const FVector& First,const FVector& Second)
    {
        return FMath::Abs(First.Z-Second.Z)<=75.f && FVector::DistSquared2D(First,Second)<=
            FMath::Square(ErrandVehicle->GetParkingAcceptanceRadius()+25.f);
    };
    TOptional<FVector> ErrandWalkingGoal;
    bool bCheckedErrandWalkingGoal=false;
    const auto GetErrandWalkingGoal=[&]() -> const TOptional<FVector>&
    {
        if (!bCheckedErrandWalkingGoal)
        {
            bCheckedErrandWalkingGoal=true;
            auto* Navigation=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
            if (!Navigation) return ErrandWalkingGoal;
            const auto* Home=ADeliveryHome::FindSharedHome(this);
            const bool bGrandpaStreet=IsValid(Home) && IsValid(Home->ReturnAnchor) && IsValid(Home->FormerResident) &&
                ResolveErrandLocation(false)==Home->FormerResident.Get();
            const double WalkRadius=FMath::Clamp(static_cast<double>(ReachRadius),65.,150.);
            const auto TryProjection=[&](const FVector& Point,const FVector& Extent)
            {
                FNavLocation Projected;
                if (!Navigation->ProjectPointToNavigation(Point,Projected,Extent,&Npc->GetNavAgentPropertiesRef()) ||
                    FMath::Abs(Projected.Location.Z-RequiredWalkGoal->Z)>75. ||
                    FVector::DistSquared2D(Projected.Location,*RequiredWalkGoal)>FMath::Square(WalkRadius) ||
                    (bGrandpaStreet && !IsSafeDeliveryDoorWait(*Home,Projected.Location))) return false;
                ErrandWalkingGoal=Projected.Location;
                return true;
            };
            if (!TryProjection(*RequiredWalkGoal,FVector(65,65,75)) && bGrandpaStreet)
            {
                // 门外候车点可能正落在台阶或导航边缘，补查同层街侧的实际可站位置
                const FVector Outward=(*RequiredWalkGoal-Home->ReturnAnchor->GetComponentLocation()).GetSafeNormal2D();
                const FVector Side=FVector::CrossProduct(FVector::UpVector,Outward);
                for (const FVector& Offset:{Outward*90.,Side*90.,-Side*90.,Outward*90.+Side*65.,Outward*90.-Side*65.})
                    if (TryProjection(*RequiredWalkGoal+Offset,FVector(35,35,75))) break;
            }
        }
        return ErrandWalkingGoal;
    };
    const auto TryFinishVehicleLegOnFoot=[&]()
    {
        if (!bRequiresWalkExit || !IsUsingErrandVehicle()) return EnsureOnFoot();
        const TWeakObjectPtr<ADeliveryCompanionMotorbike> ExpectedBike(ErrandVehicle.Get());
        const auto& WalkingGoal=GetErrandWalkingGoal();
        if (!WalkingGoal.IsSet() || FMath::Abs(ErrandVehicle->GetCurrentSpeed())>30.f ||
            !ErrandVehicle->CanParkAt(ErrandVehicle->GetActorLocation())) return false;
        const bool bOnFoot=ErrandVehicle->TryDismountForWalk(*WalkingGoal);
        if (!IsCurrent() || !ExpectedBike.IsValid() || ErrandVehicle.Get()!=ExpectedBike.Get()) return false;
        if (bOnFoot)
        {
            bVehicleHasDriveGoal=false;
            bVehicleNeedsDismount=false;
            RejectedErrandParking.Reset();
        }
        return bOnFoot;
    };
    const auto RejectErrandWalkParking=[&]()
    {
        if (!bRequiresWalkExit || !IsUsingErrandVehicle()) return;
        if (RejectedErrandParking.IsEmpty())
        {
            ErrandParkingWalkGoal=*RequiredWalkGoal;
            RejectedErrandParkingUntil=Now+2.*GetTuning().BlockedRetrySeconds;
        }
        const FVector ParkingFeet=ErrandVehicle->GetActorLocation()-FVector(0,0,ErrandVehicle->GetSimpleCollisionHalfHeight());
        if (!RejectedErrandParking.ContainsByPredicate([&](const FVector& Rejected){ return IsSameParking(Rejected,ParkingFeet); }))
        {
            if (RejectedErrandParking.Num()==4) RejectedErrandParking.RemoveAt(0,EAllowShrinking::No);
            RejectedErrandParking.Add(ParkingFeet);
            UE_LOG(LogDelivery,Log,TEXT("CompanionErrandWalkExitRejected npc=%s revision=%d parking=%s"),
                *GetNameSafe(Npc),Revision,*ParkingFeet.ToCompactString());
        }
        bVehicleLegComplete=false;
        bVehicleHasDriveGoal=false;
        ErrandVehicle->StopDriving();
        if (IsCurrent()) SetSuspension(TEXT("CompanionDismountBlocked"));
    };
    // 骑乘中的近停检查沿原导航间隔执行，停车后也复查高度和遮挡变化
    if (FollowVehicleRoutePause.Revision!=INDEX_NONE && Now>=FollowVehicleRoutePause.RetryAt)
        FollowVehicleRoutePause={};
    if (bKeepMountedFollow && IsUsingErrandVehicle() && Now<NextVehicleNavigationAt)
    {
        auto& Pause=FollowVehicleRoutePause;
        if (!IsOwnerDrivingBike(OwnerBike,OwnerPlayerState) || Pause.OwnerBike.Get()!=OwnerBike ||
            Pause.Revision!=Revision || Pause.bUsedEarlyRetry || !IsFollowBikeRouteFailure(SuspensionReason) ||
            Now<Pause.ReportedAt+GetTuning().NavigationInterval || !(FMath::Abs(OwnerBike->GetCurrentSpeed())>30.f) ||
            FVector::DistSquared2D(Goal,Pause.OwnerGoal)<FMath::Square(FollowBikeSampleDistance)) return false;
        Pause.bUsedEarlyRetry=true;
        UE_LOG(LogDelivery,Log,TEXT("CompanionFollowEarlyRepath npc=%s revision=%d prior=%s owner=%s"),
            *GetNameSafe(Npc),Revision,*SuspensionReason.ToString(),*GetNameSafe(OwnerBike));
    }
    const auto IsAtDrivenFollowParking=[&]()
    {
        return IsUsingErrandVehicle() && (bVehicleHasDriveGoal || bVehicleLegComplete) &&
            ErrandVehicle->HasReachedDestination() &&
            FVector::Dist(Goal,VehicleLegGoal)<=GetTuning().RepathDistance &&
            FVector::Dist2D(ErrandVehicle->GetActorLocation(),VehicleParkingGoal)<=ErrandVehicle->GetParkingAcceptanceRadius();
    };
    const auto CanWaitMountedNearOwner=[&]()
    {
        if (!IsUsingErrandVehicle() || !IsCurrent()) return false;
        if (OwnerBike && FMath::Abs(OwnerBike->GetCurrentSpeed())>30.f) return false;
        const FVector Here=Npc->GetActorLocation();
        if (FMath::Abs(Here.Z-Goal.Z)>180.f ||
            (FVector::Dist2D(Here,Goal)>PreferredDistance && !IsAtDrivenFollowParking())) return false;
        FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionVehicleFollowWait),false,Npc);
        Query.AddIgnoredActor(ErrandVehicle);
        Query.AddIgnoredActor(FollowOwnerPawn);
        FCollisionObjectQueryParams Objects;
        for (const auto Channel:{ECC_WorldStatic,ECC_WorldDynamic,ECC_Pawn,ECC_Vehicle,ECC_PhysicsBody})
            Objects.AddObjectTypesToQuery(Channel);
        FHitResult Hit;
        return !GetWorld()->LineTraceSingleByObjectType(Hit,Here,Goal,Objects,Query) &&
            ErrandVehicle->CanParkAt(ErrandVehicle->GetActorLocation());
    };
    if (bVehicleLegComplete && !bKeepMountedFollow)
    {
        const bool bOnFoot=TryFinishVehicleLegOnFoot();
        if (!IsCurrent()) return false;
        if (!bOnFoot)
        {
            if (!bRequiresWalkExit) { SetSuspension(TEXT("CompanionDismountBlocked")); return false; }
            RejectErrandWalkParking();
            if (!IsCurrent()) return false;
        }
        // 主人完成这段行程后又走远，可以再次去真实停车处取车
        else if (Distance>PreferredDistance && FVector::Dist2D(Goal,VehicleLegGoal)>GetTuning().RepathDistance)
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
    if (!AI || !Nav)
    {
        ReportVehiclePause(TEXT("CompanionVehicleNavigationUnavailable"));
        return false;
    }
    if (!IsUsingErrandVehicle())
    {
        if (Now<NextVehicleNavigationAt && !bVehicleOwnsApproach) return false;
        auto* Following=AI->GetPathFollowingComponent();
        if (!Following) return false;
        const TWeakObjectPtr<AAIController> WeakAI(AI);
        const TWeakObjectPtr<UPathFollowingComponent> WeakFollowing(Following);
        const TWeakObjectPtr<ADeliveryCompanionMotorbike> BoardingBike(ErrandVehicle.Get());
        const auto IsApproachCurrent=[&]()
        {
            return IsCurrent() && WeakAI.IsValid() && WeakFollowing.IsValid() && Npc->GetController()==AI &&
                AI->GetPathFollowingComponent()==Following && BoardingBike.IsValid() && BoardingBike.Get()==ErrandVehicle &&
                !ErrandVehicle->IsActorBeingDestroyed() && ErrandVehicle->GetReservedCompanion()==Npc && VehicleTravelPurpose==Purpose;
        };
        // 只有当前取车动作可以接管导航，保留同步通知期间其他动作发出的新请求
        if (Following->GetStatus()!=EPathFollowingStatus::Idle &&
            (!bVehicleOwnsApproach || Following->GetCurrentRequestId()!=VehicleApproachMoveId)) return false;
        const FVector Here=Npc->GetActorLocation();
        if (ErrandVehicle->CanBoardFromLocation(Here))
        {
            StopVehicleApproach();
            if (!IsApproachCurrent() || Following->GetStatus()!=EPathFollowingStatus::Idle) return false;
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
            if (KeepVehicleApproach(*Following,Now)) return false;
            if (!IsApproachCurrent() || Following->GetStatus()!=EPathFollowingStatus::Idle) return false;
            if (Now<NextVehicleNavigationAt) return false;
            NextVehicleNavigationAt=Now+GetTuning().NavigationInterval;
            FVector NearSide=ErrandVehicle->GetBoardingLocation();
            FVector FarSide=ErrandVehicle->GetOppositeBoardingLocation();
            if (FVector::DistSquared2D(Here,NearSide)>FVector::DistSquared2D(Here,FarSide)) Swap(NearSide,FarSide);
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
                        !IsVehicleBoardingSpotUsable(Board.Location)) continue;
                    const auto* WalkPath=Nav->FindPathToLocationSynchronously(GetWorld(),Npc->GetNavAgentLocation(),Board.Location,Npc);
                    if (!WalkPath || !WalkPath->IsValid() || WalkPath->IsPartial()) continue;
                    FAIMoveRequest Request(Board.Location);
                    Request.SetAcceptanceRadius(15); Request.SetAllowPartialPath(false);
                    Request.SetReachTestIncludesAgentRadius(false); Request.SetReachTestIncludesGoalRadius(false);
                    const auto Result=AI->MoveTo(Request);
                    if (!IsApproachCurrent())
                    {
                        if (WeakThis.IsValid() && WeakFollowing.IsValid() && Result.Code==EPathFollowingRequestResult::RequestSuccessful &&
                            Following->GetCurrentRequestId()==Result.MoveId)
                            Following->AbortMove(*this,FPathFollowingResultFlags::UserAbort,Result.MoveId);
                        return false;
                    }
                    // 当前位置已确认无法上车，导航坐标容差返回已到时继续试其他候选
                    if (Result.Code!=EPathFollowingRequestResult::RequestSuccessful) continue;
                    if (Following->GetCurrentRequestId()!=Result.MoveId) return false;
                    VehicleApproachMoveId=Result.MoveId;
                    VehicleApproachFeet=Board.Location;
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

    const auto* OwnerOnFoot=Cast<ADeliveryCharacter>(FollowOwnerPawn);
    bool bSeekingWalkParking=false;
    TOptional<FVector> OwnerWalkingGoal;
    bool bCheckedOwnerWalkingGoal=false;
    const auto GetOwnerWalkingGoal=[&]() -> const TOptional<FVector>&
    {
        if (!bCheckedOwnerWalkingGoal)
        {
            bCheckedOwnerWalkingGoal=true;
            if (IsValid(OwnerOnFoot)) OwnerWalkingGoal=FindFollowWalkingGoal(*Npc,*OwnerOnFoot,*Nav);
            // 主人经过已开的门洞时，步行终点仍需投影到主人这一侧
            if (OwnerWalkingGoal.IsSet() && IsAcrossOpenFollowDoorway(OwnerOnFoot,*OwnerWalkingGoal)) OwnerWalkingGoal.Reset();
        }
        return OwnerWalkingGoal;
    };
    const auto ContinueOnFootAtParking=[&](FName Reason)->TOptional<bool>
    {
        if (!IsValid(OwnerOnFoot) || !IsUsingErrandVehicle() || FMath::Abs(ErrandVehicle->GetCurrentSpeed())>30.f ||
            !ErrandVehicle->CanParkAt(ErrandVehicle->GetActorLocation())) return {};
        const auto& WalkingGoal=GetOwnerWalkingGoal();
        const TWeakObjectPtr<ADeliveryCompanionMotorbike> ExpectedBike(ErrandVehicle.Get());
        bool bOnFoot=WalkingGoal.IsSet() && ErrandVehicle->TryDismountForWalk(WalkingGoal.GetValue());
        if (!IsCurrent() || !ExpectedBike.IsValid() || ErrandVehicle.Get()!=ExpectedBike.Get()) return false;
        // 主人已实际下车，远处目标未投影时也可以先从安全车位落脚
        if (!bOnFoot && bFollowWalkAfterOwnerRide) bOnFoot=ErrandVehicle->TryDismountIfClear();
        if (!IsCurrent() || !ExpectedBike.IsValid() || ErrandVehicle.Get()!=ExpectedBike.Get()) return false;
        if (!bOnFoot) return {};
        // 从真实停车处继续当前 Follow，重新上车由主人换 Pawn 或新指令启动
        FollowOnFootRevision=Revision;
        FollowVehicleRoutePause={};
        bVehicleHasDriveGoal=false;
        bVehicleLegComplete=false;
        bVehicleNeedsDismount=false;
        RejectedFollowParking.Reset();
        NextNavigationAt=0;
        SetSuspension(NAME_None);
        UE_LOG(LogDelivery,Log,TEXT("CompanionFollowParkToWalk npc=%s revision=%d reason=%s exited=1"),
            *GetNameSafe(Npc),Revision,*Reason.ToString());
        return true;
    };
    if (bKeepMountedFollow)
    {
        const FVector Here=ErrandVehicle->GetActorLocation();
        if (FollowVehicleBlockedAt>=0.0 && FVector::DistSquared2D(FollowVehicleBlockedLocation,Here)>=
            FMath::Square(GetTuning().RepathDistance)) FollowVehicleBlockedAt=-1.0;
        NextVehicleNavigationAt=Now+GetTuning().NavigationInterval;
        const FVector ParkingFeet=Here-FVector(0,0,ErrandVehicle->GetSimpleCollisionHalfHeight());
        const bool bNeedsDoorwayOnFoot=Distance<=PreferredDistance && IsAcrossOpenFollowDoorway(OwnerOnFoot,ParkingFeet);
        const bool bOwnerSwitchedToWalking=IsValid(OwnerOnFoot) && (bFollowWalkAfterOwnerRide ||
            (FollowPolicy!=EDeliveryCompanionTravelMode::OwnVehicle && Distance<=PreferredDistance));
        if (bNeedsDoorwayOnFoot || bOwnerSwitchedToWalking)
        {
            const auto Result=ContinueOnFootAtParking(bNeedsDoorwayOnFoot?TEXT("OpenFollowDoorway"):TEXT("OwnerOnFoot"));
            if (Result.IsSet()) return Result.GetValue();
            bSeekingWalkParking=true;
            if (RejectedFollowParking.IsEmpty() || Now>=RejectedFollowParkingUntil ||
                FVector::DistSquared(Goal,RejectedFollowParkingOwnerGoal)>=FMath::Square(GetTuning().RepathDistance))
            {
                RejectedFollowParking.Reset();
                RejectedFollowParkingOwnerGoal=Goal;
                RejectedFollowParkingUntil=Now+2.*GetTuning().BlockedRetrySeconds;
            }
            // 正在驶向另一处时保留车路，停稳后才记下没能下车的位置
            if (FMath::Abs(ErrandVehicle->GetCurrentSpeed())<=30.f &&
                !RejectedFollowParking.ContainsByPredicate([&](const FVector& Rejected){ return IsSameParking(Rejected,ParkingFeet); }))
            {
                if (RejectedFollowParking.Num()==4) RejectedFollowParking.RemoveAt(0,EAllowShrinking::No);
                RejectedFollowParking.Add(ParkingFeet);
                UE_LOG(LogDelivery,Log,TEXT("CompanionFollowExitRejected npc=%s revision=%d parking=%s owner=%s"),
                    *GetNameSafe(Npc),Revision,*ParkingFeet.ToCompactString(),*Goal.ToCompactString());
            }
        }
        // 保留这段真实到站目标，不能每次等候都刷新它而漏掉主人累积走远
        const bool bKeepParkingGoal=IsAtDrivenFollowParking();
        if (!bSeekingWalkParking && CanWaitMountedNearOwner())
        {
            ErrandVehicle->StopDriving();
            if (!IsCurrent() || !IsUsingErrandVehicle()) return false;
            bVehicleHasDriveGoal=false;
            bVehicleLegComplete=true;
            FollowVehicleBlockedAt=-1.0;
            FollowVehicleRoutePause={};
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
            const bool bOnFoot=TryFinishVehicleLegOnFoot();
            if (!IsCurrent()) return false;
            if (bOnFoot) { SetSuspension(NAME_None); return true; }
            if (!bRequiresWalkExit) { SetSuspension(TEXT("CompanionDismountBlocked")); return false; }
            RejectErrandWalkParking();
            if (!IsCurrent()) return false;
        }
    }
    if (bKeepMountedFollow)
        ErrandVehicle->SetFollowBike(OwnerBike,PreferredDistance);
    const auto PauseOrWalk=[&](FName Reason,bool bCheckWalkingPath)
    {
        if (bKeepMountedFollow && IsUsingErrandVehicle() && IsValid(OwnerOnFoot) && bCheckWalkingPath)
        {
            const FVector Here=ErrandVehicle->GetActorLocation();
            // 一次重算接受但车实际没走动时继续累计受阻，错过窄门后仍可改走
            if (FollowVehicleBlockedAt<0.0 || FVector::DistSquared2D(FollowVehicleBlockedLocation,Here)>=
                FMath::Square(GetTuning().RepathDistance))
            {
                FollowVehicleBlockedAt=Now;
                FollowVehicleBlockedLocation=Here;
            }
            else if (Now-FollowVehicleBlockedAt>=GetTuning().BlockedRetrySeconds)
            {
                const auto Result=ContinueOnFootAtParking(Reason);
                if (Result.IsSet()) return Result.GetValue();
            }
        }
        else if (bKeepMountedFollow) FollowVehicleBlockedAt=-1.0;
        ReportVehiclePause(Reason);
        return false;
    };
    if (bVehicleHasDriveGoal && !ErrandVehicle->GetDriveFailureReason().IsNone())
    {
        const FName Failure=ErrandVehicle->GetDriveFailureReason();
        if (!bSeekingWalkParking) return PauseOrWalk(Failure,Failure==TEXT("VehicleBlocked") || Failure==TEXT("WalkOnlyDoorway"));
        bVehicleHasDriveGoal=false;
    }
    const float OwnerGoalShift=bKeepMountedFollow?FVector::Dist(Goal,VehicleLegGoal):FVector::Dist2D(Goal,VehicleLegGoal);
    if (bVehicleHasDriveGoal && OwnerGoalShift<GetTuning().RepathDistance &&
        ErrandVehicle->HasActiveDriveRoute((bSeekingWalkParking && !bFollowWalkAfterOwnerRide) || bRequiresWalkExit)) return false;
    // 未绑定实际下车位置的街外车段只负责接近，终点还要重新确认可停车和可步行
    if (bRequiresWalkExit && !bVehicleHasDriveGoal && ErrandVehicle->HasActiveDriveRoute()) return false;
    if (!bKeepMountedFollow)
    {
        if (Now<NextVehicleNavigationAt) return false;
        NextVehicleNavigationAt=Now+GetTuning().NavigationInterval;
    }
    if (bKeepMountedFollow)
    {
        const FVector Here=ErrandVehicle->GetActorLocation();
        FVector TowardBike=(Here-Goal).GetSafeNormal2D();
        if (TowardBike.IsNearlyZero()) TowardBike=-ErrandVehicle->GetActorForwardVector().GetSafeNormal2D();
        if (!bSeekingWalkParking)
        {
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
            const TWeakObjectPtr<ADeliveryCompanionMotorbike> RouteBike(ErrandVehicle.Get());
            const auto IsCurrentRouteBike=[&]()
            {
                return IsCurrent() && RouteBike.IsValid() && RouteBike.Get()==ErrandVehicle.Get() &&
                    !RouteBike->IsActorBeingDestroyed() && IsUsingErrandVehicle();
            };
            const auto TryFollowSegment=[&](const FVector& Destination,bool bUseNavigation=false,bool bRequireGoalHeight=true)
            {
                // 找路或切换车路期间若换了安排，结束这次旧调度
                if (!IsCurrentRouteBike()) return true;
                const bool bStarted=bUseNavigation?RouteBike->SetDriveDestination(Destination,true):
                    RouteBike->TrySetDirectFollowDestination(Destination,bRequireGoalHeight);
                if (!IsCurrentRouteBike()) return true;
                if (!bStarted) return false;
                VehicleLegGoal=Goal;
                // 中途目标到达时还要继续接近主人，仍以实际停车目标判断跟到
                VehicleParkingGoal=Behind;
                bVehicleHasDriveGoal=true;
                FollowVehicleRoutePause={};
                SetSuspension(NAME_None);
                return true;
            };
            const double Remaining=FVector::Dist2D(Here,Behind);
            const double MinimumStep=ErrandVehicle->GetParkingAcceptanceRadius()+60.;
            if (Remaining>MinimumStep && TryFollowSegment(Here+ToBehind*FMath::Min(Remaining,2200.),false,false)) return false;

            TArray<FVector,TInlineAllocator<6>> TraceGoals;
            if (IsOwnerDrivingBike(OwnerBike,OwnerPlayerState))
            {
                constexpr double TraceFloorTolerance=180.;
                double NearestDistanceSquared=FMath::Square(450.);
                int32 NearestSegment=INDEX_NONE;
                FVector Later=Goal;
                // 贴近车迹时只接入当前位置之后的路段，距离相同优先较新的路段
                for (int32 Index=FollowRoute.Num()-1;Index>=0;--Index)
                {
                    const FVector Point=FollowRoute[Index];
                    FVector Segment=Later-Point;
                    Segment.Z=0.;
                    const double LengthSquared=Segment.SizeSquared();
                    if (LengthSquared>UE_DOUBLE_SMALL_NUMBER)
                    {
                        const double Alpha=FMath::Clamp(FVector::DotProduct(Here-Point,Segment)/LengthSquared,0.,1.);
                        const FVector Closest=FMath::Lerp(Point,Later,Alpha);
                        const double DistanceSquared=FVector::DistSquared2D(Here,Closest);
                        if (FMath::Abs(Closest.Z-Here.Z)<=TraceFloorTolerance && DistanceSquared<NearestDistanceSquared)
                        {
                            NearestDistanceSquared=DistanceSquared;
                            NearestSegment=Index;
                        }
                    }
                    Later=Point;
                }

                double Traveled=0.;
                Later=Goal;
                for (int32 Index=FollowRoute.Num()-1;Index>NearestSegment && TraceGoals.Num()<6;--Index)
                {
                    const FVector Point=FollowRoute[Index];
                    Traveled+=FVector::Dist2D(Later,Point);
                    Later=Point;
                    if (Traveled>Distance+2200.) break;
                    const double Step=FVector::Dist2D(Here,Point);
                    if (Traveled<PreferredDistance || Step<=MinimumStep || Step>2200.) continue;
                    // 尚未贴近车迹时只接入同层且能缩短车距的点，已有连续车迹可沿坡继续
                    if (NearestSegment==INDEX_NONE && (FMath::Abs(Point.Z-Here.Z)>TraceFloorTolerance ||
                        FVector::Dist2D(Point,Goal)+MinimumStep>Distance)) continue;
                    TraceGoals.Add(Point);
                }
            }
            // 直追车尾受阻时先尝试直接接入主人已经骑过的位置
            for (const FVector& Point:TraceGoals)
                if (TryFollowSegment(Point)) return false;

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
                    if (TryFollowSegment(Nearby,false,false)) return false;
                }
            }

            // 街角遮住车尾时沿真实车迹寻路，靠近后的停车仍由原判断处理
            int32 PathAttempts=0;
            for (const FVector& Point:TraceGoals)
            {
                if (PathAttempts==3) break;
                FNavLocation RoadPoint;
                const bool bProjected=Nav->ProjectPointToNavigation(Point,RoadPoint,FVector(75,75,140),&Npc->GetNavAgentPropertiesRef());
                if (!IsCurrentRouteBike()) return false;
                if (!bProjected ||
                    FVector::DistSquared2D(Point,RoadPoint.Location)>FMath::Square(90.) ||
                    FMath::Abs(RoadPoint.Location.Z-Point.Z)>140. ||
                    FVector::Dist2D(Here,RoadPoint.Location)<=MinimumStep) continue;
                ++PathAttempts;
                if (TryFollowSegment(RoadPoint.Location,true)) return false;
            }
        }
        // 主人可能站在台阶或路缘，找他周围可安全停车的位置
        const FVector* WalkAfterParking=nullptr;
        if (bSeekingWalkParking)
        {
            const auto& WalkingGoal=GetOwnerWalkingGoal();
            if (!WalkingGoal.IsSet() && !bFollowWalkAfterOwnerRide)
            {
                if (!bVehicleHasDriveGoal || !ErrandVehicle->HasActiveDriveRoute()) ReportVehiclePause(TEXT("CompanionDismountBlocked"));
                return false;
            }
            const FVector BikeFeet=Here-FVector(0,0,ErrandVehicle->GetSimpleCollisionHalfHeight());
            if (WalkingGoal.IsSet() && (!bFollowWalkAfterOwnerRide || !IsAcrossClosedFollowDoorway(OwnerOnFoot,BikeFeet)))
                WalkAfterParking=&WalkingGoal.GetValue();
        }
        const float ParkingTolerance=ErrandVehicle->GetParkingAcceptanceRadius();
        const float FollowStopRadius=FMath::Max(ReachRadius+90.f,PreferredDistance-ParkingTolerance);
        FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionVehicleFollowParking),false,Npc);
        Query.AddIgnoredActor(ErrandVehicle);
        Query.AddIgnoredActor(FollowOwnerPawn);
        FCollisionObjectQueryParams Objects;
        for (const auto Channel:{ECC_WorldStatic,ECC_WorldDynamic,ECC_Pawn,ECC_Vehicle,ECC_PhysicsBody})
            Objects.AddObjectTypesToQuery(Channel);

        bool bFoundGround=false;
        bool bFoundVisibleCandidate=false;
        bool bFoundStaticObstruction=false;
        for (const float Angle:{0.f,45.f,-45.f,90.f,-90.f,135.f,-135.f,180.f})
        {
            const FVector Desired=Goal+TowardBike.RotateAngleAxis(Angle,FVector::UpVector)*FollowStopRadius;
            FNavLocation Parking;
            if (!Nav->ProjectPointToNavigation(Desired,Parking,FVector(100,100,180),&Npc->GetNavAgentPropertiesRef()) ||
                FVector::Dist2D(Parking.Location,Goal)>PreferredDistance ||
                FVector::Dist2D(Parking.Location,Goal)<ReachRadius+40.f ||
                FMath::Abs(Parking.Location.Z-Goal.Z)>180.f) continue;
            if (bSeekingWalkParking && RejectedFollowParking.ContainsByPredicate([&](const FVector& Rejected)
                { return IsSameParking(Rejected,Parking.Location); })) continue;
            bFoundGround=true;
            FHitResult Hit;
            // 准备步行时用完整步行路线确认能否绕墙到主人，继续骑车等候才需要直视
            if (!bSeekingWalkParking && GetWorld()->LineTraceSingleByObjectType(Hit,Parking.Location+FVector(0,0,100),Goal,Objects,Query))
            {
                if (const auto* Component=Hit.GetComponent())
                    bFoundStaticObstruction|=Component->GetCollisionObjectType()==ECC_WorldStatic ||
                        Component->Mobility==EComponentMobility::Static;
                continue;
            }
            bFoundVisibleCandidate=true;
            if (!ErrandVehicle->SetDriveDestination(Parking.Location,true,WalkAfterParking))
            {
                if (!IsCurrent() || !IsUsingErrandVehicle()) return false;
                continue;
            }
            if (!IsCurrent() || !IsUsingErrandVehicle()) return false;
            VehicleLegGoal=Goal;
            VehicleParkingGoal=Parking.Location;
            bVehicleHasDriveGoal=true;
            FollowVehicleRoutePause={};
            SetSuspension(bSeekingWalkParking?FName(TEXT("CompanionDismountBlocked")):NAME_None);
            if (bSeekingWalkParking)
                UE_LOG(LogDelivery,Log,TEXT("CompanionFollowExitRepark npc=%s revision=%d parking=%s"),
                    *GetNameSafe(Npc),Revision,*Parking.Location.ToCompactString());
            return false;
        }
        // 主人移动后的新目标暂时不可达时，先走完仍有效的当前路段
        if (ErrandVehicle->GetWorld()==GetWorld() && bVehicleHasDriveGoal && ErrandVehicle->HasActiveDriveRoute())
            return false;
        if (bSeekingWalkParking)
        {
            ReportVehiclePause(TEXT("CompanionDismountBlocked"));
            return false;
        }
        const FName Failure=!bFoundGround?FName(TEXT("CompanionVehicleFollowGroundMissing")):
            !bFoundVisibleCandidate?FName(TEXT("CompanionVehicleFollowParkingBlocked")):
            FName(TEXT("CompanionVehicleNoRoute"));
        UE_LOG(LogDelivery,Log,TEXT("CompanionFollowParkingNoRoute npc=%s revision=%d reason=%s bike=%s owner=%s"),
            *GetNameSafe(Npc),Revision,*Failure.ToString(),*Here.ToCompactString(),*Goal.ToCompactString());
        // 仅车辆或行人挡住停车视线时照常重试，留给动态障碍离开的时间
        const bool bOnlyDynamicObstruction=bFoundGround && !bFoundVisibleCandidate && !bFoundStaticObstruction;
        return PauseOrWalk(Failure,!bOnlyDynamicObstruction);
    }
    // 取件点锚点可以在建筑内，终点先落在实际交互半径内的可走地面
    const float ProjectionRadius=FMath::Clamp(ReachRadius,30.f,1200.f);
    FNavLocation Approach;
    const FVector* WalkAfterParking=nullptr;
    if (bRequiresWalkExit)
    {
        const auto& WalkingGoal=GetErrandWalkingGoal();
        if (!WalkingGoal.IsSet())
        {
            if (bVehicleHasDriveGoal && ErrandVehicle->HasActiveDriveRoute(true)) return false;
            if (LastErrandPause!=TEXT("CompanionErrandWalkGoalUnavailable"))
                UE_LOG(LogDelivery,Log,TEXT("CompanionErrandWalkGoalUnavailable npc=%s revision=%d bike=%s requested=%s"),
                    *GetNameSafe(Npc),Revision,*ErrandVehicle->GetActorLocation().ToCompactString(),*RequiredWalkGoal->ToCompactString());
            ReportVehiclePause(TEXT("CompanionErrandWalkGoalUnavailable"));
            return false;
        }
        WalkAfterParking=&WalkingGoal.GetValue();
        Approach.Location=*WalkAfterParking;
    }
    else if (!Nav->ProjectPointToNavigation(Goal,Approach,FVector(ProjectionRadius,ProjectionRadius,250),&Npc->GetNavAgentPropertiesRef()) ||
        FVector::Dist2D(Approach.Location,Goal)>ProjectionRadius)
    {
        if (!bVehicleHasDriveGoal || !ErrandVehicle->HasActiveDriveRoute()) ReportVehiclePause(TEXT("CompanionVehicleNoRoute"));
        return false;
    }
    const auto* Path=Nav->FindPathToLocationSynchronously(GetWorld(),ErrandVehicle->GetActorLocation(),Approach.Location,Npc);
    const bool bHasParkingGuide=Path && Path->IsValid() && !Path->IsPartial() && Path->PathPoints.Num()>=2;
    if (!bHasParkingGuide && !bRequiresWalkExit)
    {
        if (!bVehicleHasDriveGoal || !ErrandVehicle->HasActiveDriveRoute()) ReportVehiclePause(TEXT("CompanionVehicleNoRoute"));
        return false;
    }
    // 从路线末端向外找停车点，最近一个挡住时尝试同一路线的上游位置
    const double MinimumSetback=FMath::Max(450.f,ReachRadius+180.f);
    const double ParkingInterval=200.;
    const double MaxSetback=MinimumSetback+3.*ParkingInterval;
    const FVector Start=ErrandVehicle->GetActorLocation();
    const auto AddErrandTravelBudget=[&](double Length)
    {
        if (!bForErrand || bErrandTravelBudgetAdded) return;
        const float WalkingSpeed=Npc->GetCharacterMovement()->MaxWalkSpeed;
        if (!FMath::IsFinite(WalkingSpeed) || WalkingSpeed<=0 || !FMath::IsFinite(Length) || Length<0) return;
        ErrandTimeLimitSeconds+=Length/WalkingSpeed;
        bErrandTravelBudgetAdded=true;
    };
    double Covered=0., NextSetback=MinimumSetback;
    FVector Parking=FVector::ZeroVector;
    FName LastParkingRejection;
    int32 ParkingAttempts=0;
    bool bHasParking=false;
    const auto TryParkingSpot=[&](const FVector& Candidate)
    {
        if (FMath::Abs(Candidate.Z-Start.Z)<=150. && FVector::Dist2D(Candidate,Start)<=ErrandVehicle->GetParkingAcceptanceRadius()) return false;
        if (bRequiresWalkExit && RejectedErrandParking.ContainsByPredicate([&](const FVector& Rejected){ return IsSameParking(Rejected,Candidate); }))
            return false;
        ++ParkingAttempts;
        if (!ErrandVehicle->SetDriveDestination(Candidate,true,WalkAfterParking,&LastParkingRejection)) return false;
        Parking=Candidate;
        return true;
    };
    for (int32 Index=bHasParkingGuide?Path->PathPoints.Num()-1:0;Index>0;--Index)
    {
        const FVector End=Path->PathPoints[Index];
        const FVector Segment=Path->PathPoints[Index-1]-End;
        const double Length=Segment.Size2D();
        if (Length<=UE_KINDA_SMALL_NUMBER) continue;
        for (;NextSetback<=MaxSetback && NextSetback<=Covered+Length;NextSetback+=ParkingInterval)
        {
            const FVector Candidate=End+Segment*((NextSetback-Covered)/Length);
            bHasParking=TryParkingSpot(Candidate);
            if (bHasParking) break;
        }
        if (bHasParking || NextSetback>MaxSetback) break;
        Covered+=Length;
    }
    if (!IsCurrent() || !IsUsingErrandVehicle()) return false;
    // 车已在街边时确认当前实车姿态的出口，失败后仍可换另一处停车
    const bool bAtShortLeg=(bHasParkingGuide && Path->GetPathLength()<=MinimumSetback) ||
        (bRequiresWalkExit && FVector::Dist2D(Start,Approach.Location)<=MinimumSetback && FMath::Abs(Start.Z-Approach.Location.Z)<=150.);
    if (!bHasParking && bAtShortLeg && ErrandVehicle->CanParkAt(Start))
    {
        bVehicleLegComplete=true;
        VehicleLegGoal=Goal;
        const bool bOnFoot=TryFinishVehicleLegOnFoot();
        if (!IsCurrent()) return false;
        if (bOnFoot || !bRequiresWalkExit)
        {
            SetSuspension(bOnFoot?NAME_None:FName(TEXT("CompanionDismountBlocked")));
            return bOnFoot;
        }
        RejectErrandWalkParking();
        if (!IsCurrent() || !IsUsingErrandVehicle()) return false;
    }
    if (!bHasParking && bRequiresWalkExit)
    {
        FVector TowardBike=(Start-Approach.Location).GetSafeNormal2D();
        if (TowardBike.IsNearlyZero()) TowardBike=-ErrandVehicle->GetActorForwardVector().GetSafeNormal2D();
        // 原车路的上游没有可下车位置时，在步行终点附近寻找两侧车位
        for (const float Angle:{0.f,45.f,-45.f,90.f,-90.f,135.f,-135.f,180.f})
        {
            const FVector Desired=Approach.Location+TowardBike.RotateAngleAxis(Angle,FVector::UpVector)*MinimumSetback;
            FNavLocation Candidate;
            if (!Nav->ProjectPointToNavigation(Desired,Candidate,FVector(90,90,100),&Npc->GetNavAgentPropertiesRef()) ||
                FMath::Abs(Candidate.Location.Z-Approach.Location.Z)>100.f) continue;
            bHasParking=TryParkingSpot(Candidate.Location);
            if (bHasParking) break;
        }
    }
    if (!IsCurrent() || !IsUsingErrandVehicle()) return false;
    if (!bHasParking)
    {
        if (bVehicleHasDriveGoal && ErrandVehicle->HasActiveDriveRoute(bRequiresWalkExit)) return false;
        FName RoadRejection;
        if (bRequiresWalkExit && bHasParkingGuide && !bAtShortLeg)
        {
            const double AdvanceLimit=Path->GetPathLength()-MinimumSetback;
            double PreviousAdvance=0.;
            for (const double DistanceLimit:{2200.,1100.,550.})
            {
                double Advance=FMath::Min(AdvanceLimit,DistanceLimit);
                if (Advance<450.) break;
                if (Advance==PreviousAdvance) continue;
                PreviousAdvance=Advance;
                for (int32 Index=1;Index<Path->PathPoints.Num();++Index)
                {
                    const FVector From=Path->PathPoints[Index-1];
                    const FVector Segment=Path->PathPoints[Index]-From;
                    const double Length=Segment.Size();
                    if (Length<=UE_KINDA_SMALL_NUMBER) continue;
                    if (Advance>Length) { Advance-=Length; continue; }
                    const FVector RoadPoint=From+Segment*(Advance/Length);
                    if (FVector::Dist2D(RoadPoint,Start)<=ErrandVehicle->GetParkingAcceptanceRadius()+100.f ||
                        !ErrandVehicle->SetDriveDestination(RoadPoint,true,nullptr,&RoadRejection)) break;
                    if (!IsCurrent() || !IsUsingErrandVehicle()) return false;
                    // 街外短程不记已到停车点，停稳后重新核对完整步行出口
                    bVehicleHasDriveGoal=false;
                    SetSuspension(NAME_None);
                    LastErrandPause=NAME_None;
                    AddErrandTravelBudget(Path->GetPathLength());
                    UE_LOG(LogDelivery,Log,TEXT("CompanionErrandRoadLeg npc=%s revision=%d bike=%s segment=%s walk=%s parking_rejection=%s"),
                        *GetNameSafe(Npc),Revision,*Start.ToCompactString(),*RoadPoint.ToCompactString(),
                        *Approach.Location.ToCompactString(),*LastParkingRejection.ToString());
                    return false;
                }
            }
        }
        const FName Failure=bRequiresWalkExit?FName(TEXT("CompanionErrandParkingUnavailable")):
            FName(TEXT("CompanionVehicleParkingBlocked"));
        if (bRequiresWalkExit && LastErrandPause!=Failure)
            UE_LOG(LogDelivery,Log,TEXT("CompanionErrandParkingUnavailable npc=%s revision=%d bike=%s walk=%s guide=%d attempts=%d parking_rejection=%s road_rejection=%s"),
                *GetNameSafe(Npc),Revision,*Start.ToCompactString(),*Approach.Location.ToCompactString(),
                bHasParkingGuide,ParkingAttempts,*LastParkingRejection.ToString(),*RoadRejection.ToString());
        ReportVehiclePause(Failure);
        return false;
    }
    VehicleLegGoal=Goal;
    VehicleParkingGoal=Parking;
    bVehicleHasDriveGoal=true;
    SetSuspension(NAME_None);
    if (bForErrand) LastErrandPause=NAME_None;
    AddErrandTravelBudget(bHasParkingGuide?Path->GetPathLength():FVector::Dist(Start,Parking)+FVector::Dist(Parking,Approach.Location));
    return false;
}
