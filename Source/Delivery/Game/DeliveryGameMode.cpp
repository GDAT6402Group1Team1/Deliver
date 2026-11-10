// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/DeliveryGameMode.h"
#include "Game/DeliveryStartupFlow.h"
#include "UI/DeliveryHUD.h"
#include "Character/DeliveryCharacter.h"
#include "Game/DeliveryGameState.h"
#include "Player/DeliveryPlayerController.h"
#include "Player/DeliverPlayerState.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryGameMode::ADeliveryGameMode()
{
	// 创建 Pawn 和 PlayerController
	DefaultPawnClass = ADeliveryCharacter::StaticClass(); // 兜底从C++创建
	PlayerControllerClass = ADeliveryPlayerController::StaticClass();
	HUDClass = ADeliveryHUD::StaticClass();
	PlayerStateClass = ADeliverPlayerState::StaticClass();

	// 任务状态和电话队列挂在 GameState 上；关卡的任务清单要在 GameState 蓝图里配，
	// 所以实际使用时应在 BP_DeliverGameMode 里把这一项换成对应的蓝图子类
	GameStateClass = ADeliveryGameState::StaticClass();

	static ConstructorHelpers::FClassFinder<APawn> PawnBP(
	    TEXT("/Game/Blueprint/Character/Deliverman/BP_DeliveryMan")); // 正常从蓝图创建
	if (PawnBP.Succeeded())
	{
		DefaultPawnClass = PawnBP.Class;
	}

	static ConstructorHelpers::FClassFinder<APlayerController> ControllerBP(
	    TEXT("/Game/Blueprint/PlayerController/BP_DeliveryManPC"));
	if (ControllerBP.Succeeded())
	{
		PlayerControllerClass = ControllerBP.Class;
	}
}

void ADeliveryGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	// 启动地图先加载地面，出生统一由启动服务在玩家就绪后执行
	if (ADeliveryStartupFlow::GetDeliveryStartupFlow(this)) return;
	Super::HandleStartingNewPlayer_Implementation(NewPlayer);
}

bool ADeliveryGameMode::PlayerCanRestart_Implementation(APlayerController* Player)
{
	if (const auto* Flow = ADeliveryStartupFlow::GetDeliveryStartupFlow(this))
		if (!Flow->CanSpawnPlayer(Player)) return false;
	return Super::PlayerCanRestart_Implementation(Player);
}
