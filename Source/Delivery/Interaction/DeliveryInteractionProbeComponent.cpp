// Copyright Epic Games, Inc. All Rights Reserved.

#include "Interaction/DeliveryInteractionProbeComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "DeliveryCharacter.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Interaction/DeliveryInteractionGeometry.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Task/DeliveryItemComponent.h"

UDeliveryInteractionProbeComponent::UDeliveryInteractionProbeComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// 小物体瞄准变化较快；20Hz 让 E 提示和按键目标及时跟上准星。
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
	const FVector AimDirection = Camera->GetForwardVector().GetSafeNormal();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DeliveryPickupAim), false, Owner);
	if (Character->GetInventoryComponent()) Params.AddIgnoredActor(Character->GetInventoryComponent()->GetHeldItem());
	TArray<UDeliveryInteractableComponent*> Candidates;
	UDeliveryInteractableComponent::GetReachablePickupCandidates(Owner, Candidates);
	UDeliveryInteractableComponent* Best = nullptr;
	float BestMiss = TNumericLimits<float>::Max();
	for (UDeliveryInteractableComponent* Candidate : Candidates)
	{
		AActor* Target = Candidate->GetOwner();
		if (!Character->CanUsePickupTarget(Target, false)) continue;

		FVector BoundsCenter, BoundsExtent;
		Target->GetActorBounds(true, BoundsCenter, BoundsExtent);
		// 准星不必刚好压中小模型；尺寸越大的物体，允许的横向偏差略大。
		// 长按时给原目标一点额外余量，避免 0.5 秒内因轻微相机晃动而取消。
		const FVector AimPoint = BoundsCenter + FVector::UpVector * (BoundsExtent.Z * 0.5f);
		const float Miss = DeliveryInteractionGeometry::AimMissDistance(Start, AimDirection, AimPoint);
		const float Allowance = 35.0f + FMath::Min(BoundsExtent.Size(), 25.0f)
			+ (FocusedPickup.Get() == Candidate ? 15.0f : 0.0f);
		if (Miss < 0.0f || Miss > Allowance || Miss >= BestMiss) continue;

		// 容错只扩大瞄准范围，不允许隔着墙拾取；从镜头射到物体上半部，
		// 避免射向地面上的 Actor 原点时先打中地板。
		FHitResult Hit;
		const bool bBlocked = World->LineTraceSingleByChannel(
			Hit, Start, AimPoint, ECC_Visibility, Params);
		if (bBlocked && Hit.GetActor() != Target) continue;
		Best = Candidate;
		BestMiss = Miss;
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
