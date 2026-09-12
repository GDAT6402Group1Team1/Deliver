// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryGameState.h"

#include "Task/DeliveryPhoneCallQueueComponent.h"
#include "Task/DeliveryTaskManagerComponent.h"

ADeliveryGameState::ADeliveryGameState()
{
	TaskManager = CreateDefaultSubobject<UDeliveryTaskManagerComponent>(TEXT("TaskManager"));
	PhoneCallQueue = CreateDefaultSubobject<UDeliveryPhoneCallQueueComponent>(TEXT("PhoneCallQueue"));
}
