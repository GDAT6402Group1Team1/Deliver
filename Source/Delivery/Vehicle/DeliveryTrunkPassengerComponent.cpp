#include "Vehicle/DeliveryTrunkPassengerComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Character/DeliveryCharacter.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "AbilitySystem/DeliverAbilitySystemComponent.h"
#include "AbilitySystem/DeliverAttributeSet.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "Curves/CurveFloat.h"
#include "Input/DeliveryInputComponent.h"
#include "Delivery.h"

UDeliveryTrunkPassengerComponent::UDeliveryTrunkPassengerComponent()
{
    SetIsReplicatedByDefault(true);
    PrimaryComponentTick.bCanEverTick=true;
    PrimaryComponentTick.TickGroup=TG_PrePhysics;
}

void UDeliveryTrunkPassengerComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UDeliveryTrunkPassengerComponent,State);
}

float UDeliveryTrunkPassengerComponent::Now() const
{
    if (!GetWorld()) return 0.f;
    const auto* GS=GetWorld()->GetGameState();
    return GS ? GS->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds();
}

float UDeliveryTrunkPassengerComponent::GetPhaseProgress() const
{
    return FMath::Clamp((Now()-State.StartedAt)/FMath::Max(TransitionDuration,.2f),0.f,1.f);
}

float UDeliveryTrunkPassengerComponent::GetRecoveryProgress() const
{
    const auto* Character=Cast<ADeliveryCharacter>(GetOwner());
    const auto* ASC=Character ? Character->GetAbilitySystemComponent():nullptr;
    return ASC ? FMath::Clamp(ASC->GetNumericAttribute(UDeliverAttributeSet::GetHealthAttribute())/
        FMath::Max(1.f,ASC->GetNumericAttribute(UDeliverAttributeSet::GetMaxHealthAttribute())*.4f),0.f,1.f):0.f;
}

FText UDeliveryTrunkPassengerComponent::GetStatusText() const
{
    switch(State.Phase)
    {
    case EDeliveryPassengerPhase::Loading: return NSLOCTEXT("Delivery","TrunkLoadingPlayer","Being put in the trunk");
    case EDeliveryPassengerPhase::WaitingToExit: return NSLOCTEXT("Delivery","TrunkWaitingExit","Recovering - waiting for a clear exit");
    case EDeliveryPassengerPhase::Unloading: return NSLOCTEXT("Delivery","TrunkLeavingPlayer","Getting out of the trunk");
    default: return NSLOCTEXT("Delivery","TrunkRecoveringPlayer","Recovering - you will get out automatically");
    }
}

void UDeliveryTrunkPassengerComponent::SetPhase(EDeliveryPassengerPhase Phase)
{
    State.Phase=Phase; State.StartedAt=Now();
    UE_LOG(LogDelivery,Log,TEXT("TrunkPassenger actor=%s phase=%d time=%.3f"),*GetNameSafe(GetOwner()),int(Phase),State.StartedAt);
    GetOwner()->ForceNetUpdate(); OnRep_State();
}

bool UDeliveryTrunkPassengerComponent::BeginRide(ADeliveryMotorbike* Vehicle)
{
    auto* Character=Cast<ADeliveryCharacter>(GetOwner());
    if (!Character || !Character->HasAuthority() || IsPassenger() || !IsValid(Vehicle) ||
        Vehicle->GetWorld()!=GetWorld() || Vehicle->GetTrunkPassenger()!=Character) return false;
    auto* Ragdoll=Character->GetActiveRagdoll();
    // 装入前记录经过路径与落点检查的回收点，失败时不能把箱口当作安全地面
    FVector Safe;
    if (!Vehicle->FindPassengerExit(Character,Safe)) return false;
    if (!Ragdoll || !Ragdoll->BeginTrunkPose()) return false;
    State.Vehicle=Vehicle; State.bWakeOnExit=false;
    LastVehicleLocation=Vehicle->GetActorLocation(); VehicleVelocity=FVector::ZeroVector;
    StartFrame=FTransform(FRotator(0,Character->GetActorRotation().Yaw,0),Ragdoll->GetPhysicalPelvisLocation());
    SafeLocations.Reset(); RememberSafeLocation(Safe); ExitRequestedAt=0.f;
    AddTickPrerequisiteActor(Vehicle);
    Ragdoll->AddTickPrerequisiteComponent(this);
    SetPhase(EDeliveryPassengerPhase::Loading);
    return true;
}

bool UDeliveryTrunkPassengerComponent::RequestExit(bool bWake)
{
    if (!GetOwner()->HasAuthority() || !IsPassenger() || bReleasing) return false;
    State.bWakeOnExit |= bWake;
    if (State.Phase==EDeliveryPassengerPhase::Unloading || State.Phase==EDeliveryPassengerPhase::WaitingToExit) return true;
    ExitRequestedAt=Now();
    // 装入期间苏醒也转入出箱，不在箱内开启站立电机
    SetPhase(EDeliveryPassengerPhase::WaitingToExit);
    return true;
}

void UDeliveryTrunkPassengerComponent::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Function)
{
    Super::TickComponent(Dt,Type,Function);
    if (!IsPassenger()) return;
    ApplyCollisionMode();
    auto* Character=CastChecked<ADeliveryCharacter>(GetOwner());
    auto* Ragdoll=Character->GetActiveRagdoll();
    if (!IsValid(State.Vehicle) || !Ragdoll) { Release(true); return; }
    auto* Bike=State.Vehicle.Get();
    VehicleVelocity=(Bike->GetActorLocation()-LastVehicleLocation)/FMath::Max(Dt,.001f);
    LastVehicleLocation=Bike->GetActorLocation();
    VehicleVelocity=VehicleVelocity.GetClampedToMaxSize(2500.f);
    if (!GetOwner()->HasAuthority()) return;
    FVector Safe;
    const bool bSafe=Bike->FindPassengerExit(Character,Safe);
    if (bSafe) RememberSafeLocation(Safe);
    if (GetRecoveryProgress()>=1.f) RequestExit(true);
    const float Elapsed=Now()-State.StartedAt;
    if (State.Phase==EDeliveryPassengerPhase::Loading && Elapsed>=TransitionDuration)
    {
        const bool bArrived=Ragdoll->GetTrunkPoseError()<=ArrivalTolerance &&
            FVector::Dist(Ragdoll->GetPhysicalPelvisLocation(),Bike->GetPassengerFrame().GetLocation())<=ArrivalTolerance;
        if (bArrived) SetPhase(EDeliveryPassengerPhase::Riding);
        else if (Elapsed>=TransitionDuration+SettleTimeout) Release(true);
    }
    else if (State.Phase==EDeliveryPassengerPhase::WaitingToExit)
    {
        if (bSafe)
        {
            StartFrame=Bike->GetPassengerFrame();
            ExitFrame=FTransform(StartFrame.GetRotation(),Safe);
            SetPhase(EDeliveryPassengerPhase::Unloading);
        }
        else if (Now()-ExitRequestedAt>=BlockedExitTimeout+TransitionDuration) Release(true);
    }
    else if (State.Phase==EDeliveryPassengerPhase::Unloading)
    {
        // 移动车辆的落点每帧重检，不能拉向已经被墙挡住的位置
        if (!bSafe) { SetPhase(EDeliveryPassengerPhase::WaitingToExit); return; }
        ExitFrame.SetLocation(Safe);
        if (Elapsed>=TransitionDuration && Ragdoll->GetTrunkPoseError()<=ArrivalTolerance &&
            FVector::Dist(Ragdoll->GetPhysicalPelvisLocation(),Safe)<=ArrivalTolerance) Release(false);
        else if (Elapsed>=TransitionDuration+SettleTimeout) Release(true);
    }
    else if (State.Phase==EDeliveryPassengerPhase::Riding && Ragdoll->GetTrunkPoseError()>160.f) Release(true);
}

void UDeliveryTrunkPassengerComponent::UpdatePhysics(float Dt)
{
    if (!GetOwner()->HasAuthority() || !IsValid(State.Vehicle)) return;
    auto* Ragdoll=CastChecked<ADeliveryCharacter>(GetOwner())->GetActiveRagdoll();
    FTransform Frame=State.Vehicle->GetPassengerFrame();
    float Blend=1.f, Unfold=0.f;
    const float CurveAlpha=TransitionCurve ? FMath::Clamp(TransitionCurve->GetFloatValue(GetPhaseProgress()),0.f,1.f)
        : FMath::SmoothStep(0.f,1.f,GetPhaseProgress());
    if (State.Phase==EDeliveryPassengerPhase::Loading)
    {
        // 先收拢，再提升至箱口，减少刚体拖过箱壁的距离
        Blend=FMath::SmoothStep(0.f,.5f,CurveAlpha);
        const float Move=FMath::SmoothStep(.2f,1.f,CurveAlpha);
        Frame.SetLocation(FMath::Lerp(StartFrame.GetLocation(),Frame.GetLocation(),Move));
    }
    else if (State.Phase==EDeliveryPassengerPhase::Unloading)
    {
        Unfold=CurveAlpha;
        Frame.SetLocation(FMath::Lerp(Frame.GetLocation(),ExitFrame.GetLocation(),Unfold));
    }
    Ragdoll->UpdateTrunkPose(Frame,Blend,Unfold,VehicleVelocity,Dt);
}

void UDeliveryTrunkPassengerComponent::ApplyCollisionMode()
{
    auto* Character=Cast<ADeliveryCharacter>(GetOwner());
    if (!Character || !Character->GetMesh()) return;
    if (CollisionVehicle.Get()==State.Vehicle && bSavedCollision) return;
    // 承载期间暂停 Pawn 碰撞，离箱恢复原响应；车体移动另做双向忽略
    if (IsValid(CollisionVehicle.Get()))
    {
        TArray<UPrimitiveComponent*> Parts; CollisionVehicle->GetComponents(Parts);
        for (auto* Part:Parts) Part->IgnoreActorWhenMoving(Character,false);
        Character->GetMesh()->IgnoreActorWhenMoving(CollisionVehicle.Get(),false);
    }
    if (bSavedCollision) Character->GetMesh()->SetCollisionResponseToChannel(ECC_Pawn,SavedPawnResponse);
    if (IsPassenger())
    {
        SavedPawnResponse=Character->GetMesh()->GetCollisionResponseToChannel(ECC_Pawn);
        Character->GetMesh()->SetCollisionResponseToChannel(ECC_Pawn,ECR_Ignore);
    }
    CollisionVehicle=State.Vehicle;
    bSavedCollision=IsPassenger();
    if (IsValid(State.Vehicle))
    {
        TArray<UPrimitiveComponent*> Parts; State.Vehicle->GetComponents(Parts);
        for (auto* Part:Parts) Part->IgnoreActorWhenMoving(Character,true);
        Character->GetMesh()->IgnoreActorWhenMoving(State.Vehicle,true);
    }
}

void UDeliveryTrunkPassengerComponent::OnRep_State()
{
    if (CollisionVehicle.Get()!=State.Vehicle)
    {
        if (CollisionVehicle.IsValid()) RemoveTickPrerequisiteActor(CollisionVehicle.Get());
        if (IsValid(State.Vehicle))
        {
            AddTickPrerequisiteActor(State.Vehicle);
            LastVehicleLocation=State.Vehicle->GetActorLocation();
            VehicleVelocity=FVector::ZeroVector;
        }
    }
    ApplyCollisionMode();
    if (auto* Character=Cast<ADeliveryCharacter>(GetOwner()))
    {
        if (auto* Input=Character->GetPlayerInput()) Input->ResetInput();
        if (auto* Ragdoll=Character->GetActiveRagdoll())
        {
            Ragdoll->SetMoveInput(FVector2D::ZeroVector);
            if (IsPassenger()) Ragdoll->AddTickPrerequisiteComponent(this);
            else Ragdoll->RemoveTickPrerequisiteComponent(this);
        }
    }
    if (IsValid(State.Vehicle)) State.Vehicle->RefreshPassengerPresentation();
}

void UDeliveryTrunkPassengerComponent::Release(bool bEmergency)
{
    if (!GetOwner()->HasAuthority() || !IsPassenger() || bReleasing) return;
    bReleasing=true;
    auto* Character=CastChecked<ADeliveryCharacter>(GetOwner());
    auto* Bike=State.Vehicle.Get();
    const bool bWake=State.bWakeOnExit;
    UE_LOG(LogDelivery,Log,TEXT("TrunkPassenger release actor=%s phase=%d emergency=%d wake=%d error=%.1f"),
        *GetNameSafe(Character),int(State.Phase),bEmergency,bWake,Character->GetActiveRagdoll()->GetTrunkPoseError());
    FVector Safe;
    const bool bSafe=bEmergency && FindRecordedExit(Safe);
    // 动态障碍可能占据曾经的落点，记录也必须重检；没有有效记录就继续等待
    if (bEmergency && !bSafe && IsValid(Bike) && !Bike->IsActorBeingDestroyed())
    {
        bReleasing=false;
        if (State.Phase!=EDeliveryPassengerPhase::WaitingToExit) RequestExit(bWake);
        return;
    }
    if (auto* Ragdoll=Character->GetActiveRagdoll())
    {
        Ragdoll->EndTrunkPose();
        if (bSafe) Ragdoll->EmergencyMoveTrunkBody(Safe);
        Ragdoll->ReleaseTrunkVelocity(ExitMaxSpeed);
        Ragdoll->RemoveTickPrerequisiteComponent(this);
    }
    if (IsValid(Bike)) RemoveTickPrerequisiteActor(Bike);
    State=FDeliveryPassengerState();
    SafeLocations.Reset();
    OnRep_State();
    if (IsValid(Bike)) Bike->ClearTrunkPassenger(Character);
    if (auto* ASC=Cast<UDeliverAbilitySystemComponent>(Character->GetAbilitySystemComponent()))
        ASC->CompleteTrunkRecovery(bWake);
    GetOwner()->ForceNetUpdate();
    bReleasing=false;
}

void UDeliveryTrunkPassengerComponent::RememberSafeLocation(const FVector& Location)
{
    if (SafeLocations.IsEmpty() || FVector::DistSquared(SafeLocations.Last(),Location)>FMath::Square(50.f))
    {
        if (SafeLocations.Num()==32) SafeLocations.RemoveAt(0);
        SafeLocations.Add(Location);
    }
}

bool UDeliveryTrunkPassengerComponent::FindRecordedExit(FVector& Location) const
{
    if (!GetWorld()) return false;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(TrunkRecordedExit),false,GetOwner());
    Params.AddIgnoredActor(State.Vehicle);
    if (IsValid(State.Vehicle)) Params.AddIgnoredActor(State.Vehicle->GetDriver());
    float Best=TNumericLimits<float>::Max();
    for (const FVector& Point:SafeLocations)
    {
        FHitResult Ground;
        if (!GetWorld()->LineTraceSingleByChannel(Ground,Point+FVector(0,0,30),Point-FVector(0,0,180),ECC_Visibility,Params) || Ground.ImpactNormal.Z<.65f) continue;
        const FVector Candidate=Ground.ImpactPoint+FVector(0,0,95);
        if (GetWorld()->OverlapBlockingTestByChannel(Candidate,FQuat::Identity,ECC_Visibility,FCollisionShape::MakeCapsule(42,85),Params)) continue;
        const float Distance=FVector::DistSquared(Candidate,GetOwner()->GetActorLocation());
        if (Distance<Best) { Best=Distance; Location=Candidate; }
    }
    return Best<TNumericLimits<float>::Max();
}

void UDeliveryTrunkPassengerComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    Release(false);
    State=FDeliveryPassengerState(); ApplyCollisionMode();
    Super::EndPlay(Reason);
}
