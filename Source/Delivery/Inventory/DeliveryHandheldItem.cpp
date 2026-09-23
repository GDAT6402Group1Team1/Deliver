// Copyright Epic Games, Inc. All Rights Reserved.

#include "Inventory/DeliveryHandheldItem.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DeliveryCharacter.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"

ADeliveryHandheldItem::ADeliveryHandheldItem()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	bReplicates = true;
	SetReplicateMovement(true);
	NetUpdateFrequency = 20.0f;

	ItemMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ItemMesh"));
	ItemMesh->SetCollisionProfileName(TEXT("PhysicsActor"));
	ItemMesh->SetSimulatePhysics(true);
	ItemMesh->SetIsReplicated(true);
	RootComponent = ItemMesh;

	ItemComponent = CreateDefaultSubobject<UDeliveryInventoryItemComponent>(TEXT("Item"));
	InteractableComponent = CreateDefaultSubobject<UDeliveryInteractableComponent>(TEXT("Interactable"));
	InteractableComponent->PromptText = NSLOCTEXT("DeliveryItems", "PickupPrompt", "[E] 拾取");
	InteractableComponent->InteractRadius = 100.0f;
	InteractableComponent->PromptOffset = FVector(0.0f, 0.0f, 45.0f);
}

void ADeliveryHandheldItem::BeginPlay()
{
	Super::BeginPlay();
	// E pickup is dispatched explicitly by ADeliveryCharacter. The generic F event is not bound here.
}

void ADeliveryHandheldItem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateHeldTransform(DeltaSeconds, false);
}

void ADeliveryHandheldItem::UpdateHeldTransform(float DeltaSeconds, bool bSnap)
{
	if (!bHeldPresentationActive || !HeldAnchor.IsValid()) return;

	// Only the grip position comes from the fully physical hand. Its rigid body can roll
	// several times while walking, so inheriting its rotation makes a held tool spin.
	// Build a stable grip frame from the character's replicated horizontal facing instead.
	FVector StableForward = FVector::ForwardVector;
	if (const ADeliveryCharacter* Holder = Cast<ADeliveryCharacter>(HeldAnchor->GetOwner()))
	{
		// The actor/capsule yaw follows camera aim in this project. Held tools must follow
		// the active-ragdoll body's facing, otherwise looking around rotates the tool.
		StableForward = Holder->GetActiveRagdoll()
			? Holder->GetActiveRagdoll()->GetBodyForward()
			: Holder->GetActorForwardVector().GetSafeNormal2D();
	}
	if (StableForward.IsNearlyZero()) StableForward = FVector::ForwardVector;
	const FQuat StableRotation = FRotationMatrix::MakeFromXZ(StableForward, FVector::UpVector).ToQuat();
	const FTransform StableGripFrame(StableRotation, HeldAnchor->GetComponentLocation(), FVector::OneVector);
	const FTransform Target = HeldRelativeTransform * StableGripFrame;
	if (bSnap)
	{
		SetActorLocationAndRotation(Target.GetLocation(), Target.GetRotation(), false, nullptr, ETeleportType::TeleportPhysics);
		return;
	}

	const float PositionAlpha = 1.0f - FMath::Exp(-HeldPositionFollowSpeed * DeltaSeconds);
	const float RotationAlpha = 1.0f - FMath::Exp(-HeldRotationFollowSpeed * DeltaSeconds);
	const FVector Location = FMath::Lerp(GetActorLocation(), Target.GetLocation(), PositionAlpha);
	const FQuat Rotation = FQuat::Slerp(GetActorQuat(), Target.GetRotation(), RotationAlpha).GetNormalized();
	SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
}

void ADeliveryHandheldItem::Use_Implementation(APawn* User)
{
}

void ADeliveryHandheldItem::HandleInteract(APawn* Interactor)
{
	ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Interactor);
	if (Character && Character->GetInventoryComponent())
	{
		Character->GetInventoryComponent()->TryPickup(this);
	}
}

void ADeliveryHandheldItem::SetInventoryPresentation(bool bInHand, bool bInInventory, USceneComponent* HandParent)
{
	// A simulated root cannot be attached reliably. Stop physics before changing the
	// attachment, then explicitly restore visibility because this actor may have just
	// come out of a hidden backpack slot.
	ItemMesh->SetSimulatePhysics(false);
	ItemMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetActorEnableCollision(false);
	SetActorHiddenInGame(!bInHand);
	ItemMesh->SetVisibility(bInHand, true);
	InteractableComponent->bInteractEnabled = false;

	if (bInHand && HandParent)
	{
		DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		HeldAnchor = HandParent;
		bHeldPresentationActive = true;
		SetActorTickEnabled(true);
		// Snap only on pickup, then follow after Chaos in TG_PostPhysics to filter hand jitter.
		UpdateHeldTransform(0.0f, true);
	}
	else if (bInInventory)
	{
		DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		bHeldPresentationActive = false;
		HeldAnchor.Reset();
		SetActorTickEnabled(false);
	}
}

void ADeliveryHandheldItem::DropFromInventory(const FVector& WorldLocation, const FVector& InitialVelocity)
{
	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	bHeldPresentationActive = false;
	HeldAnchor.Reset();
	SetActorTickEnabled(false);
	SetOwner(nullptr);
	SetActorHiddenInGame(false);
	ItemMesh->SetVisibility(true, true);
	SetActorEnableCollision(true);
	SetActorLocation(WorldLocation, false, nullptr, ETeleportType::TeleportPhysics);
	ItemMesh->SetCollisionProfileName(TEXT("PhysicsActor"));
	ItemMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	ItemMesh->SetSimulatePhysics(true);
	ItemMesh->SetPhysicsLinearVelocity(InitialVelocity);
	InteractableComponent->bInteractEnabled = true;
}
