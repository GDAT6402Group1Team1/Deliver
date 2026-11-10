// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliverySettlementWidget.h"

#include "Components/TextBlock.h"
#include "Components/AudioComponent.h"
#include "DeliverySettlementComponent.h"
#include "Data/DeliveryTaskDefinition.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

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

UDeliverySettlementWidget::UDeliverySettlementWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	static ConstructorHelpers::FObjectFinder<USoundBase> PopSound(TEXT("/Game/Sound/SFX/pop.pop"));
	RowRevealSound = PopSound.Object;
	FinalRevealSound = PopSound.Object;
}

void UDeliverySettlementWidget::ApplyReward(UDeliveryTaskDefinition* InTask,
	const FDeliveryRewardBreakdown& InReward)
{
	CancelReveal();
	const uint32 ThisGeneration = RevealGeneration;
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

	PrepareReveal();
	OnSettlementShown(CachedReward);

	// 蓝图回调可能关闭面板或再次填入另一单，旧显示请求不能继续排计时器
	if (ThisGeneration != RevealGeneration)
	{
		return;
	}
	if (RevealRows.IsEmpty())
	{
		OnRevealFinished.Broadcast(this);
		return;
	}
	GetWorld()->GetTimerManager().SetTimer(RevealTimer, this,
		&UDeliverySettlementWidget::RevealNextRow, FMath::Max(0.01f, FirstRowDelay), false);
}

void UDeliverySettlementWidget::PrepareReveal()
{
	if (!bRevealRows || !GetWorld() || IsDesignTime())
	{
		return;
	}

	// 只改变透明度，不折叠控件，避免逐行出现时布局跳动
	// 背景图上的固定标签保持显示，未摆放或刻意隐藏的可选明细不占等待时间
	for (UTextBlock* Text : {TimeGradeText.Get(), ElapsedText.Get(), BaseRewardText.Get(),
		TimeMultiplierText.Get(), SpecialMultiplierText.Get(), FinalRewardText.Get()})
	{
		// 使用控件配置，不能用依赖 Slate 已构建的 IsVisible 来判断初始状态
		if (Text && Text->GetVisibility() != ESlateVisibility::Hidden
			&& Text->GetVisibility() != ESlateVisibility::Collapsed && Text->GetRenderOpacity() > 0.f)
		{
			RevealRows.Add({Text, Text->GetRenderOpacity()});
			Text->SetRenderOpacity(0.f);
		}
	}
}

void UDeliverySettlementWidget::RevealNextRow()
{
	if (!RevealRows.IsValidIndex(NextRevealRow))
	{
		return;
	}

	const int32 RowIndex = NextRevealRow++;
	const FRevealRow& Row = RevealRows[RowIndex];
	if (UTextBlock* Text = Row.Text.Get())
	{
		Text->SetRenderOpacity(Row.Opacity);
		PlayRevealSound(Text == FinalRewardText, RowIndex);
	}

	if (!RevealRows.IsValidIndex(NextRevealRow))
	{
		OnRevealFinished.Broadcast(this);
		return;
	}

	const bool bNextIsFinal = RevealRows[NextRevealRow].Text.Get() == FinalRewardText;
	const float Delay = bNextIsFinal ? FinalRowDelay : RowRevealInterval;
	GetWorld()->GetTimerManager().SetTimer(RevealTimer, this,
		&UDeliverySettlementWidget::RevealNextRow, FMath::Max(0.01f, Delay), false);
}

void UDeliverySettlementWidget::PlayRevealSound(bool bFinal, int32 RowIndex)
{
	// 只向拥有面板的本机玩家播放；长提示音不会跨行叠加
	StopRevealSound();
	const APlayerController* Controller = GetOwningPlayer();
	USoundBase* Sound = bFinal ? FinalRevealSound : RowRevealSound;
	if (!Controller || !Controller->IsLocalController() || !Sound || Sound->IsLooping() || RevealVolume <= 0.f)
	{
		return;
	}
	const float Pitch = bFinal ? FinalRevealPitch : 1.f + RowPitchStep * RowIndex;
	RevealAudio = UGameplayStatics::SpawnSound2D(this, Sound, RevealVolume,
		FMath::Clamp(Pitch, 0.5f, 2.f), 0.f, nullptr, false, false);
}

void UDeliverySettlementWidget::StopRevealSound()
{
	if (RevealAudio)
	{
		RevealAudio->Stop();
		RevealAudio->DestroyComponent();
		RevealAudio = nullptr;
	}
}

void UDeliverySettlementWidget::CancelReveal()
{
	++RevealGeneration;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RevealTimer);
	}
	StopRevealSound();
	for (const FRevealRow& Row : RevealRows)
	{
		if (UTextBlock* Text = Row.Text.Get())
		{
			Text->SetRenderOpacity(Row.Opacity);
		}
	}
	RevealRows.Reset();
	NextRevealRow = 0;
}

void UDeliverySettlementWidget::NativeDestruct()
{
	CancelReveal();
	Super::NativeDestruct();
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
