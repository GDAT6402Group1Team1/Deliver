#include "Grab/DeliveryGrabbableComponent.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DeliveryCharacter.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Net/UnrealNetwork.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"

UDeliveryGrabbableComponent::UDeliveryGrabbableComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetIsReplicatedByDefault(true);
}

void UDeliveryGrabbableComponent::BeginPlay()
{
	Super::BeginPlay();
	if (GetOwner() && GetOwner()->HasAuthority() && !GetOwner()->IsA<ADeliveryCharacter>())
	{
		GetOwner()->SetReplicates(true);
		GetOwner()->SetReplicateMovement(true);
	}
}

void UDeliveryGrabbableComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ReleaseAllGrabbers();
	Super::EndPlay(EndPlayReason);
}

void UDeliveryGrabbableComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority() || !bKinematicCarry) return;
	FName Bone;
	UPrimitiveComponent* Body = GetGrabBody(Bone);
	if (!Body || Body != Owner->GetRootComponent()) return;
	FVector Target = FVector::ZeroVector;
	FVector TargetForward = FVector::ZeroVector;
	int32 Contributors = 0;
	for (const TWeakObjectPtr<ADeliveryCharacter>& Entry : Grabbers)
	{
		const ADeliveryCharacter* Character = Entry.Get();
		const UDeliveryGrabComponent* Grab = Character ? Character->GetGrabComponent() : nullptr;
		FVector CarryCenter;
		FVector CarryForward;
		if (Grab && Grab->GetCarryPoseFor(Owner, CarryCenter, CarryForward))
		{
			Target += CarryCenter;
			TargetForward += CarryForward;
			++Contributors;
		}
	}
	if (!Contributors) return;
	Target /= Contributors;
	const FVector OldLocation = Body->GetComponentLocation();
	const FVector Next = FMath::VInterpTo(OldLocation, Target, DeltaTime, CarryFollowSpeed);
	FRotator NextRotation = Owner->GetActorRotation();
	if (!TargetForward.IsNearlyZero())
	{
		const FRotator Upright(0.0f, TargetForward.Rotation().Yaw, 0.0f);
		NextRotation = FMath::RInterpTo(NextRotation, Upright, DeltaTime, CarryFollowSpeed);
	}
	FHitResult Hit;
	Owner->SetActorLocationAndRotation(Next, NextRotation, true, &Hit, ETeleportType::None);
	LastCarryVelocity = (Body->GetComponentLocation() - PreviousCarryLocation)
		/ FMath::Max(DeltaTime, KINDA_SMALL_NUMBER);
	LastCarryVelocity = LastCarryVelocity.GetClampedToMaxSize(500.0f);
	PreviousCarryLocation = Body->GetComponentLocation();
}

void UDeliveryGrabbableComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UDeliveryGrabbableComponent, GrabberCount);
	DOREPLIFETIME(UDeliveryGrabbableComponent, bKinematicCarry);
}

UPrimitiveComponent* UDeliveryGrabbableComponent::GetGrabBody(FName& OutBone) const
{
	OutBone = NAME_None;
	AActor* Owner = GetOwner();
	if (!Owner) return nullptr;
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Owner))
	{
		OutBone = TEXT("Spine");
		return Character->GetMesh();
	}
	TArray<UPrimitiveComponent*> Bodies;
	Owner->GetComponents(Bodies);
	for (UPrimitiveComponent* Body : Bodies)
	{
		if (Body && (Body->IsSimulatingPhysics() || (bKinematicCarry && Body == Owner->GetRootComponent()))
			&& (BodyComponentName.IsNone() || Body->GetFName() == BodyComponentName))
		{
			return Body;
		}
	}
	return nullptr;
}

bool UDeliveryGrabbableComponent::CanGrab(const ADeliveryCharacter* Requester) const
{
	if (!Requester || Requester == GetOwner()) return false;
	FName Bone;
	UPrimitiveComponent* Body = GetGrabBody(Bone);
	if (!Body || (!Body->IsSimulatingPhysics(Bone) && !bKinematicCarry)) return false;
	// Actor 的移动复制只保证根刚体；子组件单独模拟会在其他客户端漂离手部快照。
	if (!GetOwner()->IsA<ADeliveryCharacter>() && Body != GetOwner()->GetRootComponent()) return false;
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner()))
	{
		return Character->GetActiveRagdoll()
			&& Character->GetActiveRagdoll()->GetControlMode() == EDeliveryRagdollControlMode::Limp;
	}
	return true;
}

bool UDeliveryGrabbableComponent::RegisterGrabber(ADeliveryCharacter* Requester)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !CanGrab(Requester)) return false;
	Grabbers.RemoveAll([](const TWeakObjectPtr<ADeliveryCharacter>& Entry) { return !Entry.IsValid(); });
	if (!Grabbers.Contains(Requester))
	{
		if (Grabbers.Num() >= 2) return false;
		Grabbers.Add(Requester);
	}
	GrabberCount = Grabbers.Num();
	if (!GetOwner()->IsA<ADeliveryCharacter>() && !bKinematicCarry)
	{
		bKinematicCarry = true;
		PreviousCarryLocation = GetOwner()->GetActorLocation();
		LastCarryVelocity = FVector::ZeroVector;
		ApplyCarryMode();
	}
	GetOwner()->ForceNetUpdate();
	return true;
}

void UDeliveryGrabbableComponent::UnregisterGrabber(ADeliveryCharacter* Requester)
{
	if (!GetOwner() || !GetOwner()->HasAuthority()) return;
	Grabbers.RemoveAll([Requester](const TWeakObjectPtr<ADeliveryCharacter>& Entry)
	{
		return !Entry.IsValid() || Entry.Get() == Requester;
	});
	GrabberCount = Grabbers.Num();
	if (bKinematicCarry && GrabberCount == 0)
	{
		bKinematicCarry = false;
		ApplyCarryMode();
	}
	GetOwner()->ForceNetUpdate();
}

void UDeliveryGrabbableComponent::OnRep_KinematicCarry()
{
	ApplyCarryMode();
}

void UDeliveryGrabbableComponent::ApplyCarryMode()
{
	AActor* Owner = GetOwner();
	if (!Owner || Owner->IsA<ADeliveryCharacter>()) return;
	UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(Owner->GetRootComponent());
	if (!Body) return;
	if (bKinematicCarry)
	{
		if (!bHasSavedPawnResponse)
		{
			SavedPawnResponse = Body->GetCollisionResponseToChannel(ECC_Pawn);
			SavedPhysicsBodyResponse = Body->GetCollisionResponseToChannel(ECC_PhysicsBody);
			bHasSavedPawnResponse = true;
		}
		Body->SetSimulatePhysics(false);
		Body->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		Body->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Ignore);
	}
	else
	{
		if (bHasSavedPawnResponse)
		{
			Body->SetCollisionResponseToChannel(ECC_Pawn, SavedPawnResponse);
			Body->SetCollisionResponseToChannel(ECC_PhysicsBody, SavedPhysicsBodyResponse);
			bHasSavedPawnResponse = false;
		}
		Body->SetSimulatePhysics(true);
		if (Owner->HasAuthority()) Body->SetPhysicsLinearVelocity(LastCarryVelocity);
	}
}

void UDeliveryGrabbableComponent::ReleaseAllGrabbers()
{
	if (!GetOwner() || !GetOwner()->HasAuthority()) return;
	const TArray<TWeakObjectPtr<ADeliveryCharacter>> Current = Grabbers;
	for (const TWeakObjectPtr<ADeliveryCharacter>& Entry : Current)
	{
		if (ADeliveryCharacter* Character = Entry.Get())
		{
			if (UDeliveryGrabComponent* Grab = Character->GetGrabComponent()) Grab->ForceRelease();
		}
	}
	Grabbers.Reset();
	GrabberCount = 0;
	if (bKinematicCarry)
	{
		bKinematicCarry = false;
		ApplyCarryMode();
	}
}
