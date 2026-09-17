// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryTaskUnlockCondition.h"

#include "Engine/World.h"
#include "Task/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"

bool UDeliveryTaskUnlockCondition::IsSatisfied_Implementation(const UDeliveryTaskManagerComponent* /*Manager*/) const
{
	return true;
}

bool UDeliveryTaskUnlockCondition_TimeSinceStart::IsSatisfied_Implementation(const UDeliveryTaskManagerComponent* Manager) const
{
	// 用"管理器 BeginPlay 以来过了多久"，不是世界时钟的绝对值——
	// 后者在 PIE 下起点不保证是 0，会让这个条件在开局当场就成立
	return Manager && Manager->GetTimeSinceLevelStart() >= DelaySeconds;
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
