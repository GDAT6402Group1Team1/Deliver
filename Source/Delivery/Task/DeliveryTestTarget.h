// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeliveryTestTarget.generated.h"

class UDeliveryInteractableComponent;
class UDeliveryTargetComponent;
class UStaticMeshComponent;

/** Simple visible target used to validate the full package loop in TestForCharacter. */
UCLASS(Blueprintable)
class DELIVERY_API ADeliveryTestTarget : public AActor
{
	GENERATED_BODY()

public:
	ADeliveryTestTarget();

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UStaticMeshComponent> TargetMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UDeliveryInteractableComponent> InteractableComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UDeliveryTargetComponent> DeliveryTargetComponent;
};
