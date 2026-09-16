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
	const UWorld* World = Manager ? Manager->GetWorld() : nullptr;

	// 用关卡时间而不是服务器同步时间：解锁判断只在服务器跑，这里要的就是"这局开始多久了"
	return World && World->GetTimeSeconds() >= DelaySeconds;
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
