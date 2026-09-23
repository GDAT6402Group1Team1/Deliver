// Copyright Epic Games, Inc. All Rights Reserved.

#include "Inventory/DeliveryInventoryComponent.h"
#include "AbilitySystemComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DeliveryCharacter.h"
#include "GAS/DeliverGameplayTags.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Net/UnrealNetwork.h"

UDeliveryInventoryComponent::UDeliveryInventoryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
	Slots.SetNum(SlotCount);
}

void UDeliveryInventoryComponent::BeginPlay()
{
	Super::BeginPlay();
	Slots.SetNum(SlotCount);
	ApplyPresentation();
}

void UDeliveryInventoryComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UDeliveryInventoryComponent, Slots);
}

ADeliveryCharacter* UDeliveryInventoryComponent::GetCharacter() const
{
	return Cast<ADeliveryCharacter>(GetOwner());
}

ADeliveryHandheldItem* UDeliveryInventoryComponent::GetItemInSlot(int32 SlotIndex) const
{
	return Slots.IsValidIndex(SlotIndex) ? Slots[SlotIndex].Get() : nullptr;
}

int32 UDeliveryInventoryComponent::FindEmptyBackpackSlot() const
{
	for (int32 Index = FirstBackpackSlot; Index < SlotCount; ++Index)
	{
		if (!GetItemInSlot(Index)) return Index;
	}
	return INDEX_NONE;
}

bool UDeliveryInventoryComponent::HasBackpackSpace() const
{
	return FindEmptyBackpackSlot() != INDEX_NONE;
}

float UDeliveryInventoryComponent::GetFullFeedbackAlpha() const
{
	const UWorld* World = GetWorld();
	if (!World) return 0.0f;
	const float Age = World->GetTimeSeconds() - FullFeedbackStartedAt;
	return Age >= 0.0f && Age < 0.7f ? (1.0f - Age / 0.7f) * (0.6f + 0.4f * FMath::Abs(FMath::Sin(Age * 18.0f))) : 0.0f;
}

bool UDeliveryInventoryComponent::CanMutateInventory() const
{
	const ADeliveryCharacter* Character = GetCharacter();
	if (!Character) return false;
	if (const UDeliveryGrabComponent* Grab = Character->GetGrabComponent(); Grab && Grab->IsGrabbing()) return false;
	const UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent();
	return !ASC || !ASC->HasMatchingGameplayTag(TAG_State_Stunned);
}

bool UDeliveryInventoryComponent::TryPickup(ADeliveryHandheldItem* Item)
{
	ADeliveryCharacter* Character = GetCharacter();
	if (!Character || !Character->HasAuthority() || !IsValid(Item) || !CanMutateInventory()) return false;
	UDeliveryInventoryItemComponent* ItemData = Item->GetItemComponent();
	if (!ItemData || Item->GetOwner()) return false;

	const int32 EmptyBackpack = FindEmptyBackpackSlot();
	const bool bBackpackItem = ItemData->CanEnterBackpack();
	if (bBackpackItem && EmptyBackpack == INDEX_NONE)
	{
		ClientInventoryFull();
		return false;
	}

	if (ItemData->ItemType == EDeliveryInventoryItemType::DeliveryItem)
	{
		if (ADeliveryHandheldItem* OldHand = GetHeldItem())
		{
			if (OldHand->GetItemComponent() && OldHand->GetItemComponent()->CanEnterBackpack() && EmptyBackpack != INDEX_NONE)
			{
				StoreItem(EmptyBackpack, OldHand);
			}
			else
			{
				DropHeldItem();
			}
		}
		StoreItem(0, Item);
	}
	else if (!GetHeldItem())
	{
		StoreItem(0, Item);
	}
	else
	{
		StoreItem(EmptyBackpack, Item);
	}

	ApplyPresentation();
	OnInventoryChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
	return true;
}

void UDeliveryInventoryComponent::RequestSlotAction(int32 SlotIndex)
{
	if (!CanMutateInventory() || SlotIndex < 0 || SlotIndex >= SlotCount) return;
	if (GetOwner() && GetOwner()->HasAuthority()) PerformSlotAction(SlotIndex);
	else ServerSlotAction(SlotIndex);
}

void UDeliveryInventoryComponent::ServerSlotAction_Implementation(int32 SlotIndex)
{
	if (CanMutateInventory()) PerformSlotAction(SlotIndex);
}

void UDeliveryInventoryComponent::PerformSlotAction(int32 SlotIndex)
{
	if (SlotIndex < 0 || SlotIndex >= SlotCount) return;
	if (SlotIndex == 0)
	{
		DropHeldItem();
		return;
	}

	ADeliveryHandheldItem* Hand = GetHeldItem();
	ADeliveryHandheldItem* Target = GetItemInSlot(SlotIndex);
	if (Hand && Hand->GetItemComponent()
		&& Hand->GetItemComponent()->ItemType == EDeliveryInventoryItemType::DeliveryItem)
	{
		DropHeldItem();
		Slots[0] = Target;
		Slots[SlotIndex] = nullptr;
	}
	else
	{
		Slots[0] = Target;
		Slots[SlotIndex] = Hand;
	}
	ApplyPresentation();
	OnInventoryChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
}

void UDeliveryInventoryComponent::StoreItem(int32 SlotIndex, ADeliveryHandheldItem* Item)
{
	if (!Slots.IsValidIndex(SlotIndex) || !Item) return;
	Slots[SlotIndex] = Item;
	Item->SetOwner(GetOwner());
}

void UDeliveryInventoryComponent::DropHeldItem()
{
	ADeliveryCharacter* Character = GetCharacter();
	ADeliveryHandheldItem* Item = GetHeldItem();
	if (!Character || !Character->HasAuthority() || !Item) return;
	Slots[0] = nullptr;
	const FVector Forward = Character->GetActorForwardVector().GetSafeNormal2D();
	const FVector Origin = Character->GetActorLocation() + Forward * DropForwardDistance + FVector::UpVector * DropUpOffset;
	Item->DropFromInventory(Origin, Forward * DropForwardSpeed);
	ApplyPresentation();
	OnInventoryChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
}

void UDeliveryInventoryComponent::RemoveItem(ADeliveryHandheldItem* Item)
{
	if (!Item || !GetOwner() || !GetOwner()->HasAuthority()) return;
	bool bRemoved = false;
	for (TObjectPtr<ADeliveryHandheldItem>& Slot : Slots)
	{
		if (Slot == Item)
		{
			Slot = nullptr;
			bRemoved = true;
		}
	}
	if (bRemoved)
	{
		ApplyPresentation();
		OnInventoryChanged.Broadcast();
		GetOwner()->ForceNetUpdate();
	}
}

void UDeliveryInventoryComponent::UseHeldItem()
{
	if (ADeliveryHandheldItem* Item = GetHeldItem()) Item->Use(GetCharacter());
}

void UDeliveryInventoryComponent::ClientInventoryFull_Implementation()
{
	FullFeedbackStartedAt = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	OnInventoryChanged.Broadcast();
}

void UDeliveryInventoryComponent::OnRep_Slots()
{
	Slots.SetNum(SlotCount);
	ApplyPresentation();
	OnInventoryChanged.Broadcast();
}

void UDeliveryInventoryComponent::ApplyPresentation()
{
	ADeliveryCharacter* Character = GetCharacter();
	if (!Character) return;
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		if (ADeliveryHandheldItem* Item = Slots[Index])
		{
			Item->SetInventoryPresentation(Index == 0, Index != 0, Character->GetHeldItemAnchor());
		}
	}
}
