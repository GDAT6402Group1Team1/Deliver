// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliverySettlementWidget.h"

#include "Components/TextBlock.h"
#include "DeliverySettlementComponent.h"
#include "Data/DeliveryTaskDefinition.h"
#include "GameFramework/PlayerController.h"

namespace
{
	/** 倍率显示成两位小数。整数倍率打成 `×1` 看着像没算，`×1.00` 才读得出"没有加成"。 */
	FText FormatMultiplier(const FText& Format, float Value)
	{
		return FText::Format(Format, FText::AsNumber(Value,
			&FNumberFormattingOptions().SetMinimumFractionalDigits(2)
				.SetMaximumFractionalDigits(2)));
	}
}

void UDeliverySettlementWidget::ApplyReward(UDeliveryTaskDefinition* InTask,
	const FDeliveryRewardBreakdown& InReward)
{
	Task = InTask;
	CachedReward = InReward;

	if (TaskNameText)
	{
		TaskNameText->SetText(Task ? Task->DisplayName : FText::GetEmpty());
	}
	if (FinalRewardText)
	{
		FinalRewardText->SetText(FText::Format(CurrencyFormat,
			FText::AsNumber(CachedReward.FinalReward)));
	}
	if (BaseRewardText)
	{
		BaseRewardText->SetText(FText::Format(CurrencyFormat,
			FText::AsNumber(CachedReward.BaseReward)));
	}
	if (TimeGradeText)
	{
		// 超时时 `TimeGradeName` 是空的。**不能就这么留空**——玩家看到的是
		// "评价那一栏没了"，像 UI 出了 bug，而不是"我迟到了"。
		TimeGradeText->SetText(CachedReward.TimeGradeName.IsEmpty()
			? OverdueGradeText : CachedReward.TimeGradeName);
	}
	if (TimeMultiplierText)
	{
		TimeMultiplierText->SetText(FormatMultiplier(MultiplierFormat, CachedReward.TimeMultiplier));
	}
	if (SpecialMultiplierText)
	{
		SpecialMultiplierText->SetText(FormatMultiplier(MultiplierFormat, CachedReward.SpecialMultiplier));
	}
	if (ElapsedText)
	{
		ElapsedText->SetText(FormatElapsed(CachedReward.ElapsedSeconds));
	}

	OnSettlementShown(CachedReward);
}

FText UDeliverySettlementWidget::FormatElapsed(float Seconds) const
{
	const int32 Total = FMath::Max(0, FMath::FloorToInt(Seconds));
	// 秒**必须补零**：`2:5` 会被读成两分五秒还是两分五十秒全看人，`2:05` 没有歧义。
	return FText::FromString(FString::Printf(TEXT("%d:%02d"), Total / 60, Total % 60));
}

void UDeliverySettlementWidget::RequestClose()
{
	// 关闭走组件，**不自己 RemoveFromParent**。
	// 加视口和登记面板是组件在同一处做的，移除也必须回到那一处，
	// 否则面板没了、光标还开着——那正是 CLAUDE.md 记的那种失同步。
	if (const APlayerController* Controller = GetOwningPlayer())
	{
		if (UDeliverySettlementComponent* Component =
			Controller->FindComponentByClass<UDeliverySettlementComponent>())
		{
			Component->HideSettlement();
		}
	}
}
