// Copyright Epic Games, Inc. All Rights Reserved.

#include "Interaction/DeliveryInteractableComponent.h"
#include "GameFramework/Pawn.h"
#include "Components/PrimitiveComponent.h"

TArray<TWeakObjectPtr<UDeliveryInteractableComponent>> UDeliveryInteractableComponent::Registry;

UDeliveryInteractableComponent::UDeliveryInteractableComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
}

void UDeliveryInteractableComponent::BeginPlay()
{
	Super::BeginPlay();
	Registry.AddUnique(this);
}

void UDeliveryInteractableComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Registry.Remove(this);
	Super::EndPlay(EndPlayReason);
}

bool UDeliveryInteractableComponent::CanInteract(const APawn* Interactor) const
{
	if (!bInteractEnabled || !Interactor || !GetOwner())
	{
		return false;
	}
	if (Interactor == GetOwner())
	{
		return false;
	}
	FVector InteractionPoint = GetOwner()->GetActorLocation();
	FVector InteractorPoint = Interactor->GetActorLocation();
	// E items on the floor are reached at their collision surface, not their pivot.
	// Measure the actual gap between the item and the player's collision surface.
	// Measuring from the pawn origin (the pelvis) makes a floor item appear farther
	// than one metre away even when it is directly beside the player's feet.
	if (InteractionKey == EDeliveryInteractionKey::PickupE)
	{
		if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent()))
		{
			FVector Closest;
			if (Primitive->GetClosestPointOnCollision(Interactor->GetActorLocation(), Closest) > 0.0f)
			{
				InteractionPoint = Closest;
			}
		}
		if (const UPrimitiveComponent* InteractorPrimitive = Cast<UPrimitiveComponent>(Interactor->GetRootComponent()))
		{
			FVector Closest;
			if (InteractorPrimitive->GetClosestPointOnCollision(InteractionPoint, Closest) > 0.0f)
			{
				InteractorPoint = Closest;
			}
		}
	}
	const float DistSq = FVector::DistSquared(InteractorPoint, InteractionPoint);
	return DistSq <= FMath::Square(InteractRadius);
}

void UDeliveryInteractableComponent::Execute(APawn* Interactor)
{
	OnInteract.Broadcast(Interactor);
}

FVector UDeliveryInteractableComponent::GetPromptLocation() const
{
	const AActor* Owner = GetOwner();
	return Owner ? Owner->GetActorLocation() + PromptOffset : PromptOffset;
}

UDeliveryInteractableComponent* UDeliveryInteractableComponent::FindBest(
	const APawn* Seeker, EDeliveryInteractionKey Key)
{
	if (!Seeker)
	{
		return nullptr;
	}

	const UWorld* World = Seeker->GetWorld();
	const FVector From = Seeker->GetActorLocation();

	UDeliveryInteractableComponent* Best = nullptr;
	float BestDistSq = TNumericLimits<float>::Max();

	for (int32 Index = Registry.Num() - 1; Index >= 0; --Index)
	{
		UDeliveryInteractableComponent* Candidate = Registry[Index].Get();
		if (!Candidate)
		{
			Registry.RemoveAtSwap(Index);
			continue;
		}
		// 同一个进程里可能同时开着编辑器世界和 PIE 世界，别把别的世界的东西提示出来。
		if (Candidate->GetWorld() != World || Candidate->InteractionKey != Key
			|| !Candidate->CanInteract(Seeker))
		{
			continue;
		}

		const float DistSq = FVector::DistSquared(From, Candidate->GetOwner()->GetActorLocation());
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = Candidate;
		}
	}

	return Best;
}

UDeliveryInteractableComponent* UDeliveryInteractableComponent::FindOn(const AActor* Actor)
{
	return Actor ? Actor->FindComponentByClass<UDeliveryInteractableComponent>() : nullptr;
}
