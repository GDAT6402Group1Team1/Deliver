// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryInventoryComponent.generated.h"

class ADeliveryCharacter;
class ADeliveryHandheldItem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDeliveryInventoryChanged);

/** Server-authoritative five-slot hotbar. Slot 0 is the hand; slots 1-4 are the backpack. */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UDeliveryInventoryComponent();
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	static constexpr int32 SlotCount = 5;
	static constexpr int32 FirstBackpackSlot = 1;

	UPROPERTY(BlueprintAssignable, Category="Inventory")
	FDeliveryInventoryChanged OnInventoryChanged;

	UFUNCTION(BlueprintPure, Category="Inventory")
	ADeliveryHandheldItem* GetItemInSlot(int32 SlotIndex) const;

	UFUNCTION(BlueprintPure, Category="Inventory")
	ADeliveryHandheldItem* GetHeldItem() const { return GetItemInSlot(0); }

	UFUNCTION(BlueprintPure, Category="Inventory")
	bool HasHeldItem() const { return GetHeldItem() != nullptr; }

	UFUNCTION(BlueprintPure, Category="Inventory")
	bool HasBackpackSpace() const;

	UFUNCTION(BlueprintPure, Category="Inventory|UI")
	float GetFullFeedbackAlpha() const;

	/** Server-side pickup entry used by item interaction. */
	bool TryPickup(ADeliveryHandheldItem* Item);

	/** Local input entry. Index is zero-based (0 means key 1). */
	void RequestSlotAction(int32 SlotIndex);

	/** Drops only slot 1. Called by the stun transition on the authoritative server. */
	void DropHeldItem();
	void RemoveItem(ADeliveryHandheldItem* Item);
	/** Server-only removal without dropping; used immediately before a successful delivery destroys the actor. */
	bool ConsumeHeldItem(ADeliveryHandheldItem* ExpectedItem);

	void UseHeldItem();

protected:
	UPROPERTY(ReplicatedUsing=OnRep_Slots, BlueprintReadOnly, Category="Inventory")
	TArray<TObjectPtr<ADeliveryHandheldItem>> Slots;

	UPROPERTY(EditDefaultsOnly, Category="Inventory|Drop")
	float DropForwardDistance = 75.0f;

	UPROPERTY(EditDefaultsOnly, Category="Inventory|Drop")
	float DropUpOffset = 35.0f;

	UPROPERTY(EditDefaultsOnly, Category="Inventory|Drop")
	float DropForwardSpeed = 80.0f;

private:
	UFUNCTION(Server, Reliable)
	void ServerSlotAction(int32 SlotIndex);

	UFUNCTION(Client, Reliable)
	void ClientInventoryFull();

	UFUNCTION()
	void OnRep_Slots();

	void PerformSlotAction(int32 SlotIndex);
	void ApplyPresentation();
	void StoreItem(int32 SlotIndex, ADeliveryHandheldItem* Item);
	int32 FindEmptyBackpackSlot() const;
	bool CanMutateInventory() const;
	ADeliveryCharacter* GetCharacter() const;
	float FullFeedbackStartedAt = -1000.0f;
};
