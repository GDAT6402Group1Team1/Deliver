#include "AI/Actions/DeliveryVehicleHoldComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

UDeliveryVehicleHoldComponent::UDeliveryVehicleHoldComponent()
{
    SetIsReplicatedByDefault(true);
    PrimaryComponentTick.bCanEverTick = true;
}
bool UDeliveryVehicleHoldComponent::FindHoldLocation(const ADeliveryNpcCharacter* Resident,FVector& Location) const
{
    Location=GetOwner()->GetActorLocation()-GetOwner()->GetActorForwardVector()*145.f;
    FHitResult Ground;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(ResidentHoldGround),false,GetOwner());
    if (Resident) Params.AddIgnoredActor(Resident);
    FCollisionObjectQueryParams Objects; Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    if (!GetWorld()->LineTraceSingleByObjectType(Ground,Location+FVector(0,0,180),Location-FVector(0,0,300),Objects,Params) || Ground.ImpactNormal.Z<.65f) return false;
    const float HalfHeight=Resident?Resident->GetCapsuleComponent()->GetScaledCapsuleHalfHeight():70.f;
    // 跟随车尾水平位置，竖直位置仍以脚下地面为准，不把车身高度当站立高度
    Location.Z=Ground.ImpactPoint.Z+HalfHeight+2.f;
    return true;
}
FVector UDeliveryVehicleHoldComponent::GetHoldLocation(const ADeliveryNpcCharacter* Resident) const
{
    FVector Location;
    FindHoldLocation(Resident?Resident:Holder.Get(),Location);
    return Location;
}
bool UDeliveryVehicleHoldComponent::CanHold(const ADeliveryNpcCharacter* Resident) const
{
    const auto* Bike = Cast<ADeliveryMotorbike>(GetOwner());
    if (!Bike || !IsValid(Resident) || Resident->GetWorld()!=GetWorld() || Resident->IsKnockedDown() ||
        IsHeld() || Resident->GetHeldVehicle() || FMath::Abs(Bike->GetCurrentSpeed())>100.f || !Bike->GetDriver() ||
        FVector::DistSquared(Resident->GetActorLocation(), GetHoldLocation(Resident))>FMath::Square(180.f)) return false;
    FVector Location;
    if (!FindHoldLocation(Resident,Location)) return false;
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(ResidentHold), false, Resident);
    Params.AddIgnoredActor(Bike);
    return !GetWorld()->SweepSingleByChannel(Hit, Resident->GetActorLocation(), Location, FQuat::Identity,
        ECC_Pawn, Resident->GetCapsuleComponent()->GetCollisionShape(), Params);
}
bool UDeliveryVehicleHoldComponent::TryHold(ADeliveryNpcCharacter* Resident, FGuid ActionId)
{
    if (!GetOwner()->HasAuthority() || !ActionId.IsValid() || !CanHold(Resident)) return false;
    Holder=Resident; HoldActionId=ActionId; ThrottleSeconds=0;
    ExpiresAt=GetWorld()->GetTimeSeconds()+FMath::Clamp(MaxHoldSeconds,.5f,5.f);
    Resident->SetHeldVehicle(this);
    OnRep_Holder(nullptr);
    GetOwner()->ForceNetUpdate();
    return true;
}
void UDeliveryVehicleHoldComponent::Release(FName Reason)
{
    if (!GetOwner()->HasAuthority()) return;
    auto* Previous=Holder.Get(); const FGuid PreviousId=HoldActionId;
    Holder=nullptr; HoldActionId.Invalidate(); ThrottleSeconds=0;
    OnRep_Holder(Previous);
    if (IsValid(Previous))
    {
        Previous->SetHeldVehicle(nullptr);
        Previous->Agent->TryFinishAction(PreviousId, EDeliveryNpcActionStatus::Succeeded,Reason);
    }
    GetOwner()->ForceNetUpdate();
}
void UDeliveryVehicleHoldComponent::UpdateThrottle(float Throttle,float Dt)
{
    if (!GetOwner()->HasAuthority() || !IsHeld()) return;
    // 按持续加油门的时间判断挣脱，不能按减速后的车速判断
    ThrottleSeconds=FMath::Abs(Throttle)>.8f ? ThrottleSeconds+Dt : 0;
    if (ThrottleSeconds>=EscapeThrottleSeconds) Release(TEXT("DriverEscaped"));
}
void UDeliveryVehicleHoldComponent::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn)
{
    Super::TickComponent(Dt,Type,Fn);
    if (!GetOwner()->HasAuthority() || !IsHeld()) return;
    const auto* Bike=Cast<ADeliveryMotorbike>(GetOwner());
    if (!Bike || !Bike->GetDriver() || Holder->IsKnockedDown() || GetWorld()->GetTimeSeconds()>=ExpiresAt ||
        Holder->Agent->GetPublicState().ActionId!=HoldActionId || !Holder->Agent->HasActiveAction())
    { Release(TEXT("HoldEnded")); return; }
    FVector Target;
    if (!FindHoldLocation(Holder,Target)) { Release(TEXT("HoldGroundLost")); return; }
    if (FVector::DistSquared(Target,Holder->GetActorLocation())>FMath::Square(250.f))
    { Release(TEXT("HoldTooFar")); return; }
    FHitResult Hit;
    Holder->SetActorLocation(FMath::VInterpTo(Holder->GetActorLocation(),Target,Dt,12.f),true,&Hit);
    Holder->SetActorRotation(GetOwner()->GetActorRotation());
    if (Hit.bBlockingHit) Release(TEXT("HoldObstructed"));
}
void UDeliveryVehicleHoldComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    if (GetOwner()->HasAuthority()) Release(TEXT("VehicleEndPlay"));
    else { auto* Previous=Holder.Get(); Holder=nullptr; OnRep_Holder(Previous); }
    Super::EndPlay(Reason);
}
void UDeliveryVehicleHoldComponent::OnRep_Holder(ADeliveryNpcCharacter* Previous)
{
    // UE 不会自动同步忽略碰撞的对象，因此服务器和驾驶客户端都要设置
    auto SetIgnored=[&](ADeliveryNpcCharacter* Resident,bool bIgnore)
    {
        if (!IsValid(Resident)) return;
        Resident->GetCapsuleComponent()->IgnoreActorWhenMoving(GetOwner(),bIgnore);
        if (auto* Body=Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent())) Body->IgnoreActorWhenMoving(Resident,bIgnore);
    };
    SetIgnored(Previous,false); SetIgnored(Holder,true);
}
void UDeliveryVehicleHoldComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(UDeliveryVehicleHoldComponent,Holder);
}

bool UDeliveryVehicleHoldComponent::IsHeld() const { return IsValid(Holder.Get()); }
