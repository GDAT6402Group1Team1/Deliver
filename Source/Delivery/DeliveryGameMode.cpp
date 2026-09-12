// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryGameMode.h"
#include "DeliveryCharacter.h"
#include "DeliveryGameState.h"
#include "DeliveryPlayerController.h"
#include "GAS/DeliverPlayerState.h"
#include "UObject/ConstructorHelpers.h"


ADeliveryGameMode::ADeliveryGameMode()
{
	// 创建 Pawn 和 PlayerController
	DefaultPawnClass = ADeliveryCharacter::StaticClass();																		// 兜底从C++创建
	PlayerControllerClass = ADeliveryPlayerController::StaticClass();
	PlayerStateClass = ADeliverPlayerState::StaticClass();

	// 任务状态和电话队列挂在 GameState 上。关卡的任务清单要在 GameState 蓝图里配，
	// 所以实际使用时应在 BP_DeliverGameMode 里把这一项换成对应的蓝图子类。
	GameStateClass = ADeliveryGameState::StaticClass();

	static ConstructorHelpers::FClassFinder<APawn> PawnBP(TEXT("/Game/Blueprint/Character/Deliverman/BP_DeliveryMan"));					// 正常从蓝图创建
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
