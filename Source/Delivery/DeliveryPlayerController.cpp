// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryPlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputMappingContext.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryPlayerController::ADeliveryPlayerController()
{
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> DefaultIMC(TEXT("/Game/Input/IMC_Default"));
	if (DefaultIMC.Succeeded())
	{
		DefaultMappingContexts.Add(DefaultIMC.Object);
	}

	static ConstructorHelpers::FObjectFinder<UInputMappingContext> MouseLookIMC(TEXT("/Game/Input/IMC_MouseLook"));
	if (MouseLookIMC.Succeeded())
	{
		MobileExcludedMappingContexts.Add(MouseLookIMC.Object);
	}
}

void ADeliveryPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (!IsLocalPlayerController())
	{
		return;
	}

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
		{
			Subsystem->AddMappingContext(CurrentContext, 0);
		}

		for (UInputMappingContext* CurrentContext : MobileExcludedMappingContexts)
		{
			Subsystem->AddMappingContext(CurrentContext, 0);
		}
	}
}
