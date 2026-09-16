// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryTaskDefinition.h"

#include "Task/DeliveryTaskUnlockCondition.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

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

#if WITH_EDITOR

#define LOCTEXT_NAMESPACE "DeliveryTaskDefinition"

EDataValidationResult UDeliveryTaskDefinition::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	if (TaskId.IsNone())
	{
		Context.AddError(LOCTEXT("EmptyTaskId", "TaskId 为空。日志、控制台命令和将来的存档都靠它定位任务。"));
		Result = EDataValidationResult::Invalid;
	}

	// 红是比黄更紧迫的阶段，剩余秒数必须更小；配反了颜色会直接从绿跳到红，黄永远出不来
	if (RedRemainingSeconds > YellowRemainingSeconds)
	{
		Context.AddError(LOCTEXT("ThresholdsInverted", "RedRemainingSeconds 必须小于 YellowRemainingSeconds（红色是剩余时间更少的阶段）。"));
		Result = EDataValidationResult::Invalid;
	}

	// 阈值大于等于总限时的话，任务一开始就是黄的或红的
	if (YellowRemainingSeconds >= TimeLimitSeconds)
	{
		Context.AddWarning(LOCTEXT("NoGreenPhase", "YellowRemainingSeconds 不小于 TimeLimitSeconds，任务一开始就是黄色，不会有绿色阶段。"));
	}

	// FindTimeGrade 取第一条容得下用时的档位，顺序配反的话后面的档永远命中不到
	for (int32 Index = 1; Index < TimeGrades.Num(); ++Index)
	{
		if (TimeGrades[Index].WithinSeconds < TimeGrades[Index - 1].WithinSeconds)
		{
			Context.AddError(FText::Format(
				LOCTEXT("GradesOutOfOrder", "TimeGrades 第 {0} 项的 WithinSeconds 比上一项小。档位必须按用时从小到大排，否则靠后的档永远命中不到。"),
				FText::AsNumber(Index)));
			Result = EDataValidationResult::Invalid;
		}
	}

	// 一档都不配的话，再快送到也会走 OvertimeMultiplier，看起来像"怎么都是超时"
	if (TimeGrades.Num() == 0)
	{
		Context.AddWarning(LOCTEXT("NoTimeGrades", "TimeGrades 为空，任何用时都会按 OvertimeMultiplier 结算。"));
	}
	// OvertimeMultiplier 的真实含义是"没命中任何档位"，不是"超过了限时"。
	// 最后一档没盖到限时的话，玩家还在限时之内就会吃到超时惩罚，很难查。
	else if (TimeGrades.Last().WithinSeconds < TimeLimitSeconds)
	{
		Context.AddWarning(FText::Format(
			LOCTEXT("GradeGapBeforeLimit",
				"最后一档只覆盖到 {0} 秒，但限时是 {1} 秒。用时落在这两个数之间时并没有超时，"
				"却会因为没命中任何档位而按 OvertimeMultiplier 结算。把最后一档的 WithinSeconds 提到限时即可。"),
			FText::AsNumber(TimeGrades.Last().WithinSeconds),
			FText::AsNumber(TimeLimitSeconds)));
	}

	for (const FDeliverySpecialEventRule& Rule : SpecialEventRules)
	{
		if (!Rule.EventTag.IsValid())
		{
			Context.AddError(LOCTEXT("EmptyEventTag", "SpecialEventRules 里有一条没填 EventTag，这条规则永远不会命中。"));
			Result = EDataValidationResult::Invalid;
		}
	}

	return Result;
}

#undef LOCTEXT_NAMESPACE

#endif // WITH_EDITOR
