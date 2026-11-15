#include "Vehicle/DeliveryCompanionMotorbike.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Combat/DeliveryCollisionChannels.h"
#include "AIController.h"
#include "Carry/DeliveryPackageCarrierInterface.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "NavLinkCustomComponent.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "Net/UnrealNetwork.h"
#include "World/DeliveryHome.h"
#include "World/DeliveryNavigationAreas.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeliveryCompanionBike, Log, All);

ADeliveryCompanionMotorbike::ADeliveryCompanionMotorbike()
{
    AutoPossessAI=EAutoPossessAI::Disabled;
    AutoPossessPlayer=EAutoReceiveInput::Disabled;
    CompanionSeat=CreateDefaultSubobject<USceneComponent>(TEXT("CompanionSeat"));
    // 人物挂点使用厘米空间，不继承车体模型的缩放
    CompanionSeat->SetupAttachment(MeshRoot);
    CompanionSeat->SetRelativeLocation(FVector(-15,0,65));
    CompanionCargoAnchor=CreateDefaultSubobject<USceneComponent>(TEXT("CompanionCargoAnchor"));
    CompanionCargoAnchor->SetupAttachment(MeshRoot);
    CompanionCargoAnchor->SetAbsolute(false,false,true);
    CompanionCargoAnchor->SetRelativeLocation(FVector(-90,0,50));
    CompanionRiderMesh=CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("CompanionRiderMesh"));
    CompanionRiderMesh->SetupAttachment(CompanionSeat);
    CompanionRiderMesh->SetAbsolute(false,false,true);
    CompanionRiderMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    CompanionRiderMesh->SetGenerateOverlapEvents(false);
    CompanionRiderMesh->SetVisibility(false);
    CompanionRiderMesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
}

void ADeliveryCompanionMotorbike::BeginPlay()
{
    Super::BeginPlay();
    if (Interactable) Interactable->bInteractEnabled=false;
    if (TrunkInteractable) TrunkInteractable->bInteractEnabled=false;
    if (PassengerInteractable) PassengerInteractable->bInteractEnabled=false;
    if (RiderMesh) RiderMesh->SetVisibility(false);
    ApplyCompanionPresentation();
}

ADeliveryNpcCharacter* ADeliveryCompanionMotorbike::GetReservedCompanion() const
{
    return IsValid(ReservedCompanion) && !ReservedCompanion->IsActorBeingDestroyed()?ReservedCompanion.Get():nullptr;
}

bool ADeliveryCompanionMotorbike::IsMounted() const { return bMounted && GetReservedCompanion()!=nullptr; }
bool ADeliveryCompanionMotorbike::HasAutonomousDriver() const { return IsMounted(); }

bool ADeliveryCompanionMotorbike::IsNetRelevantFor(const AActor* RealViewer,const AActor* ViewTarget,const FVector& SrcLocation) const
{
    const auto* Player=Cast<APlayerController>(RealViewer);
    const auto* Npc=bMounted?GetReservedCompanion():nullptr;
    const auto* Companion=Npc?Npc->FindComponentByClass<UDeliveryCompanionComponent>():nullptr;
    // 骑乘时二狗的世界位置取决于座椅附着，主人在远处也需要收到车的位置
    if (IsValid(Player) && Player->GetWorld()==GetWorld() && Companion && Companion->CanCommand(Player->PlayerState))
        return true;
    return Super::IsNetRelevantFor(RealViewer,ViewTarget,SrcLocation);
}

bool ADeliveryCompanionMotorbike::InitializeForCompanion(ADeliveryNpcCharacter* Companion)
{
    if (!HasAuthority() || !IsValid(Companion) || Companion->GetWorld()!=GetWorld() || GetDriver() || GetController()) return false;
    if (bReservationInitialized) return ReservedCompanion==Companion;
    // 同一世界内一名同伴只绑定一辆预放车，不维护跨世界静态登记
    for (TActorIterator<ADeliveryCompanionMotorbike> It(GetWorld());It;++It)
        if (*It!=this && It->GetReservedCompanion()==Companion) return false;
    ReservedCompanion=Companion;
    bReservationInitialized=true;
    ForceNetUpdate();
    return true;
}

bool ADeliveryCompanionMotorbike::HasRidePose() const
{
    const auto* Npc=GetReservedCompanion();
    const auto* Mesh=Npc?Npc->GetMesh():nullptr;
    return Mesh && Mesh->GetSkeletalMeshAsset() && RideAnimation && RideAnimation->GetSkeleton() &&
        RideAnimation->GetSkeleton()->IsCompatibleMesh(Mesh->GetSkeletalMeshAsset());
}

bool ADeliveryCompanionMotorbike::CanCompanionRide() const
{
    const auto* Npc=GetReservedCompanion();
    return Npc && !Npc->IsActorBeingDestroyed() && Npc->GetWorld()==GetWorld() &&
        Cast<AAIController>(Npc->GetController()) && Npc->Combat && !Npc->IsKnockedDown() &&
        !Npc->Combat->IsReactingToHit() && Npc->Agent && !Npc->Agent->ShouldPauseForConversation() &&
        (!Npc->Agent->HasActiveAction() || Npc->Agent->CanContinueCompanionMovement());
}

void ADeliveryCompanionMotorbike::ReceiveHorn(float Multiplier)
{
    ReceiveHornFrom(Multiplier,nullptr,nullptr);
}

void ADeliveryCompanionMotorbike::ReceiveHornFrom(float Multiplier,AActor* Vehicle,AActor* SourceDriver)
{
    if (HasAuthority() && IsMounted() && Vehicle!=this)
        if (auto* Npc=GetReservedCompanion()) Npc->ReceiveHornFrom(Multiplier,Vehicle,SourceDriver);
}

AActor* ADeliveryCompanionMotorbike::GetAutonomousHornDriver() const
{
    return IsMounted() && FollowHornEndsAt>GetWorld()->GetTimeSeconds()?GetReservedCompanion():nullptr;
}

void ADeliveryCompanionMotorbike::ReportFollowBlocked()
{
    if (!HasAuthority() || !IsMounted()) return;
    auto* Npc=GetReservedCompanion();
    if (!IsValid(Npc)) return;
    const double Now=GetWorld()->GetTimeSeconds();
    LastFollowBlock=Now;
    if (Now<NextBlockObservation || Npc->Agent->IsDecisionPending()) return;
    NextBlockObservation=Now+10.;
    Npc->ObserveFact(TEXT("CompanionFollowBlocked"),nullptr,TEXT("跟随路段暂时过不去，正在找能绕开的路线"),EDeliveryNpcEvidenceSource::ActionResult);
}

bool ADeliveryCompanionMotorbike::CanSoundFollowHorn() const
{
    if (!HasAuthority() || !IsMounted() || !CollisionBox || bDismountPending || !CanCompanionRide()) return false;
    const double Now=GetWorld()->GetTimeSeconds();
    if (Now-LastFollowBlock>3. || Now<NextFollowHorn) return false;
    const FVector Here=GetActorLocation();
    FHitResult Hit;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionHornBlocker),false,this);
    Query.AddIgnoredActor(ReservedCompanion);
    const FCollisionShape Shape=FCollisionShape::MakeSphere(38.f);
    if (!GetWorld()->SweepSingleByChannel(Hit,Here,Here+GetActorForwardVector()*350.,FQuat::Identity,
        CollisionBox->GetCollisionObjectType(),Shape,Query,
        FCollisionResponseParams(CollisionBox->GetCollisionResponseToChannels()))) return false;
    const auto* Component=Hit.GetComponent();
    return IsValid(Component) && (Component->GetCollisionObjectType()==DeliveryCollision::Pedestrian ||
        Component->GetCollisionObjectType()==ECC_Vehicle);
}

bool ADeliveryCompanionMotorbike::TrySoundFollowHorn()
{
    if (!CanSoundFollowHorn()) return false;
    const double Now=GetWorld()->GetTimeSeconds();
    NextFollowHorn=Now+8.;
    FollowHornEndsAt=Now+.3;
    SetHornActive(true);
    return true;
}

FVector ADeliveryCompanionMotorbike::GetBoardingLocation() const
{
    const auto* Npc=GetReservedCompanion();
    const float Radius=Npc?Npc->GetSimpleCollisionRadius():30;
    const float Side=CollisionBox?CollisionBox->GetScaledBoxExtent().Y:50;
    return GetActorLocation()+FRotationMatrix(FRotator(0,GetActorRotation().Yaw,0)).GetUnitAxis(EAxis::Y)*(Side+Radius+25);
}

FVector ADeliveryCompanionMotorbike::GetOppositeBoardingLocation() const
{
    return GetActorLocation()*2.-GetBoardingLocation();
}

bool ADeliveryCompanionMotorbike::CanBoardFromLocation(const FVector& PawnLocation) const
{
    const auto* Npc=GetReservedCompanion();
    if (!Npc || PawnLocation.ContainsNaN() || FMath::Abs(PawnLocation.Z-GetActorLocation().Z)>150.) return false;
    const float RadiusSquared=FMath::Square(FMath::Max(30.f,BoardingRadius));
    if (FVector::DistSquared2D(PawnLocation,GetBoardingLocation())>RadiusSquared &&
        FVector::DistSquared2D(PawnLocation,GetOppositeBoardingLocation())>RadiusSquared) return false;
    const FVector EyeOffset=Npc->GetPawnViewLocation()-Npc->GetActorLocation();
    FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionBoarding),false,this);
    Params.AddIgnoredActor(Npc);
    FHitResult Hit;
    return !GetWorld()->LineTraceSingleByChannel(Hit,PawnLocation+EyeOffset,CompanionSeat->GetComponentLocation(),ECC_Visibility,Params);
}

bool ADeliveryCompanionMotorbike::TryMount(ADeliveryNpcCharacter* Companion)
{
    if (!HasAuthority() || !IsValid(Companion) || Companion!=GetReservedCompanion()) return false;
    if (bMounted) return true;
    const auto CanStartMount=[&]()
    {
        return CanCompanionRide() && !Companion->IsHidden() && Companion->GetActorEnableCollision() &&
            !Companion->GetAttachParentActor() && Companion->GetCharacterMovement()->IsMovingOnGround();
    };
    if (!CanStartMount()) return false;
    if (!HasRidePose()) { DriveFailure=TEXT("RidePoseMissingOrIncompatible"); return false; }
    if (!CanBoardFromLocation(Companion->GetActorLocation())) return false;
    const TWeakObjectPtr<ADeliveryCompanionMotorbike> WeakThis(this);
    const TWeakObjectPtr<ADeliveryNpcCharacter> WeakNpc(Companion);
    const TWeakObjectPtr<UDeliveryCompanionComponent> Arrangement(Companion->FindComponentByClass<UDeliveryCompanionComponent>());
    const int32 Revision=Arrangement.IsValid()?Arrangement->GetCommandRevision():INDEX_NONE;
    auto* AI=CastChecked<AAIController>(Companion->GetController());
    AI->StopMovement();
    // 停止步行会同步通知导航监听者，旧安排不能在主人改令之后继续上车
    if (!WeakThis.IsValid() || !WeakNpc.IsValid() || IsActorBeingDestroyed() || bMounted ||
        GetReservedCompanion()!=Companion || Companion->GetController()!=AI ||
        (Revision!=INDEX_NONE && (!Arrangement.IsValid() || Arrangement->GetCommandRevision()!=Revision)) || !CanStartMount() ||
        !CanBoardFromLocation(Companion->GetActorLocation())) return false;
    if (!Companion->AttachToComponent(CompanionSeat,FAttachmentTransformRules::SnapToTargetNotIncludingScale)) return false;
    StopDriving();
    DriveFailure=NAME_None; bArrived=false; bDismountPending=false;
    Companion->GetCharacterMovement()->StopMovementImmediately();
    Companion->GetCharacterMovement()->DisableMovement();
    // 控制器保持在原人物，只有附着和表现改变
    Companion->SetActorEnableCollision(false);
    bMounted=true;
    ApplyCompanionPresentation();
    if (!WeakThis.IsValid() || !WeakNpc.IsValid() || IsActorBeingDestroyed() || Companion->IsActorBeingDestroyed() ||
        !bMounted || ReservedCompanion!=Companion) return false;
    Companion->ForceNetUpdate(); ForceNetUpdate();
    UE_LOG(LogDeliveryCompanionBike,Log,TEXT("CompanionMounted npc=%s bike=%s revision=%d"),
        *GetNameSafe(Companion),*GetName(),Revision);
    return true;
}

bool ADeliveryCompanionMotorbike::SetDriveDestination(const FVector& Goal,bool bKeepDrivingWhileReplanning,
    const FVector* WalkAfterParking,FName* OutRejection)
{
    if (OutRejection) *OutRejection=NAME_None;
    if (!HasAuthority())
    {
        if (OutRejection) *OutRejection=TEXT("CompanionUnavailable");
        return false;
    }
    const auto Reject=[&](FName Reason)
    {
        // 保留车路时仍返回本次拒绝原因，规划失败不用覆盖车辆当前公开状态
        if (OutRejection) *OutRejection=Reason;
        if (!bKeepDrivingWhileReplanning) FailDrive(Reason);
        return false;
    };
    if (!IsMounted() || bDismountPending || !CanCompanionRide()) return Reject(TEXT("CompanionUnavailable"));
    if (Goal.ContainsNaN()) return Reject(TEXT("ParkingBlocked"));

    // 路线确认后再替换当前路线，移动主人重新定点时保留现有车速和卡住计时
    auto* NewPath=UNavigationSystemV1::FindPathToLocationSynchronously(GetWorld(),GetActorLocation(),Goal,ReservedCompanion,
        UDeliveryCompanionVehicleNavFilter::StaticClass());
    if (!NewPath || !NewPath->IsValid() || NewPath->IsPartial() || NewPath->PathPoints.Num()<2)
        return Reject(TEXT("PathUnavailable"));
    const auto& Points=NewPath->PathPoints;
    auto* Home=ADeliveryHome::FindSharedHome(this);
    if (IsValid(Home) && IsValid(Home->DoorwayLink))
    {
        // 旧地图的导航块也不得给专属车分配这一条门洞连接
        const FNavLinkId DoorwayId=Home->DoorwayLink->GetId();
        if (DoorwayId.IsValid() && NewPath->GetPath()->ContainsCustomLink(DoorwayId)) return Reject(TEXT("WalkOnlyDoorway"));
        for (int32 Index=1;Index<Points.Num();++Index)
            if (Home->CrossesPedestrianDoorway(Points[Index-1],Points[Index])) return Reject(TEXT("WalkOnlyDoorway"));
    }
    const FVector ArrivalDirection=(Points.Last()-Points[Points.Num()-2]).GetSafeNormal2D();
    const FQuat ArrivalHeading=ArrivalDirection.IsNearlyZero()?
        FRotator(0,GetActorRotation().Yaw,0).Quaternion():ArrivalDirection.Rotation().Quaternion();
    // 停车位的长宽按最后一段到站朝向检查，最终停车仍使用实时车身朝向
    FVector ParkingCenter;
    if (!IsParkSpaceClear(Goal,ArrivalHeading,&ParkingCenter)) return Reject(TEXT("ParkingBlocked"));
    if (WalkAfterParking)
    {
        if (!IsValid(CompanionSeat) || !IsValid(MeshRoot)) return Reject(TEXT("WalkExitUnreachable"));
        // 停车时车身侧倾归零，按模型上的座位和预计车身还原位置，到站后再按实际姿态复查
        const FVector SeatOnMesh=MeshRoot->GetComponentTransform().InverseTransformPosition(CompanionSeat->GetComponentLocation());
        const FTransform MeshAtRest(FQuat::Identity,MeshRoot->GetRelativeLocation(),MeshRoot->GetRelativeScale3D());
        const FTransform ArrivalPose(ArrivalHeading,ParkingCenter,GetActorScale3D());
        FVector Exit;
        const FVector ArrivalSeat=ArrivalPose.TransformPosition(MeshAtRest.TransformPosition(SeatOnMesh));
        if (!FindDismountLocationAtPose(ParkingCenter,ArrivalHeading,ArrivalSeat,Exit,WalkAfterParking))
            return Reject(TEXT("WalkExitUnreachable"));
    }
    // 带步行出口的车路失效后重新检查末段朝向和出口，避免 UE 单独改掉到站姿态
    NewPath->EnableRecalculationOnInvalidation(WalkAfterParking?ENavigationOptionFlag::Disable:ENavigationOptionFlag::Enable);
    if (!bKeepDrivingWhileReplanning || !bDriving)
    {
        // 跟随调用者已经设置当前主人车辆，接受新绕线路段时继续用它的车速
        if (!bKeepDrivingWhileReplanning) StopDriving();
        ProgressLocation=GetActorLocation();
        LastProgressTime=GetWorld()->GetTimeSeconds();
    }
    DrivePath=NewPath;
    DrivePedestrianHome=Home;
    bDrivingDirect=false;
    bDrivingToWalkExit=WalkAfterParking!=nullptr;
    PathPointIndex=1;
    DriveGoal=Goal;
    bDriving=true;
    bArrived=false;
    DriveFailure=NAME_None;
    return true;
}

bool ADeliveryCompanionMotorbike::TrySetDirectFollowDestination(const FVector& Goal,bool bRequireGoalHeight)
{
    if (!HasAuthority() || !IsMounted() || bDismountPending || !CanCompanionRide() || Goal.ContainsNaN()) return false;
    const FVector Here=GetActorLocation();
    const double Distance=FVector::Dist2D(Here,Goal);
    if (Distance<20 || Distance>2400) return false;
    // 行驶段采用车辆自身的爬坡限制，保留每段净空和跨台阶高度检查
    const int32 Samples=FMath::CeilToInt(Distance/90.);
    FVector Previous=Here;
    const FQuat Heading=(Goal-Here).GetSafeNormal2D().Rotation().Quaternion();
    const float DriveNormalZ=FMath::Cos(FMath::DegreesToRadians(MaxClimbAngle));
    for (int32 Index=1;Index<=Samples;++Index)
    {
        FVector Probe=FMath::Lerp(Here,Goal,double(Index)/Samples);
        // 探地高度跟随上一段真实支撑，缓坡累计的高差不能变成中途悬空
        Probe.Z=Previous.Z;
        FVector Center;
        if (!IsSupportedVehicleSpace(Probe,Heading,DriveNormalZ,&Center,true) ||
            FMath::Abs(Center.Z-Previous.Z)>65) return false;
        if (!HasClearDrivingSegment(Previous,Center,Heading))
        {
            // 坡脊处两采样间的直线可能擦地，按原车 TryStepUp 的升起、前进、落下逐段核验
            const FVector RaisedStart=Previous+FVector(0,0,FMath::Max(0.f,MaxStepHeight));
            const FVector RaisedEnd(Center.X,Center.Y,RaisedStart.Z);
            if (RaisedEnd.Z<Center.Z || !HasClearDrivingSegment(Previous,RaisedStart,Heading) ||
                !HasClearDrivingSegment(RaisedStart,RaisedEnd,Heading) || !HasClearDrivingSegment(RaisedEnd,Center,Heading)) return false;
        }
        Previous=Center;
    }
    // 有高度的车迹必须到同一层实地，水平探路仍逐段检查坡度、支撑与车体净空
    if (bRequireGoalHeight && FMath::Abs(Previous.Z-Goal.Z)>180.) return false;
    auto* Home=ADeliveryHome::FindSharedHome(this);
    const FVector Height(0,0,HoverHeight);
    if (IsValid(Home) && Home->CrossesPedestrianDoorway(Here-Height,Previous-Height)) return false;
    if (!bDriving)
    {
        ProgressLocation=Here;
        LastProgressTime=GetWorld()->GetTimeSeconds();
    }
    DrivePath=nullptr;
    DrivePedestrianHome=Home;
    DriveGoal=Previous;
    bDrivingDirect=true;
    bDrivingToWalkExit=false;
    bDriving=true;
    bArrived=false;
    DriveFailure=NAME_None;
    return true;
}

bool ADeliveryCompanionMotorbike::HasActiveDriveRoute(bool bRequireWalkExit) const
{
    return bDriving && (!bRequireWalkExit || bDrivingToWalkExit) && DriveFailure.IsNone() && (bDrivingDirect ||
        (DrivePath && DrivePath->IsValid() && !DrivePath->IsPartial()));
}

void ADeliveryCompanionMotorbike::StopDriving()
{
    if (!HasAuthority()) return;
    FollowBike.Reset();
    DrivePedestrianHome.Reset();
    bDriving=false; bDrivingDirect=false; bDrivingToWalkExit=false; DrivePath=nullptr; PathPointIndex=1;
    StopAutonomousMotion();
}

void ADeliveryCompanionMotorbike::SetFollowBike(ADeliveryMotorbike* Bike,float DesiredDistance)
{
    if (!HasAuthority() || Bike==this || (Bike && Bike->GetWorld()!=GetWorld())) return;
    FollowBike=Bike;
    FollowDistance=DesiredDistance;
}

float ADeliveryCompanionMotorbike::GetForwardSpeedLimit() const
{
    const auto* Bike=FollowBike.Get();
    return Bike?FMath::Max(MaxSpeed,FMath::Abs(Bike->GetCurrentSpeed())+FollowCatchUpSpeed):MaxSpeed;
}

void ADeliveryCompanionMotorbike::FailDrive(FName Reason)
{
    StopDriving(); bArrived=false; DriveFailure=Reason;
}

void ADeliveryCompanionMotorbike::UpdateAutonomousDriveInput(float DeltaSeconds,float& OutThrottle,float& OutSteer)
{
    OutThrottle=OutSteer=0;
    if (!HasAuthority() || !bDriving) return;
    if (!CanCompanionRide()) { FailDrive(TEXT("CompanionInterrupted")); return; }
    if (!bDrivingDirect && (!DrivePath || !DrivePath->IsValid() || DrivePath->IsPartial()))
    { FailDrive(TEXT("PathInvalidated")); return; }
    const FVector Here=GetActorLocation();
    const float Remaining=FVector::Dist2D(Here,DriveGoal);
    // 需要步行续程时靠近验过出口的位置，避免在门框前提早停车
    const float ArrivalRadius=bDrivingToWalkExit?FMath::Min(30.f,GetParkingAcceptanceRadius()):GetParkingAcceptanceRadius();
    if (Remaining<=ArrivalRadius)
    {
        // 直线跟车的中途点到达后继续找路，正式停车仍由上层复核
        if (!bDrivingDirect && !CanParkAt(Here)) { FailDrive(TEXT("ParkingBlocked")); return; }
        StopDriving(); bArrived=true; DriveFailure=NAME_None;
        return;
    }
    const double Now=GetWorld()->GetTimeSeconds();
    if (FVector::DistSquared2D(Here,ProgressLocation)>FMath::Square(20.f))
    { ProgressLocation=Here; LastProgressTime=Now; }
    else if (Now-LastProgressTime>FMath::Max(1.f,BlockedTimeout))
    { FailDrive(TEXT("VehicleBlocked")); return; }
    FVector Aim=DriveGoal;
    if (!bDrivingDirect)
    {
        const auto& Points=DrivePath->PathPoints;
        if (!Points.IsValidIndex(PathPointIndex)) { FailDrive(TEXT("PathInvalidated")); return; }
        if (PathPointIndex<Points.Num()-1 && FVector::Dist2D(Here,Points[PathPointIndex])<FMath::Max(50.f,LookAheadDistance))
        {
            const FVector ToNext=Points[PathPointIndex+1]-Here;
            const FVector Direction=ToNext.GetSafeNormal2D();
            const FVector Near=Here+Direction*FMath::Min<double>(FMath::Max(50.f,LookAheadDistance),ToNext.Size2D());
            // 只看前方一段，车身切不过墙角时先贴近本路点，再尝试转向下一段
            if (FVector::Dist2D(Here,Points[PathPointIndex])<=25. ||
                (!Direction.IsNearlyZero() && HasClearDrivingSegment(Here,Near,Direction.Rotation().Quaternion())))
                ++PathPointIndex;
        }
        Aim=Points[PathPointIndex];
    }
    const FVector Height(0,0,HoverHeight);
    if (const auto* Home=DrivePedestrianHome.Get(); IsValid(Home) &&
        Home->CrossesPedestrianDoorway(Here-Height,bDrivingDirect?Aim-Height:Aim))
    {
        FailDrive(TEXT("WalkOnlyDoorway"));
        return;
    }
    const FVector Toward=(Aim-Here).GetSafeNormal2D();
    const float Angle=FMath::FindDeltaAngleDegrees(GetActorRotation().Yaw,Toward.Rotation().Yaw);
    OutSteer=FMath::Clamp(Angle/45.f,-1.f,1.f);
    // 近停车点和急弯前降低油门，位移始终由原车的碰撞移动完成
    const float Corner=FMath::GetMappedRangeValueClamped(FVector2D(20,100),FVector2D(1,.25f),FMath::Abs(Angle));
    float Approach=FMath::Clamp(Remaining/500.f,.15f,1.f);
    float DesiredSpeed=TravelSpeed;
    if (const auto* Bike=FollowBike.Get())
    {
        const double Gap=FVector::Dist2D(Here,Bike->GetActorLocation())-FollowDistance;
        const float OwnerSpeed=FMath::Abs(Bike->GetCurrentSpeed());
        const float Extra=FMath::Clamp(float(Gap*.7),-OwnerSpeed,FollowCatchUpSpeed);
        // 此时组件仍要求完成绕行或靠边停车，不能仅因两车直线距离近就停在墙的另一侧
        DesiredSpeed=FMath::Max(120.f,OwnerSpeed+Extra);
        // 开阔路段按车距控制速度，导航绕行仍在路径末端减速
        if (bDrivingDirect && Gap>150.) Approach=1.f;
    }
    OutThrottle=FMath::Clamp(DesiredSpeed/FMath::Max(GetForwardSpeedLimit(),1.f),0.f,1.f)*Corner*Approach;
}

bool ADeliveryCompanionMotorbike::HasClearDrivingSegment(const FVector& From,const FVector& To,const FQuat& Heading) const
{
    if (!GetWorld() || !CollisionBox) return false;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionDrivingSegment),false,this);
    Query.AddIgnoredActor(ReservedCompanion);
    return !GetWorld()->SweepTestByChannel(From,To,Heading,CollisionBox->GetCollisionObjectType(),
        FCollisionShape::MakeBox(CollisionBox->GetScaledBoxExtent()),Query,
        FCollisionResponseParams(CollisionBox->GetCollisionResponseToChannels()));
}

bool ADeliveryCompanionMotorbike::IsParkSpaceClear(const FVector& Goal,const FQuat& Rotation,FVector* OutVehicleCenter) const
{
    return IsSupportedVehicleSpace(Goal,Rotation,FMath::Cos(FMath::DegreesToRadians(25.f)),OutVehicleCenter);
}

bool ADeliveryCompanionMotorbike::IsSupportedVehicleSpace(const FVector& Goal,const FQuat& Rotation,float MinimumGroundNormalZ,
    FVector* OutVehicleCenter,bool bFitTravelClearance) const
{
    if (!GetWorld() || !CollisionBox || Goal.ContainsNaN()) return false;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionParking),false,this);
    Params.AddIgnoredActor(ReservedCompanion);
    FHitResult Ground;
    if (!GetWorld()->LineTraceSingleByChannel(Ground,Goal+FVector(0,0,150),Goal-FVector(0,0,300),ECC_Visibility,Params) ||
        Ground.ImpactNormal.Z<MinimumGroundNormalZ) return false;
    const FVector Extent=CollisionBox->GetScaledBoxExtent();
    FVector Center(Ground.ImpactPoint.X,Ground.ImpactPoint.Y,Ground.ImpactPoint.Z+HoverHeight);
    double HighestSupportZ=Ground.ImpactPoint.Z;
    // 检查整个车体及四角支撑，防止跨边缘时只剩中心落在地面
    for (const float X:{-.85f,.85f}) for (const float Y:{-.85f,.85f})
    {
        const FVector Offset=Rotation.RotateVector(FVector(Extent.X*X,Extent.Y*Y,0));
        FVector Corner=Ground.ImpactPoint+Offset;
        // 行驶时以实测坡面估计四角高度，仍须逐角找到真实支撑
        if (bFitTravelClearance && Ground.ImpactNormal.Z>UE_SMALL_NUMBER)
            Corner.Z-=FVector::DotProduct(Offset,Ground.ImpactNormal)/Ground.ImpactNormal.Z;
        // 跨坡脊时另一角可能已在反坡，探测同时覆盖中心高度与本侧坡面预测高度
        const double TopZ=FMath::Max(Ground.ImpactPoint.Z,Corner.Z)+50.;
        const double BottomZ=FMath::Min(Ground.ImpactPoint.Z,Corner.Z)-50.;
        FHitResult Support;
        if (!GetWorld()->LineTraceSingleByChannel(Support,FVector(Corner.X,Corner.Y,TopZ),
            FVector(Corner.X,Corner.Y,BottomZ),ECC_Visibility,Params) ||
            Support.ImpactNormal.Z<MinimumGroundNormalZ) return false;
        HighestSupportZ=FMath::Max(HighestSupportZ,Support.ImpactPoint.Z);
    }
    // 行驶预检给平放的车身盒留出坡面净空，抬升上限沿用真实越障配置
    if (bFitTravelClearance)
        Center.Z+=FMath::Clamp(HighestSupportZ+Extent.Z+8.-Center.Z,0.,static_cast<double>(MaxStepHeight));
    const auto HasBodyClearance=[&]()
    {
        return !GetWorld()->OverlapBlockingTestByChannel(Center,Rotation,CollisionBox->GetCollisionObjectType(),
            FCollisionShape::MakeBox(Extent),Params,FCollisionResponseParams(CollisionBox->GetCollisionResponseToChannels()));
    };
    bool bClear=HasBodyClearance();
    // 尖坡顶可能落在中心和角支撑之间，只在车辆原有越阶高度内检查整个车盒的净空
    const double HighestCenter=Ground.ImpactPoint.Z+HoverHeight+FMath::Max(0.f,MaxStepHeight);
    while (!bClear && bFitTravelClearance && Center.Z<HighestCenter)
    {
        Center.Z=FMath::Min(Center.Z+8.,HighestCenter);
        bClear=HasBodyClearance();
    }
    if (!bClear) return false;
    if (OutVehicleCenter) *OutVehicleCenter=Center;
    return true;
}

bool ADeliveryCompanionMotorbike::CanParkAt(const FVector& Goal) const
{
    return IsParkSpaceClear(Goal,FRotator(0,GetActorRotation().Yaw,0).Quaternion());
}

bool ADeliveryCompanionMotorbike::FindFooting(const FVector& Near,FVector& OutLocation) const
{
    const auto* Npc=GetReservedCompanion();
    const auto* Capsule=Npc?Npc->GetCapsuleComponent():nullptr;
    if (!GetWorld() || !Capsule) return false;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionDismount),false,this);
    Params.AddIgnoredActor(Npc);
    FHitResult Ground;
    if (!GetWorld()->LineTraceSingleByChannel(Ground,Near+FVector(0,0,150),Near-FVector(0,0,350),ECC_Visibility,Params) ||
        Ground.ImpactNormal.Z<Npc->GetCharacterMovement()->GetWalkableFloorZ()) return false;
    const float Radius=Capsule->GetScaledCapsuleRadius();
    const float Height=Capsule->GetScaledCapsuleHalfHeight();
    OutLocation=Ground.ImpactPoint+FVector(0,0,Height+3);
    return !GetWorld()->OverlapBlockingTestByChannel(OutLocation,FQuat::Identity,Capsule->GetCollisionObjectType(),
        FCollisionShape::MakeCapsule(Radius,Height),Params,FCollisionResponseParams(Capsule->GetCollisionResponseToChannels()));
}

bool ADeliveryCompanionMotorbike::FindDismountLocation(FVector& OutLocation,const FVector* WalkGoal) const
{
    if (!IsValid(CompanionSeat)) return false;
    return FindDismountLocationAtPose(GetActorLocation(),FRotator(0,GetActorRotation().Yaw,0).Quaternion(),
        CompanionSeat->GetComponentLocation(),OutLocation,WalkGoal);
}

bool ADeliveryCompanionMotorbike::FindDismountLocationAtPose(const FVector& VehicleCenter,const FQuat& VehicleHeading,
    const FVector& SeatLocation,FVector& OutLocation,const FVector* WalkGoal) const
{
    auto* Npc=GetReservedCompanion();
    if (!Npc || !CollisionBox || VehicleCenter.ContainsNaN() || VehicleHeading.ContainsNaN() || SeatLocation.ContainsNaN()) return false;
    const FVector Extent=CollisionBox->GetScaledBoxExtent();
    const float Radius=Npc->GetSimpleCollisionRadius()+35;
    const auto* Capsule=Npc->GetCapsuleComponent();
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (WalkGoal && (!Nav || WalkGoal->ContainsNaN())) return false;
    TOptional<FVector> SafeFooting;
    for (const FVector Offset:{FVector(0,Extent.Y+Radius,0),FVector(0,-Extent.Y-Radius,0),
        FVector(-Extent.X-Radius,0,0),FVector(Extent.X+Radius,0,0)})
    {
        const FVector Near=VehicleCenter+VehicleHeading.RotateVector(Offset);
        FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionExitSide),false,this);
        Params.AddIgnoredActor(Npc);
        FHitResult Wall;
        // 下车不能穿过车边墙体落到墙另一侧
        if (GetWorld()->LineTraceSingleByChannel(Wall,SeatLocation,Near+FVector(0,0,40),ECC_Visibility,Params)) continue;
        FVector Footing;
        if (!FindFooting(Near,Footing)) continue;
        const FVector Feet=Footing-FVector(0,0,Capsule->GetScaledCapsuleHalfHeight());
        const double ParkingGround=VehicleCenter.Z-HoverHeight;
        if (WalkGoal && FMath::Abs(Feet.Z-3.-ParkingGround)>Npc->GetCharacterMovement()->MaxStepHeight+5.f) continue;
        if (!GetWorld()->SweepSingleByChannel(Wall,SeatLocation,Footing,FQuat::Identity,
            Capsule->GetCollisionObjectType(),FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(),Capsule->GetScaledCapsuleHalfHeight()),
            Params,FCollisionResponseParams(Capsule->GetCollisionResponseToChannels())))
        {
            // 先尝试贴合步行导航的出口，仍保留物理安全的落脚点用于中断和无导航场景
            FNavLocation Projected;
            if (!Nav || (Nav->ProjectPointToNavigation(Feet,Projected,FVector(20,20,35),&Npc->GetNavAgentPropertiesRef()) &&
                FVector::DistSquared(Projected.Location,Feet)<=FMath::Square(20.f)))
            {
                // 车的另一侧可能属于不连通的步行区域，继续检查其余出口
                if (WalkGoal)
                {
                    const auto* Path=Nav->FindPathToLocationSynchronously(GetWorld(),Projected.Location,*WalkGoal,Npc);
                    if (!Path || !Path->IsValid() || Path->IsPartial()) continue;
                }
                OutLocation=Footing;
                return true;
            }
            if (!WalkGoal && !SafeFooting.IsSet()) SafeFooting=Footing;
        }
    }
    if (!SafeFooting.IsSet()) return false;
    OutLocation=SafeFooting.GetValue();
    return true;
}

bool ADeliveryCompanionMotorbike::TryDismount()
{
    return TryDismountAtExit(nullptr);
}

bool ADeliveryCompanionMotorbike::TryDismountIfClear()
{
    return FMath::Abs(GetCurrentSpeed())<=30.f && TryDismountAtExit(nullptr,true);
}

bool ADeliveryCompanionMotorbike::TryDismountForWalk(const FVector& WalkGoal)
{
    return TryDismountAtExit(&WalkGoal);
}

bool ADeliveryCompanionMotorbike::TryDismountAtExit(const FVector* WalkGoal,bool bCheckExitBeforeStopping)
{
    if (!HasAuthority()) return false;
    const double Now=GetWorld()->GetTimeSeconds();
    // 行为树和车辆 Tick 可以同时请求下车，碰撞复核共用同一个重试间隔
    if (bMounted && bDismountPending && Now<NextDismountAttempt) return false;
    FVector Exit;
    // 跟随仍可重选停车点，找不到出口时继续保留这段车辆路线
    const bool bCheckExit=WalkGoal || bCheckExitBeforeStopping;
    if (bCheckExit && (!bMounted || (WalkGoal && bDismountPending) || !FindDismountLocation(Exit,WalkGoal))) return false;
    FollowHornEndsAt=0.;
    SetHornActive(false);
    StopDriving();
    if (!bMounted) { bDismountPending=false; return true; }
    bDismountPending=true;
    NextDismountAttempt=Now+.25;
    auto* Npc=GetReservedCompanion();
    if (!Npc) { bMounted=false; bDismountPending=false; ApplyCompanionPresentation(); ForceNetUpdate(); return true; }
    if (!bCheckExit && !FindDismountLocation(Exit)) { DriveFailure=TEXT("DismountBlocked"); return false; }
    bMounted=false; bDismountPending=false;
    Npc->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    Npc->SetActorLocation(Exit,false,nullptr,ETeleportType::TeleportPhysics);
    Npc->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
    const TWeakObjectPtr<ADeliveryCompanionMotorbike> WeakThis(this);
    const TWeakObjectPtr<ADeliveryNpcCharacter> WeakNpc(Npc);
    ApplyCompanionPresentation();
    if (!WeakThis.IsValid() || !WeakNpc.IsValid() || IsActorBeingDestroyed() || Npc->IsActorBeingDestroyed()) return false;
    Npc->ForceNetUpdate(); ForceNetUpdate();
    if (DriveFailure==TEXT("DismountBlocked")) DriveFailure=NAME_None;
    const auto* Arrangement=Npc->FindComponentByClass<UDeliveryCompanionComponent>();
    UE_LOG(LogDeliveryCompanionBike,Log,TEXT("CompanionDismounted npc=%s bike=%s revision=%d"),
        *GetNameSafe(Npc),*GetName(),Arrangement?Arrangement->GetCommandRevision():INDEX_NONE);
    return true;
}

void ADeliveryCompanionMotorbike::ApplyCompanionPresentation()
{
    const TWeakObjectPtr<ADeliveryCompanionMotorbike> WeakThis(this);
    auto* Npc=GetReservedCompanion();
    if (PresentedCompanion.Get()!=Npc || !bMounted) RestoreCompanionPresentation();
    if (!WeakThis.IsValid() || IsActorBeingDestroyed()) return;
    Npc=GetReservedCompanion();
    if (!bMounted || !Npc || !HasRidePose()) { CompanionRiderMesh->SetVisibility(false); return; }
    if (PresentedCompanion.Get()!=Npc)
    {
        const auto* Source=Npc->GetMesh();
        CompanionRiderMesh->SetSkeletalMeshAsset(Source->GetSkeletalMeshAsset());
        CompanionRiderMesh->EmptyOverrideMaterials();
        for (int32 Index=0;Index<Source->GetNumMaterials();++Index) CompanionRiderMesh->SetMaterial(Index,Source->GetMaterial(Index));
        FTransform Relative=Source->GetRelativeTransform();
        Relative.AddToTranslation(RideMeshOffset);
        Relative.SetScale3D(Source->GetComponentScale());
        CompanionRiderMesh->SetRelativeTransform(Relative);
        CompanionRiderMesh->PlayAnimation(RideAnimation,true);
        PresentedCompanion=Npc;
    }
    // 场景阶段与车状态可能先后复制，迟到的初遇回调不能让骑乘中的原身体重新走动
    if (!Npc->IsHidden()) Npc->SetActorHiddenInGame(true);
    if (Npc->GetActorEnableCollision()) Npc->SetActorEnableCollision(false);
    if (Npc->GetCharacterMovement()->MovementMode!=MOVE_None) Npc->GetCharacterMovement()->DisableMovement();
    if (!CompanionRiderMesh->IsVisible()) CompanionRiderMesh->SetVisibility(true);
    if (RiderMesh && RiderMesh->IsVisible()) RiderMesh->SetVisibility(false);
    RefreshCompanionPackage(Npc);
}

void ADeliveryCompanionMotorbike::RestoreCompanionPresentation()
{
    const TWeakObjectPtr<ADeliveryNpcCharacter> Previous=PresentedCompanion;
    PresentedCompanion.Reset();
    CompanionRiderMesh->SetVisibility(false);
    if (auto* Npc=Previous.Get(); IsValid(Npc) && !Npc->IsActorBeingDestroyed())
    {
        // 旧车的迟到清理不能覆盖已经换到其他车的角色
        if (!Npc->GetAttachParentActor() || Npc->GetAttachParentActor()==this)
        {
            if (!HasAuthority() && Npc->GetAttachParentActor()==this)
                Npc->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
            Npc->SetActorHiddenInGame(false);
            Npc->SetActorEnableCollision(true);
            Npc->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
            RefreshCompanionPackage(Npc);
        }
    }
}

void ADeliveryCompanionMotorbike::RefreshCompanionPackage(ADeliveryNpcCharacter* Npc)
{
    if (!IsValid(Npc) || Npc->IsActorBeingDestroyed() || Npc->GetWorld()!=GetWorld()) return;
    const auto* Carrier=Cast<IDeliveryHandheldCarrierInterface>(Npc);
    auto* Package=Carrier?Carrier->GetCarriedHandheldItem():nullptr;
    if (!IsValid(Package) || Package->IsActorBeingDestroyed() || Package->GetOwner()!=Npc || Package->GetWorld()!=GetWorld()) return;
    if (bMounted && ReservedCompanion==Npc && Package->GetRootComponent()->GetAttachParent()==CompanionCargoAnchor &&
        !Package->IsHidden() && !Package->GetActorEnableCollision()) return;
    // 挂点刷新可能进入交接或销毁回调，返回后只通知仍属于原同伴的同一份物品
    const TWeakObjectPtr<ADeliveryCompanionMotorbike> WeakThis(this);
    const TWeakObjectPtr<ADeliveryNpcCharacter> WeakNpc(Npc);
    const TWeakObjectPtr<ADeliveryHandheldItem> WeakPackage(Package);
    Package->RefreshCarrierPresentation();
    if (WeakThis.IsValid() && WeakNpc.IsValid() && WeakPackage.IsValid() &&
        !WeakThis->IsActorBeingDestroyed() && WeakPackage->GetOwner()==WeakNpc.Get() && HasAuthority())
        WeakPackage->ForceNetUpdate();
}

void ADeliveryCompanionMotorbike::OnRep_CompanionRide() { ApplyCompanionPresentation(); }

void ADeliveryCompanionMotorbike::Tick(float DeltaSeconds)
{
    const TWeakObjectPtr<ADeliveryCompanionMotorbike> WeakThis(this);
    if (bMounted) ApplyCompanionPresentation();
    if (!WeakThis.IsValid() || IsActorBeingDestroyed()) return;
    if (HasAuthority() && bMounted && bDismountPending)
        TryDismount();
    else if (HasAuthority() && bMounted && !CanCompanionRide())
    {
        const auto* Npc=GetReservedCompanion();
        const auto* Companion=Npc?Npc->FindComponentByClass<UDeliveryCompanionComponent>():nullptr;
        if (Companion && Npc->GetAttachParentActor()==this && Companion->CanKeepVehicleSeatWhilePaused()) StopDriving();
        else
        {
            FailDrive(TEXT("CompanionInterrupted"));
            TryDismount();
        }
    }
    Super::Tick(DeltaSeconds);
    if (!WeakThis.IsValid() || IsActorBeingDestroyed()) return;
    if ((bMounted && PresentedCompanion.Get()!=GetReservedCompanion()) || (!bMounted && PresentedCompanion.IsValid()))
        ApplyCompanionPresentation();
}

void ADeliveryCompanionMotorbike::ReleaseForEndPlay(EEndPlayReason::Type Reason)
{
    StopDriving();
    auto* Npc=GetReservedCompanion();
    if (HasAuthority() && bMounted && Npc && !Npc->IsActorBeingDestroyed())
    {
        FVector Exit;
        // 销毁只在当前位置落脚，切图时只解除附着，不查询正在退出的旧世界
        const bool bHasExit=Reason==EEndPlayReason::Destroyed &&
            (FindDismountLocation(Exit) || FindFooting(GetActorLocation(),Exit));
        Npc->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        if (bHasExit) Npc->SetActorLocation(Exit,false,nullptr,ETeleportType::TeleportPhysics);
        Npc->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
        Npc->ForceNetUpdate();
    }
    bMounted=false;
    RestoreCompanionPresentation();
    ReservedCompanion=nullptr;
}

void ADeliveryCompanionMotorbike::EndPlay(const EEndPlayReason::Type Reason)
{
    ReleaseForEndPlay(Reason);
    Super::EndPlay(Reason);
}

void ADeliveryCompanionMotorbike::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ADeliveryCompanionMotorbike,ReservedCompanion);
    DOREPLIFETIME(ADeliveryCompanionMotorbike,bMounted);
}
