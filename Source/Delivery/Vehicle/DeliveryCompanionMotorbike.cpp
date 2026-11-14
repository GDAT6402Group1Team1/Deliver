#include "Vehicle/DeliveryCompanionMotorbike.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
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
#include "Interaction/DeliveryInteractableComponent.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "Net/UnrealNetwork.h"

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

bool ADeliveryCompanionMotorbike::TryMount(ADeliveryNpcCharacter* Companion)
{
    if (!HasAuthority() || Companion!=GetReservedCompanion()) return false;
    if (bMounted) return true;
    if (!CanCompanionRide() || Companion->IsHidden() || !Companion->GetActorEnableCollision() ||
        Companion->GetAttachParentActor() || !Companion->GetCharacterMovement()->IsMovingOnGround()) return false;
    if (!HasRidePose()) { DriveFailure=TEXT("RidePoseMissingOrIncompatible"); return false; }
    const FVector Difference=Companion->GetActorLocation()-GetBoardingLocation();
    if (Difference.SizeSquared2D()>FMath::Square(FMath::Max(30.f,BoardingRadius)) || FMath::Abs(Difference.Z)>150) return false;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionBoarding),false,this);
    Params.AddIgnoredActor(Companion);
    FHitResult Hit;
    if (GetWorld()->LineTraceSingleByChannel(Hit,Companion->GetPawnViewLocation(),CompanionSeat->GetComponentLocation(),ECC_Visibility,Params)) return false;
    CastChecked<AAIController>(Companion->GetController())->StopMovement();
    StopDriving();
    DriveFailure=NAME_None; bArrived=false; bDismountPending=false;
    Companion->GetCharacterMovement()->StopMovementImmediately();
    Companion->GetCharacterMovement()->DisableMovement();
    // 控制器保持在原人物，只有附着和表现改变
    Companion->SetActorEnableCollision(false);
    Companion->AttachToComponent(CompanionSeat,FAttachmentTransformRules::SnapToTargetNotIncludingScale);
    bMounted=true;
    const TWeakObjectPtr<ADeliveryCompanionMotorbike> WeakThis(this);
    const TWeakObjectPtr<ADeliveryNpcCharacter> WeakNpc(Companion);
    ApplyCompanionPresentation();
    if (!WeakThis.IsValid() || !WeakNpc.IsValid() || IsActorBeingDestroyed() || Companion->IsActorBeingDestroyed() ||
        !bMounted || ReservedCompanion!=Companion) return false;
    Companion->ForceNetUpdate(); ForceNetUpdate();
    return true;
}

bool ADeliveryCompanionMotorbike::SetDriveDestination(const FVector& Goal,bool bKeepDrivingWhileReplanning)
{
    if (!HasAuthority()) return false;
    const auto Reject=[&](FName Reason)
    {
        if (!bKeepDrivingWhileReplanning) FailDrive(Reason);
        return false;
    };
    if (!IsMounted() || bDismountPending || !CanCompanionRide()) return Reject(TEXT("CompanionUnavailable"));
    if (Goal.ContainsNaN() || !CanParkAt(Goal)) return Reject(TEXT("ParkingBlocked"));

    // 路线确认后再替换当前路线，移动主人重新定点时保留现有车速和卡住计时
    auto* NewPath=UNavigationSystemV1::FindPathToLocationSynchronously(GetWorld(),GetActorLocation(),Goal,ReservedCompanion);
    if (!NewPath || !NewPath->IsValid() || NewPath->IsPartial() || NewPath->PathPoints.Num()<2)
        return Reject(TEXT("PathUnavailable"));
    NewPath->EnableRecalculationOnInvalidation(ENavigationOptionFlag::Enable);
    if (!bKeepDrivingWhileReplanning || !bDriving)
    {
        // 跟随调用者已经设置当前主人车辆，接受新绕线路段时继续用它的车速
        if (!bKeepDrivingWhileReplanning) StopDriving();
        ProgressLocation=GetActorLocation();
        LastProgressTime=GetWorld()->GetTimeSeconds();
    }
    DrivePath=NewPath;
    bDrivingDirect=false;
    PathPointIndex=1;
    DriveGoal=Goal;
    bDriving=true;
    bArrived=false;
    DriveFailure=NAME_None;
    return true;
}

bool ADeliveryCompanionMotorbike::TrySetDirectFollowDestination(const FVector& Goal)
{
    if (!HasAuthority() || !IsMounted() || bDismountPending || !CanCompanionRide() || Goal.ContainsNaN()) return false;
    const FVector Here=GetActorLocation();
    const double Distance=FVector::Dist2D(Here,Goal);
    if (Distance<20 || Distance>2400 || FMath::Abs(Goal.Z-Here.Z)>180) return false;
    const int32 Samples=FMath::CeilToInt(Distance/150.);
    FVector Previous=Here;
    const FQuat Heading=(Goal-Here).GetSafeNormal2D().Rotation().Quaternion();
    FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionDirectFollow),false,this);
    Query.AddIgnoredActor(ReservedCompanion);
    if (!CollisionBox) return false;
    const FCollisionShape Shape=FCollisionShape::MakeBox(CollisionBox->GetScaledBoxExtent());
    for (int32 Index=1;Index<=Samples;++Index)
    {
        FVector Center;
        if (!IsParkSpaceClear(FMath::Lerp(Here,Goal,double(Index)/Samples),&Center) ||
            FMath::Abs(Center.Z-Previous.Z)>65 ||
            GetWorld()->SweepTestByChannel(Previous,Center,Heading,CollisionBox->GetCollisionObjectType(),
                Shape,Query,FCollisionResponseParams(CollisionBox->GetCollisionResponseToChannels()))) return false;
        Previous=Center;
    }
    if (!bDriving)
    {
        ProgressLocation=Here;
        LastProgressTime=GetWorld()->GetTimeSeconds();
    }
    DrivePath=nullptr;
    DriveGoal=Previous;
    bDrivingDirect=true;
    bDriving=true;
    bArrived=false;
    DriveFailure=NAME_None;
    return true;
}

bool ADeliveryCompanionMotorbike::HasActiveDriveRoute() const
{
    return bDriving && DriveFailure.IsNone() && (bDrivingDirect ||
        (DrivePath && DrivePath->IsValid() && !DrivePath->IsPartial()));
}

void ADeliveryCompanionMotorbike::StopDriving()
{
    if (!HasAuthority()) return;
    FollowBike.Reset();
    bDriving=false; bDrivingDirect=false; DrivePath=nullptr; PathPointIndex=1;
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
    if (Remaining<=GetParkingAcceptanceRadius())
    {
        if (!CanParkAt(Here)) { FailDrive(TEXT("ParkingBlocked")); return; }
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
        while (PathPointIndex<Points.Num()-1 && FVector::Dist2D(Here,Points[PathPointIndex])<FMath::Max(50.f,LookAheadDistance))
            ++PathPointIndex;
        Aim=Points[PathPointIndex];
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

bool ADeliveryCompanionMotorbike::IsParkSpaceClear(const FVector& Goal,FVector* OutVehicleCenter) const
{
    if (!GetWorld() || !CollisionBox || Goal.ContainsNaN()) return false;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionParking),false,this);
    Params.AddIgnoredActor(ReservedCompanion);
    FHitResult Ground;
    if (!GetWorld()->LineTraceSingleByChannel(Ground,Goal+FVector(0,0,150),Goal-FVector(0,0,300),ECC_Visibility,Params) ||
        Ground.ImpactNormal.Z<FMath::Cos(FMath::DegreesToRadians(25.f))) return false;
    const FVector Extent=CollisionBox->GetScaledBoxExtent();
    const FQuat Rotation=FRotator(0,GetActorRotation().Yaw,0).Quaternion();
    const FVector Center(Ground.ImpactPoint.X,Ground.ImpactPoint.Y,Ground.ImpactPoint.Z+HoverHeight);
    // 检查整个车体及四角支撑，不能只用导航点或车身中心判定可停放
    for (const float X:{-.85f,.85f}) for (const float Y:{-.85f,.85f})
    {
        const FVector Corner=Ground.ImpactPoint+Rotation.RotateVector(FVector(Extent.X*X,Extent.Y*Y,0));
        FHitResult Support;
        if (!GetWorld()->LineTraceSingleByChannel(Support,Corner+FVector(0,0,50),Corner-FVector(0,0,50),ECC_Visibility,Params) ||
            Support.ImpactNormal.Z<FMath::Cos(FMath::DegreesToRadians(25.f))) return false;
    }
    if (GetWorld()->OverlapBlockingTestByChannel(Center,Rotation,CollisionBox->GetCollisionObjectType(),
        FCollisionShape::MakeBox(Extent),Params,FCollisionResponseParams(CollisionBox->GetCollisionResponseToChannels()))) return false;
    if (OutVehicleCenter) *OutVehicleCenter=Center;
    return true;
}

bool ADeliveryCompanionMotorbike::CanParkAt(const FVector& Goal) const { return IsParkSpaceClear(Goal); }

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

bool ADeliveryCompanionMotorbike::FindDismountLocation(FVector& OutLocation) const
{
    const auto* Npc=GetReservedCompanion();
    if (!Npc || !CollisionBox) return false;
    const FVector Extent=CollisionBox->GetScaledBoxExtent();
    const float Radius=Npc->GetSimpleCollisionRadius()+35;
    const FQuat Rotation=FRotator(0,GetActorRotation().Yaw,0).Quaternion();
    for (const FVector Offset:{FVector(0,Extent.Y+Radius,0),FVector(0,-Extent.Y-Radius,0),
        FVector(-Extent.X-Radius,0,0),FVector(Extent.X+Radius,0,0)})
    {
        const FVector Near=GetActorLocation()+Rotation.RotateVector(Offset);
        FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionExitSide),false,this);
        Params.AddIgnoredActor(Npc);
        FHitResult Wall;
        // 下车不能穿过车边墙体落到墙另一侧
        if (GetWorld()->LineTraceSingleByChannel(Wall,CompanionSeat->GetComponentLocation(),Near+FVector(0,0,40),ECC_Visibility,Params)) continue;
        if (!FindFooting(Near,OutLocation)) continue;
        const auto* Capsule=Npc->GetCapsuleComponent();
        if (!GetWorld()->SweepSingleByChannel(Wall,CompanionSeat->GetComponentLocation(),OutLocation,FQuat::Identity,
            Capsule->GetCollisionObjectType(),FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(),Capsule->GetScaledCapsuleHalfHeight()),
            Params,FCollisionResponseParams(Capsule->GetCollisionResponseToChannels()))) return true;
    }
    return false;
}

bool ADeliveryCompanionMotorbike::TryDismount()
{
    if (!HasAuthority()) return false;
    FollowHornEndsAt=0.;
    SetHornActive(false);
    StopDriving();
    if (!bMounted) { bDismountPending=false; return true; }
    bDismountPending=true;
    NextDismountAttempt=GetWorld()->GetTimeSeconds()+.25;
    auto* Npc=GetReservedCompanion();
    if (!Npc) { bMounted=false; bDismountPending=false; ApplyCompanionPresentation(); ForceNetUpdate(); return true; }
    FVector Exit;
    if (!FindDismountLocation(Exit)) { DriveFailure=TEXT("DismountBlocked"); return false; }
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
    if (HasAuthority() && bMounted && !CanCompanionRide())
    {
        FailDrive(TEXT("CompanionInterrupted"));
        TryDismount();
    }
    else if (HasAuthority() && bMounted && bDismountPending && GetWorld()->GetTimeSeconds()>=NextDismountAttempt)
        TryDismount();
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
