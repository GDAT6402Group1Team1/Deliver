// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryItemComponent.h"

#include "Task/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"

UDeliveryItemComponent::UDeliveryItemComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

bool UDeliveryItemComponent::CanBeAcquired() const
{
	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);

	return Manager && Manager->CanAcquireItem(OwningTask);
}

bool UDeliveryItemComponent::NotifyAcquired(APlayerState* Player)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return false;
	}

	UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);

	return Manager && Manager->TryAcquireItem(OwningTask, Player);
}
