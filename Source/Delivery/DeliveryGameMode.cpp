// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryGameMode.h"
#include "DeliveryCharacter.h"
#include "DeliveryPlayerController.h"
#include "GAS/DeliverPlayerState.h"
#include "UObject/ConstructorHelpers.h"


ADeliveryGameMode::ADeliveryGameMode()
{
	// 创建 Pawn 和 PlayerController
	DefaultPawnClass = ADeliveryCharacter::StaticClass();																		// 兜底从C++创建
	PlayerControllerClass = ADeliveryPlayerController::StaticClass();
	PlayerStateClass = ADeliverPlayerState::StaticClass();

	static ConstructorHelpers::FClassFinder<APawn> PawnBP(TEXT("/Game/Blueprint/Character/BP_DeliveryMan"));					// 正常从蓝图创建
	if (PawnBP.Succeeded())
	{
		DefaultPawnClass = PawnBP.Class;
	}

	static ConstructorHelpers::FClassFinder<APlayerController> ControllerBP(TEXT("/Game/Blueprint/PlayerController/BP_DeliveryManPC"));
	if (ControllerBP.Succeeded())
	{
		PlayerControllerClass = ControllerBP.Class;
	}
}
