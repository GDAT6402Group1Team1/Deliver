#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "Vehicle/DeliveryCompanionMotorbike.h"
#include "AIController.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "Navigation/PathFollowingComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"

bool UDeliveryCompanionComponent::IsUsingErrandVehicle() const
{
    return IsValid(ErrandVehicle) && ErrandVehicle->GetReservedCompanion()==GetOwner() && ErrandVehicle->IsMounted();
}

bool UDeliveryCompanionComponent::EnsureOnFoot()
{
    if (!IsUsingErrandVehicle()) { bVehicleNeedsDismount=false; return true; }
    if (!GetOwner() || !GetOwner()->HasAuthority()) return false;
    ErrandVehicle->StopDriving();
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
    if (IsValid(ErrandVehicle) && ErrandVehicle->GetReservedCompanion()==GetOwner())
    {
        ErrandVehicle->StopDriving();
        EnsureOnFoot();
    }
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
    NextVehicleNavigationAt=0;
    StopVehicleApproach();
    if (!WeakThis.IsValid() || CommandRevision!=Revision || VehicleTravelPurpose!=EDeliveryCompanionTravelPurpose::None) return;
    EnsureOnFoot();
}

void UDeliveryCompanionComponent::ReportVehiclePause(FName Reason)
{
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const int32 Revision=CommandRevision;
    const auto Purpose=VehicleTravelPurpose;
    if (VehicleTravelPurpose==EDeliveryCompanionTravelPurpose::Errand) ReportErrandPause(Reason);
    else
    {
        StopVehicleTravel(VehicleTravelPurpose);
        if (!WeakThis.IsValid() || CommandRevision!=Revision || VehicleTravelPurpose!=Purpose) return;
        SetSuspension(Reason);
    }
    if (!WeakThis.IsValid() || CommandRevision!=Revision || VehicleTravelPurpose!=Purpose) return;
    NextVehicleNavigationAt=GetWorld()->GetTimeSeconds()+GetTuning().BlockedRetrySeconds;
}

bool UDeliveryCompanionComponent::UpdateVehicleTravel(const FVector& Goal,float ReachRadius,EDeliveryCompanionTravelPurpose Purpose)
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority() || !CanCommand(OwnerPlayerState) || Purpose==EDeliveryCompanionTravelPurpose::None ||
        Goal.ContainsNaN() || !FMath::IsFinite(ReachRadius)) return false;
    const bool bForErrand=Purpose==EDeliveryCompanionTravelPurpose::Errand;
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
    const float Distance=FVector::Dist2D(Npc->GetActorLocation(),Goal);
    const float PreferredDistance=IsValid(ErrandVehicle)?ErrandVehicle->GetPreferredTravelDistance():1200.f;
    if (bVehicleLegComplete)
    {
        const bool bOnFoot=EnsureOnFoot();
        if (!IsCurrent()) return false;
        if (!bOnFoot) { SetSuspension(TEXT("CompanionDismountBlocked")); return false; }
        // 主人完成这段行程后又走远，可以再次去真实停车处取车
        if (Distance>PreferredDistance && FVector::Dist2D(Goal,VehicleLegGoal)>GetTuning().RepathDistance)
            bVehicleLegComplete=false;
        else { SetSuspension(NAME_None); return true; }
    }
    // 短距离只走最后一段，长距离必须使用地图指定的专属车，不搜索或征用玩家车辆
    if (!IsUsingErrandVehicle() && Distance<=PreferredDistance)
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
    const double Now=GetWorld()->GetTimeSeconds();

    if (!IsUsingErrandVehicle())
    {
        if (Now<NextVehicleNavigationAt && !bVehicleOwnsApproach) return false;
        const FVector Boarding=ErrandVehicle->GetBoardingLocation();
        if (FVector::Dist2D(Npc->GetActorLocation(),Boarding)<=ErrandVehicle->GetBoardingRadius() &&
            FMath::Abs(Npc->GetActorLocation().Z-Boarding.Z)<=150.f)
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
            FVector Toward=(Npc->GetActorLocation()-Boarding).GetSafeNormal2D();
            if (Toward.IsNearlyZero()) Toward=ErrandVehicle->GetActorRightVector();
            for (const float Angle:{0.f,45.f,-45.f,90.f,-90.f,180.f})
            {
                FNavLocation Board;
                const FVector Proposed=Boarding+Toward.RotateAngleAxis(Angle,FVector::UpVector)*30.f;
                if (!Nav->ProjectPointToNavigation(Proposed,Board,FVector(90,90,250),&Npc->GetNavAgentPropertiesRef()) ||
                    FVector::Dist2D(Board.Location,Boarding)>ErrandVehicle->GetBoardingRadius()-15.f || !IsDestinationClear(Board.Location)) continue;
                const auto* WalkPath=Nav->FindPathToLocationSynchronously(GetWorld(),Npc->GetNavAgentLocation(),Board.Location,Npc);
                if (!WalkPath || !WalkPath->IsValid() || WalkPath->IsPartial()) continue;
                FAIMoveRequest Request(Board.Location);
                Request.SetAcceptanceRadius(15); Request.SetAllowPartialPath(false);
                Request.SetReachTestIncludesAgentRadius(false); Request.SetReachTestIncludesGoalRadius(false);
                const auto Result=AI->MoveTo(Request);
                if (!IsCurrent()) return false;
                if (Result.Code==EPathFollowingRequestResult::Failed) continue;
                VehicleApproachMoveId=Result.MoveId;
                bVehicleOwnsApproach=Result.Code==EPathFollowingRequestResult::RequestSuccessful;
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
            ReportVehiclePause(TEXT("CompanionVehicleApproachBlocked"));
            return false;
        }
    }

    if (bVehicleHasDriveGoal && ErrandVehicle->HasReachedDestination())
    {
        // 到站事实先落到本阶段，EnsureOnFoot 会清驾驶目标；堵住出口只重试下车
        bVehicleLegComplete=true;
        bHasErrandDestination=false;
        bHasFollowDestination=false;
        const bool bOnFoot=EnsureOnFoot();
        if (!IsCurrent()) return false;
        if (!bOnFoot) { SetSuspension(TEXT("CompanionDismountBlocked")); return false; }
        SetSuspension(NAME_None);
        return true;
    }
    if (bVehicleHasDriveGoal && !ErrandVehicle->GetDriveFailureReason().IsNone())
    {
        ReportVehiclePause(ErrandVehicle->GetDriveFailureReason());
        return false;
    }
    if (bVehicleHasDriveGoal && FVector::Dist2D(Goal,VehicleLegGoal)<GetTuning().RepathDistance) return false;
    if (Now<NextVehicleNavigationAt) return false;
    NextVehicleNavigationAt=Now+GetTuning().NavigationInterval;
    // 取件点锚点可以在建筑内，终点先落在实际交互半径内的可走地面
    const float ProjectionRadius=FMath::Clamp(ReachRadius,30.f,1200.f);
    FNavLocation Approach;
    if (!Nav->ProjectPointToNavigation(Goal,Approach,FVector(ProjectionRadius,ProjectionRadius,250),&Npc->GetNavAgentPropertiesRef()) ||
        FVector::Dist2D(Approach.Location,Goal)>ProjectionRadius)
    { ReportVehiclePause(TEXT("CompanionVehicleNoRoute")); return false; }
    const auto* Path=Nav->FindPathToLocationSynchronously(GetWorld(),ErrandVehicle->GetActorLocation(),Approach.Location,Npc);
    if (!Path || !Path->IsValid() || Path->IsPartial() || Path->PathPoints.Num()<2)
    { ReportVehiclePause(TEXT("CompanionVehicleNoRoute")); return false; }
    // 从路线末端退回停车，取件、破门后的进屋和交接仍由二狗步行执行
    float Remaining=FMath::Max(450.f,ReachRadius+180.f);
    FVector Parking=Path->PathPoints[0];
    for (int32 Index=Path->PathPoints.Num()-1;Index>0;--Index)
    {
        const FVector Segment=Path->PathPoints[Index-1]-Path->PathPoints[Index];
        const float Length=Segment.Size();
        if (Length>=Remaining) { Parking=Path->PathPoints[Index]+Segment.GetSafeNormal()*Remaining; break; }
        Remaining-=Length;
    }
    if (!ErrandVehicle->SetDriveDestination(Parking))
    {
        const FName Failure=ErrandVehicle->GetDriveFailureReason();
        ReportVehiclePause(Failure.IsNone()?FName(TEXT("CompanionVehicleRouteRejected")):Failure);
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
