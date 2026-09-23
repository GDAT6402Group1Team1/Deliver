// Copyright Epic Games, Inc. All Rights Reserved.

#include "Interaction/DeliveryInteractionProbeComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "DeliveryCharacter.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Task/DeliveryItemComponent.h"

UDeliveryInteractionProbeComponent::UDeliveryInteractionProbeComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// 走近/走远的判定不需要每帧重算，10Hz 眼睛看不出差别。
	PrimaryComponentTick.TickInterval = 0.1f;
	SetIsReplicatedByDefault(false);
}

void UDeliveryInteractionProbeComponent::TickComponent(
	float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const APawn* Owner = Cast<APawn>(GetOwner());
	// 不是本机操控的 Pawn 就别探测：包括别人的角色，也包括自己上了车之后被丢下的那具身体。
	if (!Owner || !Owner->IsLocallyControlled())
	{
		FocusedGeneral = nullptr;
		FocusedPickup = nullptr;
		RestorePickupHighlight();
		return;
	}

	// An incapacitated pawn cannot interact. Clear both input targets and the old highlight.
	const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Owner);
	if (Character && Character->IsIncapacitated())
	{
		FocusedGeneral = nullptr;
		FocusedPickup = nullptr;
		RestorePickupHighlight();
		return;
	}

	UDeliveryInteractableComponent* Pickup = FindAimedPickup(Owner);
	UDeliveryInteractableComponent* General = UDeliveryInteractableComponent::FindBest(
		Owner, EDeliveryInteractionKey::GeneralF);
	FocusedPickup = Pickup;
	FocusedGeneral = General;
	SetPickupHighlight(Pickup ? Pickup->GetOwner() : nullptr);

	UDeliveryInteractableComponent* PromptTarget = Pickup ? Pickup : General;
	if (bShowPrompt && PromptTarget)
	{
		if (UDeliveryPromptSubsystem* Prompt = UDeliveryPromptSubsystem::Get(this))
		{
			float Progress = -1.0f;
			if (Pickup)
			{
				if (Character)
				{
					Progress = Character->GetPickupHoldProgress(Pickup->GetOwner());
				}
			}
			FText Text = PromptTarget->PromptText;
			if (Pickup)
			{
				if (const UDeliveryItemComponent* Item = Pickup->GetOwner()->FindComponentByClass<UDeliveryItemComponent>())
				{
					const FText Reason = Item->GetPickupBlockedReason();
					if (!Reason.IsEmpty()) Text = Reason;
				}
			}
			Prompt->PushPrompt(Text, PromptTarget->GetPromptLocation(), Progress);
		}
	}
}

AActor* UDeliveryInteractionProbeComponent::GetFocusedActor() const
{
	const UDeliveryInteractableComponent* Target = FocusedGeneral.Get();
	return Target ? Target->GetOwner() : nullptr;
}

AActor* UDeliveryInteractionProbeComponent::GetFocusedPickupActor() const
{
	const UDeliveryInteractableComponent* Target = FocusedPickup.Get();
	return Target ? Target->GetOwner() : nullptr;
}

UDeliveryInteractableComponent* UDeliveryInteractionProbeComponent::FindAimedPickup(const APawn* Owner) const
{
	const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Owner);
	const UCameraComponent* Camera = Character ? Character->GetFollowCamera() : nullptr;
	UWorld* World = GetWorld();
	if (!Character || !Camera || !World) return nullptr;

	const FVector Start = Camera->GetComponentLocation();
	const FVector End = Start + Camera->GetForwardVector() * 3000.0f;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DeliveryPickupAim), false, Owner);
	if (Character->GetInventoryComponent()) Params.AddIgnoredActor(Character->GetInventoryComponent()->GetHeldItem());
	FHitResult Hit;
	if (!World->SweepSingleByChannel(Hit, Start, End, FQuat::Identity, ECC_Visibility,
		FCollisionShape::MakeSphere(10.0f), Params))
	{
		return nullptr;
	}

	UDeliveryInteractableComponent* Candidate = UDeliveryInteractableComponent::FindOn(Hit.GetActor());
	return Candidate && Candidate->InteractionKey == EDeliveryInteractionKey::PickupE
		&& Candidate->CanInteract(Owner) && Character->CanUsePickupTarget(Hit.GetActor(), false)
		? Candidate : nullptr;
}

void UDeliveryInteractionProbeComponent::SetPickupHighlight(AActor* Actor)
{
	if (HighlightedActor.Get() == Actor) return;
	RestorePickupHighlight();
	HighlightedActor = Actor;
	if (!Actor) return;

	TArray<UPrimitiveComponent*> Primitives;
	Actor->GetComponents(Primitives);
	for (UPrimitiveComponent* Primitive : Primitives)
	{
		if (!Primitive || !Primitive->IsVisible()) continue;
		FPrimitiveHighlightState& State = HighlightStates.AddDefaulted_GetRef();
		State.Component = Primitive;
		State.bRenderCustomDepth = Primitive->bRenderCustomDepth;
		State.StencilValue = Primitive->CustomDepthStencilValue;
		Primitive->SetRenderCustomDepth(true);
		Primitive->SetCustomDepthStencilValue(1);
	}
}

void UDeliveryInteractionProbeComponent::RestorePickupHighlight()
{
	for (const FPrimitiveHighlightState& State : HighlightStates)
	{
		if (UPrimitiveComponent* Primitive = State.Component.Get())
		{
			Primitive->SetRenderCustomDepth(State.bRenderCustomDepth);
			Primitive->SetCustomDepthStencilValue(State.StencilValue);
		}
	}
	HighlightStates.Reset();
	HighlightedActor.Reset();
}

void UDeliveryInteractionProbeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RestorePickupHighlight();
	Super::EndPlay(EndPlayReason);
}
