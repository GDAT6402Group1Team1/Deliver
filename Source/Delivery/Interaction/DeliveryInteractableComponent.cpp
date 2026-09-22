// Copyright Epic Games, Inc. All Rights Reserved.

#include "Interaction/DeliveryInteractableComponent.h"
#include "GameFramework/Pawn.h"

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
	const float DistSq = FVector::DistSquared(Interactor->GetActorLocation(), GetOwner()->GetActorLocation());
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

UDeliveryInteractableComponent* UDeliveryInteractableComponent::FindBest(const APawn* Seeker)
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
		if (Candidate->GetWorld() != World || !Candidate->CanInteract(Seeker))
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
