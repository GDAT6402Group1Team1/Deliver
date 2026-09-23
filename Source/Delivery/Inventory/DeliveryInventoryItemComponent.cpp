// Copyright Epic Games, Inc. All Rights Reserved.

#include "Inventory/DeliveryInventoryItemComponent.h"
#include "DeliveryCharacter.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Net/UnrealNetwork.h"

UDeliveryInventoryItemComponent::UDeliveryInventoryItemComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
	DisplayName = NSLOCTEXT("DeliveryItems", "UnnamedItem", "道具");
}

void UDeliveryInventoryItemComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UDeliveryInventoryItemComponent, Durability);
}

void UDeliveryInventoryItemComponent::ApplyDurabilityDamage(float Amount)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || Amount <= 0.0f)
	{
		return;
	}
	Durability = FMath::Max(0.0f, Durability - Amount);
	OnRep_Durability();
	if (Durability <= 0.0f)
	{
		if (ADeliveryHandheldItem* Item = Cast<ADeliveryHandheldItem>(GetOwner()))
		{
			if (ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Item->GetOwner()))
			{
				if (UDeliveryInventoryComponent* Inventory = Character->GetInventoryComponent())
				{
					Inventory->RemoveItem(Item);
				}
			}
		}
		GetOwner()->Destroy();
	}
}

float UDeliveryInventoryItemComponent::GetDurabilityFraction() const
{
	return MaxDurability > KINDA_SMALL_NUMBER ? FMath::Clamp(Durability / MaxDurability, 0.0f, 1.0f) : 0.0f;
}

void UDeliveryInventoryItemComponent::OnRep_Durability()
{
	Durability = FMath::Clamp(Durability, 0.0f, FMath::Max(0.0f, MaxDurability));
}
