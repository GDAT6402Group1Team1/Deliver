// Copyright Epic Games, Inc. All Rights Reserved.

#include "Interaction/DeliveryPromptSubsystem.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"

bool UDeliveryPromptSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// 只在真正跑起来的世界里建。编辑器预览世界、资产缩略图世界没有视口可挂。
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UDeliveryPromptSubsystem::Deinitialize()
{
	if (PromptWidget.IsValid() && GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(PromptWidget.ToSharedRef());
	}
	PromptWidget.Reset();
	Super::Deinitialize();
}

UDeliveryPromptSubsystem* UDeliveryPromptSubsystem::Get(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
		: nullptr;
	return World ? World->GetSubsystem<UDeliveryPromptSubsystem>() : nullptr;
}

void UDeliveryPromptSubsystem::PushPrompt(const FText& Text, const FVector& WorldAnchor, float HoldProgress)
{
	PromptText = Text;
	PromptAnchor = WorldAnchor;
	PromptHoldProgress = HoldProgress < 0.0f ? -1.0f : FMath::Clamp(HoldProgress, 0.0f, 1.0f);
	LastPushTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	EnsureWidget();
}

void UDeliveryPromptSubsystem::PushCornerHint(const FText& Text, bool bDimmed)
{
	CornerText = Text;
	bCornerDimmed = bDimmed;
	LastCornerPushTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	EnsureWidget();
}

void UDeliveryPromptSubsystem::PushObjective(const FText& Title, const FText& Detail, bool bUrgent)
{
	ObjectiveTitle = Title;
	ObjectiveDetail = Detail;
	bObjectiveUrgent = bUrgent;
	LastObjectivePushTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	EnsureWidget();
}

bool UDeliveryPromptSubsystem::IsObjectiveFresh() const
{
	const UWorld* World = GetWorld();
	return World && (World->GetTimeSeconds() - LastObjectivePushTime) < PromptTimeout;
}

bool UDeliveryPromptSubsystem::IsCornerFresh() const
{
	const UWorld* World = GetWorld();
	return World && (World->GetTimeSeconds() - LastCornerPushTime) < PromptTimeout;
}

bool UDeliveryPromptSubsystem::IsPromptFresh() const
{
	const UWorld* World = GetWorld();
	return World && (World->GetTimeSeconds() - LastPushTime) < PromptTimeout;
}

bool UDeliveryPromptSubsystem::ComputeScreenPosition(FVector2D& OutPosition) const
{
	const UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return false;
	}
	// 用 UMG 这个版本而不是 PlayerController::ProjectWorldLocationToScreen：
	// 它已经把 DPI 缩放折算进去了，返回的就是 Slate 坐标，直接当 Offset 用。
	return UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(PC, PromptAnchor, OutPosition, false);
}

void UDeliveryPromptSubsystem::EnsureWidget()
{
	if (PromptWidget.IsValid() || !GEngine || !GEngine->GameViewport)
	{
		return;
	}

	TWeakObjectPtr<const UDeliveryPromptSubsystem> WeakThis(this);

	const auto VisibilityAttr = TAttribute<EVisibility>::CreateLambda([WeakThis]()
	{
		const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		FVector2D Unused;
		const bool bShow = Self && Self->IsPromptFresh() && Self->ComputeScreenPosition(Unused);
		// HitTestInvisible：提示只是看的，不能挡住底下的点击。
		return bShow ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
	});

	const auto OffsetAttr = TAttribute<FMargin>::CreateLambda([WeakThis]()
	{
		const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		FVector2D Position = FVector2D::ZeroVector;
		if (Self)
		{
			Self->ComputeScreenPosition(Position);
		}
		return FMargin(Position.X, Position.Y, 0.0f, 0.0f);
	});

	const auto TextAttr = TAttribute<FText>::CreateLambda([WeakThis]()
	{
		const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		return Self ? Self->PromptText : FText::GetEmpty();
	});
	const auto ProgressAttr = TAttribute<TOptional<float>>::CreateLambda([WeakThis]()
	{
		const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		return Self && Self->PromptHoldProgress >= 0.0f
			? TOptional<float>(Self->PromptHoldProgress) : TOptional<float>();
	});
	const auto ProgressVisibility = TAttribute<EVisibility>::CreateLambda([WeakThis]()
	{
		const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		return Self && Self->PromptHoldProgress >= 0.0f
			? EVisibility::HitTestInvisible : EVisibility::Collapsed;
	});

	SAssignNew(PromptWidget, SConstraintCanvas)
	+ SConstraintCanvas::Slot()
		.Anchors(FAnchors(0.0f, 0.0f))
		.AutoSize(true)
		// 对齐点取底边中心：锚点算出来的是世界坐标那一点，浮窗应该浮在它上方。
		.Alignment(FVector2D(0.5f, 1.0f))
		.Offset(OffsetAttr)
		[
			SNew(SBorder)
			.Visibility(VisibilityAttr)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.55f))
			.Padding(FMargin(16.0f, 8.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Text(TextAttr)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 20))
					.ColorAndOpacity(FSlateColor(FLinearColor::White))
					.ShadowOffset(FVector2D(1.0f, 1.0f))
					.ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
				[
					SNew(SBox)
					.WidthOverride(150.0f)
					.HeightOverride(8.0f)
					.Visibility(ProgressVisibility)
					[
						SNew(SProgressBar)
						.Percent(ProgressAttr)
						.FillColorAndOpacity(FLinearColor(0.25f, 0.85f, 1.0f, 1.0f))
					]
				]
			]
		];

	// 左下角那一条。和上面的世界浮窗共用一个 Canvas，但锚点/时间戳完全独立。
	const auto CornerVisibility = TAttribute<EVisibility>::CreateLambda([WeakThis]()
	{
		const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		return (Self && Self->IsCornerFresh()) ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
	});
	const auto CornerTextAttr = TAttribute<FText>::CreateLambda([WeakThis]()
	{
		const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		return Self ? Self->CornerText : FText::GetEmpty();
	});
	const auto CornerColorAttr = TAttribute<FSlateColor>::CreateLambda([WeakThis]()
	{
		const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		// 冷却中压暗，玩家不用读秒也知道现在按了没用。
		return FSlateColor(Self && Self->bCornerDimmed
			? FLinearColor(0.55f, 0.55f, 0.55f, 1.0f) : FLinearColor::White);
	});

	StaticCastSharedPtr<SConstraintCanvas>(PromptWidget)->AddSlot()
		// 锚在视口左下角；对齐点也取左下，于是 Offset 就是"离左边多远、离底边多高"。
		.Anchors(FAnchors(0.0f, 1.0f))
		.AutoSize(true)
		.Alignment(FVector2D(0.0f, 1.0f))
		.Offset(FMargin(28.0f, -28.0f, 0.0f, 0.0f))
		[
			SNew(SBorder)
			.Visibility(CornerVisibility)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.45f))
			.Padding(FMargin(12.0f, 6.0f))
			[
				SNew(STextBlock)
				.Text(CornerTextAttr)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 15))
				.ColorAndOpacity(CornerColorAttr)
				.ShadowOffset(FVector2D(1.0f, 1.0f))
				.ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f))
			]
		];

	const auto ObjectiveVisibility = TAttribute<EVisibility>::CreateLambda([WeakThis]()
	{
		const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		return (Self && Self->IsObjectiveFresh()) ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
	});
	const auto ObjectiveTitleAttr = TAttribute<FText>::CreateLambda([WeakThis]()
	{
		const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		return Self ? Self->ObjectiveTitle : FText::GetEmpty();
	});
	const auto ObjectiveDetailAttr = TAttribute<FText>::CreateLambda([WeakThis]()
	{
		const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		return Self ? Self->ObjectiveDetail : FText::GetEmpty();
	});
	const auto ObjectiveTitleColor = TAttribute<FSlateColor>::CreateLambda([WeakThis]()
	{
		const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		// 超时和来电标红：这两种状态玩家必须立刻看见，其余时候不抢注意力
		return FSlateColor(Self && Self->bObjectiveUrgent
			? FLinearColor(1.0f, 0.45f, 0.35f, 1.0f) : FLinearColor::White);
	});
	const auto ObjectiveDetailVisibility = TAttribute<EVisibility>::CreateLambda([WeakThis]()
	{
		// 副行为空时整行折叠，否则标题下面会多出一块空白
		const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		return (Self && !Self->ObjectiveDetail.IsEmpty()) ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
	});

	StaticCastSharedPtr<SConstraintCanvas>(PromptWidget)->AddSlot()
		// 锚在视口顶边中央；对齐点取上中，于是 Offset.Top 就是"离顶边多远"
		.Anchors(FAnchors(0.5f, 0.0f))
		.AutoSize(true)
		.Alignment(FVector2D(0.5f, 0.0f))
		.Offset(FMargin(0.0f, 28.0f, 0.0f, 0.0f))
		[
			SNew(SBorder)
			.Visibility(ObjectiveVisibility)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.5f))
			.Padding(FMargin(18.0f, 8.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Text(ObjectiveTitleAttr)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 17))
					.ColorAndOpacity(ObjectiveTitleColor)
					.ShadowOffset(FVector2D(1.0f, 1.0f))
					.ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f))
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
					.Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Visibility(ObjectiveDetailVisibility)
					.Text(ObjectiveDetailAttr)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 14))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.85f, 0.85f, 1.0f)))
					.ShadowOffset(FVector2D(1.0f, 1.0f))
					.ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f))
				]
			]
		];

	GEngine->GameViewport->AddViewportWidgetContent(PromptWidget.ToSharedRef(), 5);
}
