// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryTaskUnlockCondition.h"

#include "Task/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"

bool UDeliveryTaskUnlockCondition::IsSatisfied_Implementation(const UDeliveryTaskManagerComponent* /*Manager*/) const
{
	return true;
}

bool UDeliveryTaskUnlockCondition_TasksCompleted::IsSatisfied_Implementation(const UDeliveryTaskManagerComponent* Manager) const
{
	if (!Manager)
	{
		return false;
	}

	for (const UDeliveryTaskDefinition* Required : RequiredTasks)
	{
		if (Required && Manager->GetTaskStatus(Required) != EDeliveryTaskStatus::Completed)
		{
			return false;
		}
	}

	return true;
}
