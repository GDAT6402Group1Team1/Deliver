// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "DeliveryGameMode.generated.h"

/** 配置默认游戏对象，角色和控制器类型由游戏模式蓝图指定 */
UCLASS(abstract)

class ADeliveryGameMode : public AGameModeBase
{
	GENERATED_BODY()

  public:
	// 地图加载时调用
	ADeliveryGameMode();
	virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;
	virtual bool PlayerCanRestart_Implementation(APlayerController* Player) override;
};
