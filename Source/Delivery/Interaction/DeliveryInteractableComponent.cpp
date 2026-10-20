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
	const float Distance = GetInteractionDistance(Interactor);

	return Distance >= 0.0f && Distance <= InteractRadius;
}

FVector UDeliveryInteractableComponent::GetInteractionPoint(const FVector& FromLocation) const
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return FromLocation;
	}

	// 锚点优先。挂在整栋楼上时 Actor 原点可能在楼心/地下，而大型网格用
	// "Use Complex Collision As Simple" 时连简单碰撞体都没有、下面那个查询也会失败，
	// 于是"箭头指门口、判定从楼心算"。锚点是唯一能让两者统一的办法
	if (const USceneComponent* Anchor = InteractAnchor.Get())
	{
		return Anchor->GetComponentLocation();
	}

	// E items on the floor are reached at their collision surface, not their pivot.
	// Measuring from the pawn origin (the pelvis) makes a floor item appear farther
	// than one metre away even when it is directly beside the player's feet.
	if (InteractionKey == EDeliveryInteractionKey::PickupE)
	{
		if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Owner->GetRootComponent()))
		{
			FVector Closest;
			if (Primitive->GetClosestPointOnCollision(FromLocation, Closest) > 0.0f)
			{
				return Closest;
			}
		}
	}

	return Owner->GetActorLocation();
}

float UDeliveryInteractableComponent::GetInteractionDistance(const APawn* Interactor) const
{
	if (!Interactor || !GetOwner())
	{
		return -1.0f;
	}

	const FVector InteractionPoint = GetInteractionPoint(Interactor->GetActorLocation());
	FVector InteractorPoint = Interactor->GetActorLocation();

	// 玩家这一侧也取碰撞表面：从胶囊原点（骨盆）算的话，脚边的东西也会显得一米开外
	if (InteractionKey == EDeliveryInteractionKey::PickupE)
	{
		if (const UPrimitiveComponent* InteractorPrimitive =
			Cast<UPrimitiveComponent>(Interactor->GetRootComponent()))
		{
			FVector Closest;
			if (InteractorPrimitive->GetClosestPointOnCollision(InteractionPoint, Closest) > 0.0f)
			{
				InteractorPoint = Closest;
			}
		}
	}

	return FVector::Dist(InteractorPoint, InteractionPoint);
}

void UDeliveryInteractableComponent::Execute(APawn* Interactor)
{
	OnInteract.Broadcast(Interactor);
}

FVector UDeliveryInteractableComponent::GetPromptLocation() const
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return PromptOffset;
	}

	// 浮窗也吊在锚点上方，不是 Actor 原点上方。
	// 否则会出现"提示飘在楼中间、能按的地方在门口"——玩家会走向提示那边
	const FVector Base = InteractAnchor.Get()
		? InteractAnchor->GetComponentLocation()
		: Owner->GetActorLocation();

	return Base + PromptOffset;
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

void UDeliveryInteractableComponent::GetReachablePickupCandidates(
	const APawn* Seeker, TArray<UDeliveryInteractableComponent*>& OutCandidates)
{
	OutCandidates.Reset();
	if (!Seeker) return;

	for (int32 Index = Registry.Num() - 1; Index >= 0; --Index)
	{
		UDeliveryInteractableComponent* Candidate = Registry[Index].Get();
		if (!Candidate)
		{
			Registry.RemoveAtSwap(Index);
			continue;
		}
		if (Candidate->GetWorld() == Seeker->GetWorld()
			&& Candidate->InteractionKey == EDeliveryInteractionKey::PickupE
			&& Candidate->CanInteract(Seeker))
		{
			OutCandidates.Add(Candidate);
		}
	}
}

UDeliveryInteractableComponent* UDeliveryInteractableComponent::FindOn(const AActor* Actor)
{
	return Actor ? Actor->FindComponentByClass<UDeliveryInteractableComponent>() : nullptr;
}
