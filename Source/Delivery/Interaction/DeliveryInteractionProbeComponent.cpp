// Copyright Epic Games, Inc. All Rights Reserved.

#include "Interaction/DeliveryInteractionProbeComponent.h"
#include "Components/PrimitiveComponent.h"
#include "DeliveryCharacter.h"
#include "Camera/CameraComponent.h"
#include "Interaction/DeliveryInteractionGeometry.h"
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
	// 20Hz 更新范围内的 E 目标。
	PrimaryComponentTick.TickInterval = 0.05f;
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

	UDeliveryInteractableComponent* Pickup = FindPreferredPickup(Owner);
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

UDeliveryInteractableComponent* UDeliveryInteractionProbeComponent::FindPreferredPickup(const APawn* Owner) const
{
	const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Owner);
	if (!Character) return nullptr;

	TArray<UDeliveryInteractableComponent*> Candidates;
	UDeliveryInteractableComponent::GetReachablePickupCandidates(Owner, Candidates);
	UDeliveryInteractableComponent* Best = nullptr;
	float BestDistance = TNumericLimits<float>::Max();
	bool bBestUsable = false;
	bool bBestAimed = false;
	float BestMiss = TNumericLimits<float>::Max();
	const UCameraComponent* Camera = Character->GetFollowCamera();
	for (UDeliveryInteractableComponent* Candidate : Candidates)
	{
		AActor* Target = Candidate->GetOwner();
		if (!Character->CanUsePickupTarget(Target, false)) continue;
		const bool bUsable = Character->CanUsePickupTarget(Target);
		// Keep the held interaction stable while it remains valid and in range.
		if (bUsable && Character->GetPickupHoldProgress(Target) >= 0.0f) return Candidate;
		FVector Closest = Target->GetActorLocation();
		if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Target->GetRootComponent()))
		{
			FVector Surface;
			if (Primitive->GetClosestPointOnCollision(Owner->GetActorLocation(), Surface) > 0.0f)
				Closest = Surface;
		}
		const float Distance = FVector::DistSquared(Owner->GetActorLocation(), Closest);
		// Blocked packages remain discoverable for their explanatory prompt,
		// but must not hide a usable pickup or delivery target.
		FVector Center, Extent;
		Target->GetActorBounds(true, Center, Extent);
		const float Miss = Camera ? DeliveryInteractionGeometry::AimMissDistance(
			Camera->GetComponentLocation(), Camera->GetForwardVector().GetSafeNormal(),
			Center + FVector::UpVector * (Extent.Z * 0.5f)) : -1.0f;
		const float Allowance = 35.0f + FMath::Min(Extent.Size(), 25.0f)
			+ (FocusedPickup.Get() == Candidate ? 15.0f : 0.0f);
		const bool bAimed = Miss >= 0.0f && Miss <= Allowance;
		// Aim only ranks reachable targets. Never reject a target for missing the crosshair.
		const bool bBetterRank = bAimed != bBestAimed ? bAimed
			: (bAimed && Miss != BestMiss ? Miss < BestMiss : Distance < BestDistance);
		if (!Best || (bUsable && !bBestUsable) || (bUsable == bBestUsable && bBetterRank))
		{
			Best = Candidate;
			BestDistance = Distance;
			bBestUsable = bUsable;
			bBestAimed = bAimed;
			BestMiss = Miss;
		}
	}
	return Best;
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
