// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryTaskDefinition.h"

#include "Task/DeliveryTaskUnlockCondition.h"

bool UDeliveryTaskDefinition::AreUnlockConditionsMet(const UDeliveryTaskManagerComponent* Manager) const
{
	for (const UDeliveryTaskUnlockCondition* Condition : UnlockConditions)
	{
		// 空槽位当成没配这条，不要让美术漏填一行就把整个任务锁死
		if (Condition && !Condition->IsSatisfied(Manager))
		{
			return false;
		}
	}

	return true;
}

EDeliveryTaskUrgency UDeliveryTaskDefinition::GetUrgency(float RemainingSeconds) const
{
	if (RemainingSeconds <= RedRemainingSeconds)
	{
		return EDeliveryTaskUrgency::Red;
	}

	if (RemainingSeconds <= YellowRemainingSeconds)
	{
		return EDeliveryTaskUrgency::Yellow;
	}

	return EDeliveryTaskUrgency::Green;
}

const FDeliveryTimeGrade* UDeliveryTaskDefinition::FindTimeGrade(float ElapsedSeconds) const
{
	// 配置顺序即优先级：从快到慢，第一条容得下这次用时的就是评价结果。
	// 不做排序，配错顺序应该在配置阶段被发现，而不是被代码悄悄纠正。
	for (const FDeliveryTimeGrade& Grade : TimeGrades)
	{
		if (ElapsedSeconds <= Grade.WithinSeconds)
		{
			return &Grade;
		}
	}

	return nullptr;
}
