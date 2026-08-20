// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryGameMode.h"
#include "DeliveryCharacter.h"
#include "DeliveryPlayerController.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryGameMode::ADeliveryGameMode()
{
	DefaultPawnClass = ADeliveryCharacter::StaticClass();
	PlayerControllerClass = ADeliveryPlayerController::StaticClass();

	static ConstructorHelpers::FClassFinder<APawn> PawnBP(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	if (PawnBP.Succeeded())
	{
		DefaultPawnClass = PawnBP.Class;
	}

	static ConstructorHelpers::FClassFinder<APlayerController> ControllerBP(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonPlayerController"));
	if (ControllerBP.Succeeded())
	{
		PlayerControllerClass = ControllerBP.Class;
	}
}
