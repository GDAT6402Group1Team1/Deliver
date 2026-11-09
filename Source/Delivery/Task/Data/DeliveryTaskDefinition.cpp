// Copyright Epic Games, Inc. All Rights Reserved.

#include "Task/Data/DeliveryTaskDefinition.h"

#include "Task/Data/DeliveryTaskUnlockCondition.h"

#if WITH_EDITOR
#include "Audio/DeliveryDialogueVoiceComponent.h"
#include "Misc/DataValidation.h"
#endif

// 逐项检查任务的解锁条件
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

// 根据当前任务时间计算紧急程度
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

// 根据剩余时间选择奖励档位
const FDeliveryTimeGrade* UDeliveryTaskDefinition::FindTimeGrade(float RemainingSeconds) const
{
	if (TimeGrades.Num() == 0)
	{
		return nullptr;
	}

	// 按配置顺序取第一个满足剩余时间要求的档位，顺序由资产校验检查
	for (const FDeliveryTimeGrade& Grade : TimeGrades)
	{
		if (RemainingSeconds >= Grade.RemainingSeconds)
		{
			return &Grade;
		}
	}

	// 低于所有时间档位时，仍使用最后一档的奖励倍率
	return &TimeGrades.Last();
}

#if WITH_EDITOR

#define LOCTEXT_NAMESPACE "DeliveryTaskDefinition"

// 在编辑器检查任务配置，提前发现缺少资源或无效数值
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
		Context.AddError(
		    LOCTEXT("ThresholdsInverted",
			        "RedRemainingSeconds 必须小于 YellowRemainingSeconds（红色是剩余时间更少的阶段）。"));
		Result = EDataValidationResult::Invalid;
	}

	// 阈值大于等于总限时的话，任务一开始就是黄的或红的
	if (YellowRemainingSeconds >= TimeLimitSeconds)
	{
		Context.AddWarning(
		    LOCTEXT("NoGreenPhase",
			        "YellowRemainingSeconds 不小于 TimeLimitSeconds，任务一开始就是黄色，不会有绿色阶段。"));
	}

	// FindTimeGrade 取第一条够得着的档位，顺序配反的话后面的档永远命中不到
	for (int32 Index = 1; Index < TimeGrades.Num(); ++Index)
	{
		if (TimeGrades[Index].RemainingSeconds > TimeGrades[Index - 1].RemainingSeconds)
		{
			Context.AddError(FText::Format(
			    LOCTEXT(
			        "GradesOutOfOrder",
			        "TimeGrades 第 {0} 项的 RemainingSeconds 比上一项大。档位必须按剩余时间从多到少排，否则靠后的档永远命中不到。"),
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
		    FText::AsNumber(TimeGrades[0].RemainingSeconds), FText::AsNumber(TimeLimitSeconds)));
	}

	for (const FDeliverySpecialEventRule& Rule : SpecialEventRules)
	{
		if (!Rule.EventTag.IsValid())
		{
			Context.AddError(
			    LOCTEXT("EmptyEventTag", "SpecialEventRules 里有一条没填 EventTag，这条规则永远不会命中。"));
			Result = EDataValidationResult::Invalid;
		}
	}

	// 通话时长由服务器计时、到点直接挂断，不等本机念完（见 FDeliveryPhoneCallContent 的注释）。
	// 所以 DurationSeconds 配得比台词短，玩家听到的就是"NPC 话没说完就被切断"——
	// 而那看着像对白写短了或者语音坏了，几乎不会让人想到是时长没配。
	// 只报警告不报错：故意切断一段长台词是合理的做法，这里只是把数摆出来
	auto ValidateCallDuration = [&Context](const FDeliveryPhoneCallContent& Call, const FText& FieldName) {
		if (Call.Dialogue.IsEmpty())
		{
			return;
		}

		const float NeedSeconds = UDeliveryDialogueVoiceComponent::EstimateSpeechSeconds(Call.Dialogue);
		if (Call.DurationSeconds >= NeedSeconds)
		{
			return;
		}

		// 把"念到哪儿被切"也报出来：直接给出截断位置比只报两个数好查得多
		const int32 SpokenLength =
		    UDeliveryDialogueVoiceComponent::SpokenGlyphCountWithin(Call.Dialogue, Call.DurationSeconds);
		const FString Spoken = Call.Dialogue.ToString().Left(SpokenLength);

		Context.AddWarning(FText::Format(
		    LOCTEXT("CallCutShort",
		            "{0} 的 DurationSeconds 只有 {1} 秒，而这段台词念完要 {2} 秒，"
		            "玩家听不到后面的内容（会在「{3}」处被挂断）。"),
		    FieldName, FText::AsNumber(Call.DurationSeconds),
		    FText::AsNumber(FMath::CeilToFloat(NeedSeconds * 10.f) / 10.f),
		    FText::FromString(Spoken.Right(24).ReplaceCharWithEscapedChar())));
	};

	ValidateCallDuration(UnlockCall, LOCTEXT("UnlockCallField", "UnlockCall"));
	ValidateCallDuration(OverdueCall, LOCTEXT("OverdueCallField", "OverdueCall"));

	return Result;
}

#undef LOCTEXT_NAMESPACE

#endif // 编辑器功能
