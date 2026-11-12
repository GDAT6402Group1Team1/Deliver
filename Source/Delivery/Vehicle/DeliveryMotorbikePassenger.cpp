#include "Vehicle/DeliveryMotorbike.h"
#include "Vehicle/DeliveryTrunkPassengerComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "AbilitySystem/DeliverAbilitySystemComponent.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Components/SceneComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"

bool ADeliveryMotorbike::HasTrunkPassenger() const { return IsValid(TrunkPassenger); }

bool ADeliveryMotorbike::CanUsePassengerTrunk(APawn* Interactor, bool)
{
    const auto* User = Cast<ADeliveryCharacter>(Interactor);
    if (!IsValid(User) || User->GetWorld() != GetWorld() || User->IsIncapacitated() ||
        Driver || IsTrunkBusy() || HasCargo() || !PassengerInteractable ||
        PassengerInteractable->GetInteractionDistance(User) > TrunkInteractRadius) return false;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(TrunkPassengerUse), false, this);
    Params.AddIgnoredActor(User);
    if (User->GetGrabComponent()) Params.AddIgnoredActor(User->GetGrabComponent()->GrabTarget);
    Params.AddIgnoredActor(TrunkPassenger);
    const FVector Point = PassengerInteractable->GetInteractionPoint(User->GetActorLocation());
    FHitResult Hit;
    if (GetWorld()->LineTraceSingleByChannel(Hit, User->GetPawnViewLocation(), Point, ECC_Visibility, Params)) return false;
    const auto* Grab = User->GetGrabComponent();
    if (HasTrunkPassenger()) return !Grab || !Grab->IsGrabbing();
    const auto* Target = Grab ? Cast<ADeliveryCharacter>(Grab->GrabTarget) : nullptr;
    if (!IsValid(Target) || Target == User || !Grab->IsDraggingCharacter() ||
        !Target->TrunkPassenger || Target->TrunkPassenger->IsPassenger() || Target->IsRidingVehicle() ||
        !Target->GetGrabbableComponent() || Target->GetGrabbableComponent()->GrabberCount != 1) return false;
    const auto* ASC = Cast<UDeliverAbilitySystemComponent>(Target->GetAbilitySystemComponent());
    // 本机提示可用身体状态，服务器只接受真实 HP 晕倒，不接受客户端指定目标
    if (HasAuthority() ? (!ASC || !ASC->IsStunned()) :
        (!Target->GetActiveRagdoll() || Target->GetActiveRagdoll()->GetControlMode()!=EDeliveryRagdollControlMode::Limp)) return false;
    if (FVector::Dist(Target->GetActorLocation(), GetPassengerFrame().GetLocation()) > 300.f) return false;
    Params.AddIgnoredActor(User); Params.AddIgnoredActor(Target);
    return !GetWorld()->LineTraceSingleByChannel(Hit, Target->GetActorLocation(), Point, ECC_Visibility, Params);
}

bool ADeliveryMotorbike::UsePassengerTrunk(APawn* Interactor)
{
    if (!HasAuthority() || !CanUsePassengerTrunk(Interactor, true)) return false;
    if (HasTrunkPassenger()) return TrunkPassenger->TrunkPassenger->RequestExit(false);
    auto* User = CastChecked<ADeliveryCharacter>(Interactor);
    auto* Target = Cast<ADeliveryCharacter>(User->GetGrabComponent()->GrabTarget);
    // 先占用后交接；BeginRide 失败不解除原拖拽
    TrunkPassenger = Target;
    if (!Target->TrunkPassenger->BeginRide(this)) { TrunkPassenger = nullptr; return false; }
    Target->GetGrabbableComponent()->ReleaseAllGrabbers();
    RefreshPassengerPresentation(); ForceNetUpdate();
    return true;
}

bool ADeliveryMotorbike::IsPassengerTransitioning() const
{
    return IsValid(TrunkPassenger) && TrunkPassenger->TrunkPassenger && TrunkPassenger->TrunkPassenger->IsTransitioning();
}

void ADeliveryMotorbike::ClearTrunkPassenger(ADeliveryCharacter* Passenger)
{
    if (!HasAuthority() || TrunkPassenger != Passenger) return;
    TrunkPassenger = nullptr;
    RefreshPassengerPresentation(); ForceNetUpdate();
}

FTransform ADeliveryMotorbike::GetPassengerFrame() const
{
    const FTransform Base = CargoPivot ? CargoPivot->GetComponentTransform() : GetActorTransform();
    return FTransform(Base.GetRotation() * PassengerRotation.Quaternion(),
        Base.GetLocation() + Base.TransformVectorNoScale(PassengerOffset));
}

bool ADeliveryMotorbike::FindPassengerExit(ADeliveryCharacter* Passenger, FVector& OutLocation) const
{
    if (!GetWorld()) return false;
    const FVector Center = GetPassengerFrame().GetLocation();
    const FVector Extent = CollisionBox ? CollisionBox->GetScaledBoxExtent() : FVector(110,45,45);
    const float Side = Extent.Y + 120.f;
    const FVector Offsets[] = {GetActorRightVector()*Side, -GetActorRightVector()*Side,
        -GetActorForwardVector()*(Extent.X+150.f)};
    FCollisionQueryParams Params(SCENE_QUERY_STAT(TrunkPassengerExit), false, this);
    Params.AddIgnoredActor(Passenger); Params.AddIgnoredActor(Driver);
    for (const auto& Offset : Offsets)
    {
        FHitResult Ground, Obstacle;
        const FVector Candidate = GetActorLocation() + Offset;
        if (!GetWorld()->LineTraceSingleByChannel(Ground, Candidate+FVector(0,0,150),
            Candidate-FVector(0,0,500), ECC_Visibility, Params) || Ground.ImpactNormal.Z < .65f) continue;
        const FVector Hips = Ground.ImpactPoint + FVector(0,0,95);
        if (GetWorld()->SweepSingleByChannel(Obstacle, Center, Hips, FQuat::Identity, ECC_Visibility,
            FCollisionShape::MakeSphere(35), Params)) continue;
        if (GetWorld()->OverlapBlockingTestByChannel(Hips, FQuat::Identity, ECC_Visibility,
            FCollisionShape::MakeCapsule(42,85), Params)) continue;
        OutLocation = Hips;
        return true;
    }
    return false;
}

void ADeliveryMotorbike::OnRep_TrunkPassenger() { RefreshPassengerPresentation(); }

void ADeliveryMotorbike::RefreshPassengerPresentation(float DeltaSeconds)
{
    if (!PassengerInteractable) return;
    PassengerInteractable->InteractAnchor = TrunkAnchor;
    PassengerInteractable->InteractRadius = TrunkInteractRadius;
    PassengerInteractable->bInteractEnabled = !Driver && !HasCargo();
    PassengerInteractable->PromptText = HasTrunkPassenger()
        ? NSLOCTEXT("Delivery", "UnloadPassenger", "E - Take Player Out")
        : NSLOCTEXT("Delivery", "LoadPassenger", "E - Put Dragged Player In");
    if (HasTrunkPassenger() && TrunkPassenger->TrunkPassenger)
    {
        const auto* Passenger = TrunkPassenger->TrunkPassenger.Get();
        const float Rest = PassengerLidAngle / FMath::Max(FMath::Abs(TrunkLidOpenAngle), 1.f);
        const float Alpha = Passenger->IsTransitioning() ? 1.f : Rest;
        ApplyTrunkLidAlpha(FMath::FInterpConstantTo(CurrentTrunkLidAlpha,Alpha,DeltaSeconds,3.f));
    }
    else if (TrunkPhase == EDeliveryTrunkPhase::Idle)
        ApplyTrunkLidAlpha(FMath::FInterpConstantTo(CurrentTrunkLidAlpha,0.f,DeltaSeconds,3.f));
}

void ADeliveryMotorbike::EndPlay(const EEndPlayReason::Type Reason)
{
    if (HasAuthority() && IsValid(TrunkPassenger) && TrunkPassenger->TrunkPassenger)
        TrunkPassenger->TrunkPassenger->Release(Reason == EEndPlayReason::Destroyed);
    // 驾驶权已在 Destroyed 交还，这里只解除各端表现缓存
    RestorePresentedDriverCollision();
    Super::EndPlay(Reason);
}
