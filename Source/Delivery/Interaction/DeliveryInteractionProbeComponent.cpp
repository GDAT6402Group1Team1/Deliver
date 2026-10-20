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
#include "Task/DeliveryPickupPointComponent.h"
#include "Vehicle/DeliveryMotorbike.h"

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
	const ADeliveryCharacter* OwnerCharacter = Cast<ADeliveryCharacter>(Owner);

	/**
	 * 本机操控，**或者**我是某辆本机操控的车的驾驶员。
	 *
	 * 骑车时控制器 Possess 的是车，这具身体的 `IsLocallyControlled()` 变成 false
	 * ——只判那一句的话上车之后 E 整个失灵，取货点和收件点都按不动。
	 * 判的是"车是不是本机的"而不是"我身上有没有车"：别人骑的车上也挂着一具身体，
	 * 那具不该在我这边探测。
	 */
	const ADeliveryMotorbike* RiddenBike = OwnerCharacter ? OwnerCharacter->GetCurrentVehicle() : nullptr;
	const bool bLocallyDriving = RiddenBike && RiddenBike->IsLocallyControlled();

	if (!Owner || (!Owner->IsLocallyControlled() && !bLocallyDriving))
	{
		FocusedGeneral = nullptr;
		FocusedPickup = nullptr;
		RestorePickupHighlight();
		return;
	}

	// An incapacitated pawn cannot interact. Clear both input targets and the old highlight.
	const ADeliveryCharacter* Character = OwnerCharacter;
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
				// 取货点和快递箱子各有一套拦截原因。"按了没反应"在游戏里完全无法自查，
				// 两边都必须把原因顶到提示浮窗上
				AActor* PickupOwner = Pickup->GetOwner();
				if (const UDeliveryPickupPointComponent* Point = UDeliveryPickupPointComponent::FindOn(PickupOwner))
				{
					const FText Reason = Point->GetBlockedReason();
					if (!Reason.IsEmpty()) Text = Reason;
				}
				else if (const UDeliveryItemComponent* Item = PickupOwner->FindComponentByClass<UDeliveryItemComponent>())
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
