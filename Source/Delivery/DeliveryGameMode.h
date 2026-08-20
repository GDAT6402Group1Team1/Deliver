// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "DeliveryGameMode.generated.h"

/**
 * Minimal GameMode. Default pawn / controller classes are assigned in the GameMode Blueprint.
 */
UCLASS(abstract)
class ADeliveryGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:

	ADeliveryGameMode();
};
