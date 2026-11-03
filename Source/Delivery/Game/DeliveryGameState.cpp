// 创建并持有全局任务管理与电话队列组件
// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/DeliveryGameState.h"

#include "Phone/DeliveryPhoneCallQueueComponent.h"
#include "Task/DeliveryTaskManagerComponent.h"

ADeliveryGameState::ADeliveryGameState()
{
	TaskManager = CreateDefaultSubobject<UDeliveryTaskManagerComponent>(TEXT("TaskManager"));
	PhoneCallQueue = CreateDefaultSubobject<UDeliveryPhoneCallQueueComponent>(TEXT("PhoneCallQueue"));
}
