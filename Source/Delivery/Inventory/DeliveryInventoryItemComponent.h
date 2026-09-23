// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryInventoryItemComponent.generated.h"

class UTexture2D;

UENUM(BlueprintType)
enum class EDeliveryInventoryItemType : uint8
{
	DeliveryItem UMETA(DisplayName="Delivery Item"),
	BackpackItem UMETA(DisplayName="Backpack Item")
};

/** Metadata shared by every single-hand item. World physics stays on the owning actor. */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryInventoryItemComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UDeliveryInventoryItemComponent();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Item")
	EDeliveryInventoryItemType ItemType = EDeliveryInventoryItemType::BackpackItem;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Item")
	FText DisplayName;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Item")
	TObjectPtr<UTexture2D> Icon;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Item|Durability", meta=(ClampMin="0.0"))
	float MaxDurability = 100.0f;

	UPROPERTY(ReplicatedUsing=OnRep_Durability, EditInstanceOnly, BlueprintReadOnly,
		Category="Item|Durability", meta=(ClampMin="0.0"))
	float Durability = 100.0f;

	UFUNCTION(BlueprintCallable, Category="Item|Durability", BlueprintAuthorityOnly)
	void ApplyDurabilityDamage(float Amount);

	UFUNCTION(BlueprintPure, Category="Item|Durability")
	float GetDurabilityFraction() const;

	UFUNCTION(BlueprintPure, Category="Item")
	bool CanEnterBackpack() const { return ItemType == EDeliveryInventoryItemType::BackpackItem; }

private:
	UFUNCTION()
	void OnRep_Durability();
};
