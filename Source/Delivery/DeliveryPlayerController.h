// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "DeliveryPlayerController.generated.h"

class UInputMappingContext;

/**
 * PlayerController that only installs Enhanced Input mapping contexts.
 */
UCLASS()
class ADeliveryPlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	ADeliveryPlayerController();
	
protected:

	/** Input Mapping Contexts always applied */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	TArray<TObjectPtr<UInputMappingContext>> DefaultMappingContexts;

	/** Extra mapping contexts for mouse look, skipped on touch devices */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	TArray<TObjectPtr<UInputMappingContext>> MobileExcludedMappingContexts;

	virtual void SetupInputComponent() override;
};
