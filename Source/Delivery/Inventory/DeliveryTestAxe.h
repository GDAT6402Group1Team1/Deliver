// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "DeliveryTestAxe.generated.h"

class UStaticMeshComponent;

/** Simple code-built axe used to exercise pickup/hotbar/drop before final art exists. */
UCLASS(Blueprintable)
class DELIVERY_API ADeliveryTestAxe : public ADeliveryHandheldItem
{
	GENERATED_BODY()

public:
	ADeliveryTestAxe();
	virtual void BeginPlay() override;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UStaticMeshComponent> AxeHead;
};
