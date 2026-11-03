// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "DeliveryGameState.generated.h"

class UDeliveryPhoneCallQueueComponent;
class UDeliveryTaskManagerComponent;

/**
 * 保存全局任务状态和电话队列，所有玩家共享
 * 任务清单在游戏状态蓝图中配置，个人追踪目标和奖励分别由玩家组件管理
 */
UCLASS()

class DELIVERY_API ADeliveryGameState : public AGameStateBase
{
	GENERATED_BODY()

  public:
	ADeliveryGameState();

	// 全局组件查询
	UDeliveryTaskManagerComponent* GetTaskManager() const
	{
		return TaskManager;
	}

	UDeliveryPhoneCallQueueComponent* GetPhoneCallQueue() const
	{
		return PhoneCallQueue;
	}

  protected:
	// 共享任务与电话组件
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Task")
	TObjectPtr<UDeliveryTaskManagerComponent> TaskManager;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Task")
	TObjectPtr<UDeliveryPhoneCallQueueComponent> PhoneCallQueue;
};
