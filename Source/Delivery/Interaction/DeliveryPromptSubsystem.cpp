// Copyright Epic Games, Inc. All Rights Reserved.

#include "Interaction/DeliveryPromptSubsystem.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SConstraintCanvas.h"
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

void UDeliveryPromptSubsystem::PushPrompt(const FText& Text, const FVector& WorldAnchor)
{
	PromptText = Text;
	PromptAnchor = WorldAnchor;
	LastPushTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	EnsureWidget();
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
				SNew(STextBlock)
				.Text(TextAttr)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 20))
				.ColorAndOpacity(FSlateColor(FLinearColor::White))
				.ShadowOffset(FVector2D(1.0f, 1.0f))
				.ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f))
			]
		];

	GEngine->GameViewport->AddViewportWidgetContent(PromptWidget.ToSharedRef(), 5);
}
