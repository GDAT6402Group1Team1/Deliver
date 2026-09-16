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

const FDeliveryTimeGrade* UDeliveryTaskDefinition::FindTimeGrade(float RemainingSeconds) const
{
	if (TimeGrades.Num() == 0)
	{
		return nullptr;
	}

	// 配置顺序即优先级：剩余时间从多到少，第一条够得着的就是评价结果。
	// 不做排序，配错顺序应该在保存资产时被校验挡住，而不是被代码悄悄纠正。
	for (const FDeliveryTimeGrade& Grade : TimeGrades)
	{
		if (RemainingSeconds >= Grade.RemainingSeconds)
		{
			return &Grade;
		}
	}

	// 超时超过了最后一档。最后一档就是配置里最严厉的那一级，继续按它算，
	// 不额外发明一个"更惨"的倍率——否则策划在表里看不到这个数。
	return &TimeGrades.Last();
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

	// FindTimeGrade 取第一条够得着的档位，顺序配反的话后面的档永远命中不到
	for (int32 Index = 1; Index < TimeGrades.Num(); ++Index)
	{
		if (TimeGrades[Index].RemainingSeconds > TimeGrades[Index - 1].RemainingSeconds)
		{
			Context.AddError(FText::Format(
				LOCTEXT("GradesOutOfOrder", "TimeGrades 第 {0} 项的 RemainingSeconds 比上一项大。档位必须按剩余时间从多到少排，否则靠后的档永远命中不到。"),
				FText::AsNumber(Index)));
			Result = EDataValidationResult::Invalid;
		}
	}

	if (TimeGrades.Num() == 0)
	{
		Context.AddWarning(LOCTEXT("NoTimeGrades", "TimeGrades 为空，无论多快送到都按 1 倍结算。"));
	}
	// 第一档的门槛比限时还高的话，那一档永远够不着——剩余时间不可能超过限时
	else if (TimeGrades[0].RemainingSeconds > TimeLimitSeconds)
	{
		Context.AddWarning(FText::Format(
			LOCTEXT("TopGradeUnreachable",
				"第一档要求剩余 {0} 秒，但总限时只有 {1} 秒，这一档永远命中不到。"),
			FText::AsNumber(TimeGrades[0].RemainingSeconds),
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
