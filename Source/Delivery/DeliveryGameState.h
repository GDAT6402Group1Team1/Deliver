// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "DeliveryGameState.generated.h"

class UDeliveryPhoneCallQueueComponent;
class UDeliveryTaskManagerComponent;

/**
 * 承载全局共享状态的 GameState。
 * 任务状态和电话队列挂在这里，而不是挂在某个玩家身上：多人模式下解锁、来电、计时、完成
 * 对所有人是同一份数据，只有"追踪哪个任务"和奖励结算是各人各自的。
 *
 * 关卡里的任务清单在 GameState 蓝图上配（TaskManager 的 TaskDefinitions）。
 */
UCLASS()
class DELIVERY_API ADeliveryGameState : public AGameStateBase
{
	GENERATED_BODY()

public:

	ADeliveryGameState();

	UDeliveryTaskManagerComponent* GetTaskManager() const { return TaskManager; }
	UDeliveryPhoneCallQueueComponent* GetPhoneCallQueue() const { return PhoneCallQueue; }

protected:

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Task")
	TObjectPtr<UDeliveryTaskManagerComponent> TaskManager;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Task")
	TObjectPtr<UDeliveryPhoneCallQueueComponent> PhoneCallQueue;
};
