// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "DeliveryPackageItem.generated.h"

class UDeliveryItemComponent;

/** Single-hand physical package. It can occupy only the hand slot and owns task identity. */
UCLASS(Blueprintable)
class DELIVERY_API ADeliveryPackageItem : public ADeliveryHandheldItem
{
	GENERATED_BODY()

public:
	ADeliveryPackageItem();

	UFUNCTION(BlueprintPure, Category="Delivery")
	UDeliveryItemComponent* GetDeliveryItemComponent() const { return DeliveryItemComponent; }

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UDeliveryItemComponent> DeliveryItemComponent;
};

/** Code-built package for the first end-to-end task test. */
UCLASS(Blueprintable)
class DELIVERY_API ADeliveryTestPackage : public ADeliveryPackageItem
{
	GENERATED_BODY()

public:
	ADeliveryTestPackage();
	virtual void BeginPlay() override;
};
