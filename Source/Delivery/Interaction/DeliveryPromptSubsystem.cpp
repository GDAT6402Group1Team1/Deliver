// Copyright Epic Games, Inc. All Rights Reserved.

#include "Interaction/DeliveryPromptSubsystem.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Styling/CoreStyle.h"
#include "Rendering/DrawElements.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
// 沿用头顶小气泡，在旁边画出 T 键，玩家能直接看出交谈入口
class SDeliveryConversationPromptIcon : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SDeliveryConversationPromptIcon) {} SLATE_END_ARGS()
    void Construct(const FArguments&) {}
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(56.f,30.f); }
    virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&,
        FSlateWindowElementList& Elements, int32 Layer, const FWidgetStyle& Style, bool) const override
    {
        const TArray<FVector2D> Outline={{5,3},{19,3},{21,5},{21,15},{19,17},{11,17},{6,21},{6,17},{5,17},{3,15},{3,5},{5,3}};
        const FLinearColor Tint=Style.GetColorAndOpacityTint();
        FSlateDrawElement::MakeLines(Elements,Layer,Geometry.ToPaintGeometry(),Outline,
            ESlateDrawEffect::None,FLinearColor(0,0,0,.85f)*Tint,true,4.f);
        FSlateDrawElement::MakeLines(Elements,Layer+1,Geometry.ToPaintGeometry(),Outline,
            ESlateDrawEffect::None,FLinearColor::White*Tint,true,2.f);
        for (float X : {7.f,12.f,17.f})
        {
            const TArray<FVector2D> Dot={{X,10},{X+.25f,10}};
            FSlateDrawElement::MakeLines(Elements,Layer+2,Geometry.ToPaintGeometry(),Dot,
                ESlateDrawEffect::None,FLinearColor::White*Tint,true,2.5f);
        }
        const TArray<FVector2D> KeyOutline={{31,2},{53,2},{53,26},{31,26},{31,2}};
        FSlateDrawElement::MakeLines(Elements,Layer+2,Geometry.ToPaintGeometry(),KeyOutline,
            ESlateDrawEffect::None,FLinearColor(0,0,0,.9f)*Tint,true,5.f);
        FSlateDrawElement::MakeLines(Elements,Layer+3,Geometry.ToPaintGeometry(),KeyOutline,
            ESlateDrawEffect::None,FLinearColor(1.f,.9f,.5f)*Tint,true,2.f);
        const TArray<FVector2D> KeyTop={{36,8},{48,8}};
        const TArray<FVector2D> KeyStem={{42,8},{42,21}};
        for (const TArray<FVector2D>* Stroke : {&KeyTop,&KeyStem})
        {
            FSlateDrawElement::MakeLines(Elements,Layer+3,Geometry.ToPaintGeometry(),*Stroke,
                ESlateDrawEffect::None,FLinearColor(0,0,0,.9f)*Tint,true,5.f);
            FSlateDrawElement::MakeLines(Elements,Layer+4,Geometry.ToPaintGeometry(),*Stroke,
                ESlateDrawEffect::None,FLinearColor::White*Tint,true,2.5f);
        }
        return Layer+4;
    }
};
}

// 只在需要游戏界面的世界中创建提示系统
bool UDeliveryPromptSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// 只在真正跑起来的世界里建；编辑器预览世界、资产缩略图世界没有视口可挂
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

// 移除提示控件并清理显示状态
void UDeliveryPromptSubsystem::Deinitialize()
{
	if (PromptWidget.IsValid() && GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(PromptWidget.ToSharedRef());
	}
	PromptWidget.Reset();
	Super::Deinitialize();
}

// 从当前世界取得提示系统
UDeliveryPromptSubsystem* UDeliveryPromptSubsystem::Get(const UObject* WorldContextObject)
{
	const UWorld* World =
	    GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
		        : nullptr;
	return World ? World->GetSubsystem<UDeliveryPromptSubsystem>() : nullptr;
}

// 交互提示与固定提示

// 更新交互提示的文字、世界位置和长按进度
void UDeliveryPromptSubsystem::PushPrompt(const FText& Text, const FVector& WorldAnchor, float HoldProgress, bool bConversationIcon)
{
	PromptText = Text;
	bPromptConversationIcon = bConversationIcon;
	PromptAnchor = WorldAnchor;
	PromptHoldProgress = HoldProgress < 0.0f ? -1.0f : FMath::Clamp(HoldProgress, 0.0f, 1.0f);
	LastPushTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	EnsureWidget();
}

// 角落提示由提示组件明确更新，不再依赖持续刷新时间戳
void UDeliveryPromptSubsystem::SetCornerHint(UObject* Source, const FText& Text, bool bDimmed)
{
    if (!IsValid(Source)) return;
    CornerSource = Source;
    CornerText = Text;
    bCornerDimmed = bDimmed;
    EnsureWidget();
}

// 旧角色退出时不能清掉新来源已经写入的提示
void UDeliveryPromptSubsystem::ClearCornerHint(const UObject* Source)
{
    if (CornerSource.Get() != Source) return;
    CornerSource.Reset();
    CornerText = FText::GetEmpty();
}

// 目的地与字幕

// 更新顶部任务提示
void UDeliveryPromptSubsystem::PushObjective(const FText& Title, const FText& Detail, bool bUrgent)
{
	ObjectiveTitle = Title;
	ObjectiveDetail = Detail;
	bObjectiveUrgent = bUrgent;
	LastObjectivePushTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	EnsureWidget();
}

// 更新正在显示的对白字幕
void UDeliveryPromptSubsystem::SetSubtitle(const FText& Speaker, const FText& Text)
{
	SubtitleSpeaker = Speaker;
	SubtitleText = Text;
	bSubtitleVisible = !Text.IsEmpty();
	EnsureWidget();
}

// 清除当前对白字幕
void UDeliveryPromptSubsystem::ClearSubtitle()
{
	SubtitleSpeaker = FText::GetEmpty();
	SubtitleText = FText::GetEmpty();
	bSubtitleVisible = false;
}

// 提示有效性检查

bool UDeliveryPromptSubsystem::IsObjectiveFresh() const
{
	const UWorld* World = GetWorld();
	return World && (World->GetTimeSeconds() - LastObjectivePushTime) < PromptTimeout;
}

bool UDeliveryPromptSubsystem::HasCornerHint() const
{
    return CornerSource.IsValid() && !CornerText.IsEmpty();
}

bool UDeliveryPromptSubsystem::IsPromptFresh() const
{
	const UWorld* World = GetWorld();
	return World && (World->GetTimeSeconds() - LastPushTime) < PromptTimeout;
}

// 将世界中的提示位置转换成屏幕坐标
bool UDeliveryPromptSubsystem::ComputeScreenPosition(FVector2D& OutPosition) const
{
	const UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return false;
	}
	// 用 UMG 这个版本而不是 PlayerController::ProjectWorldLocationToScreen：
	// 它已经把 DPI 缩放折算进去了，返回的就是 Slate 坐标，直接当 Offset 用
	return UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(PC, PromptAnchor, OutPosition, false);
}

// 创建提示控件

// 在本机创建提示控件，并连接各项显示数据
void UDeliveryPromptSubsystem::EnsureWidget()
{
	if (PromptWidget.IsValid() || !GEngine || !GEngine->GameViewport)
	{
		return;
	}

	TWeakObjectPtr<const UDeliveryPromptSubsystem> WeakThis(this);

	const auto VisibilityAttr = TAttribute<EVisibility>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    FVector2D Unused;
		    const bool bShow = Self && Self->IsPromptFresh() && Self->ComputeScreenPosition(Unused);
		    // HitTestInvisible：提示只是看的，不能挡住底下的点击
		    return bShow ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
	    });

	const auto OffsetAttr = TAttribute<FMargin>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    FVector2D Position = FVector2D::ZeroVector;
		    if (Self)
		    {
			    Self->ComputeScreenPosition(Position);
			    // NPC 字幕在头顶上方，交谈键放在下面，开口时两者可以同时显示
			    if (Self->bPromptConversationIcon) Position += FVector2D(0.f,76.f);
		    }
		    return FMargin(Position.X, Position.Y, 0.0f, 0.0f);
	    });

    const auto TextPromptVisibility = TAttribute<EVisibility>::CreateLambda([WeakThis,VisibilityAttr]()
    {
        const auto* Self=WeakThis.Get();
        return Self && !Self->bPromptConversationIcon ? VisibilityAttr.Get() : EVisibility::Collapsed;
    });
    const auto IconPromptVisibility = TAttribute<EVisibility>::CreateLambda([WeakThis,VisibilityAttr]()
    {
        const auto* Self=WeakThis.Get();
        return Self && Self->bPromptConversationIcon ? VisibilityAttr.Get() : EVisibility::Collapsed;
    });

	const auto TextAttr = TAttribute<FText>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    return Self ? Self->PromptText : FText::GetEmpty();
	    });
	const auto ProgressAttr = TAttribute<TOptional<float>>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    return Self && Self->PromptHoldProgress >= 0.0f ? TOptional<float>(Self->PromptHoldProgress)
			                                                : TOptional<float>();
	    });
	const auto ProgressVisibility = TAttribute<EVisibility>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    return Self && Self->PromptHoldProgress >= 0.0f ? EVisibility::HitTestInvisible
			                                                : EVisibility::Collapsed;
	    });

	SAssignNew(PromptWidget, SConstraintCanvas) +
	    SConstraintCanvas::Slot()
	        .Anchors(FAnchors(0.0f, 0.0f))
	        .AutoSize(true)
	        // 对齐点取底边中心：锚点算出来的是世界坐标那一点，浮窗应该浮在它上方
	        .Alignment(FVector2D(0.5f, 1.0f))
	        .Offset(OffsetAttr)
	            [SNew(SBorder)
				     .Visibility(TextPromptVisibility)
				     .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				     .BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.55f))
				     .Padding(FMargin(
	                     16.0f,
	                     8.0f))[SNew(SVerticalBox) +
						        SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
						            [SNew(STextBlock)
						                 .Text(TextAttr)
						                 .Font(FCoreStyle::GetDefaultFontStyle("Bold", 20))
						                 .ColorAndOpacity(FSlateColor(FLinearColor::White))
						                 .ShadowOffset(FVector2D(1.0f, 1.0f))
						                 .ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f))] +
						        SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
						            [SNew(SBox).WidthOverride(150.0f).HeightOverride(8.0f).Visibility(
	                                    ProgressVisibility)[SNew(SProgressBar)
										                        .Percent(ProgressAttr)
										                        .FillColorAndOpacity(FLinearColor(
	                                                                0.25f, 0.85f, 1.0f, 1.0f))]]]];

    StaticCastSharedPtr<SConstraintCanvas>(PromptWidget)->AddSlot()
        .Anchors(FAnchors(0.f,0.f)).AutoSize(true).Alignment(FVector2D(.5f,1.f)).Offset(OffsetAttr)
        [SNew(SDeliveryConversationPromptIcon).Visibility(IconPromptVisibility)];

	// 左下角那一条；和上面的世界浮窗共用一个 Canvas，但锚点/时间戳完全独立
	const auto CornerVisibility = TAttribute<EVisibility>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    return (Self && Self->HasCornerHint()) ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
	    });
	const auto CornerTextAttr = TAttribute<FText>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    return Self ? Self->CornerText : FText::GetEmpty();
	    });
	const auto CornerColorAttr = TAttribute<FSlateColor>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    // 冷却中压暗，玩家不用读秒也知道现在按了没用
		    return FSlateColor(Self && Self->bCornerDimmed ? FLinearColor(0.55f, 0.55f, 0.55f, 1.0f)
			                                               : FLinearColor::White);
	    });

	StaticCastSharedPtr<SConstraintCanvas>(PromptWidget)
	    ->AddSlot()
	    // 锚在视口左下角；对齐点也取左下，于是 Offset 就是"离左边多远、离底边多高"
	    .Anchors(FAnchors(0.0f, 1.0f))
	    .AutoSize(true)
	    .Alignment(FVector2D(0.0f, 1.0f))
	    .Offset(FMargin(
	        28.0f, -28.0f, 0.0f,
	        0.0f))[SNew(SBorder)
			           .Visibility(CornerVisibility)
			           .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			           .BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.45f))
			           .Padding(FMargin(
	                       12.0f, 6.0f))[SNew(STextBlock)
						                     .Text(CornerTextAttr)
						                     .Font(FCoreStyle::GetDefaultFontStyle("Bold", 15))
						                     .ColorAndOpacity(CornerColorAttr)
						                     .ShadowOffset(FVector2D(1.0f, 1.0f))
						                     .ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f))]];

	const auto ObjectiveVisibility = TAttribute<EVisibility>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    return (Self && Self->IsObjectiveFresh()) ? EVisibility::HitTestInvisible
			                                          : EVisibility::Collapsed;
	    });
	const auto ObjectiveTitleAttr = TAttribute<FText>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    return Self ? Self->ObjectiveTitle : FText::GetEmpty();
	    });
	const auto ObjectiveDetailAttr = TAttribute<FText>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    return Self ? Self->ObjectiveDetail : FText::GetEmpty();
	    });
	const auto ObjectiveTitleColor = TAttribute<FSlateColor>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    // 超时和来电标红：这两种状态玩家必须立刻看见，其余时候不抢注意力
		    return FSlateColor(Self && Self->bObjectiveUrgent ? FLinearColor(1.0f, 0.45f, 0.35f, 1.0f)
			                                                  : FLinearColor::White);
	    });
	const auto ObjectiveDetailVisibility = TAttribute<EVisibility>::CreateLambda(
	    [WeakThis]()
	    {
		    // 副行为空时整行折叠，否则标题下面会多出一块空白
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    return (Self && !Self->ObjectiveDetail.IsEmpty()) ? EVisibility::HitTestInvisible
			                                                  : EVisibility::Collapsed;
	    });

	StaticCastSharedPtr<SConstraintCanvas>(PromptWidget)
	    ->AddSlot()
	    // 锚在视口顶边中央；对齐点取上中，于是 Offset.Top 就是"离顶边多远"
	    .Anchors(FAnchors(0.5f, 0.0f))
	    .AutoSize(true)
	    .Alignment(FVector2D(0.5f, 0.0f))
	    .Offset(FMargin(0.0f, 28.0f, 0.0f, 0.0f))
	        [SNew(SBorder)
			     .Visibility(ObjectiveVisibility)
			     .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			     .BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.5f))
			     .Padding(FMargin(18.0f, 8.0f))
			         [SNew(SVerticalBox) +
			          SVerticalBox::Slot().AutoHeight().HAlign(
	                      HAlign_Center)[SNew(STextBlock)
						                     .Text(ObjectiveTitleAttr)
						                     .Font(FCoreStyle::GetDefaultFontStyle("Bold", 17))
						                     .ColorAndOpacity(ObjectiveTitleColor)
						                     .ShadowOffset(FVector2D(1.0f, 1.0f))
						                     .ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f))] +
			          SVerticalBox::Slot()
			              .AutoHeight()
			              .HAlign(HAlign_Center)
			              .Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
			                  [SNew(STextBlock)
			                       .Visibility(ObjectiveDetailVisibility)
			                       .Text(ObjectiveDetailAttr)
			                       .Font(FCoreStyle::GetDefaultFontStyle("Regular", 14))
			                       .ColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.85f, 0.85f, 1.0f)))
			                       .ShadowOffset(FVector2D(1.0f, 1.0f))
			                       .ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f))]]];

	const auto SubtitleVisibility = TAttribute<EVisibility>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    return (Self && Self->bSubtitleVisible) ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
	    });
	const auto SubtitleSpeakerAttr = TAttribute<FText>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    return Self ? Self->SubtitleSpeaker : FText::GetEmpty();
	    });
	const auto SubtitleTextAttr = TAttribute<FText>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    return Self ? Self->SubtitleText : FText::GetEmpty();
	    });
	const auto SubtitleSpeakerVisibility = TAttribute<EVisibility>::CreateLambda(
	    [WeakThis]()
	    {
		    const UDeliveryPromptSubsystem* Self = WeakThis.Get();
		    return (Self && !Self->SubtitleSpeaker.IsEmpty()) ? EVisibility::HitTestInvisible
			                                                  : EVisibility::Collapsed;
	    });

	StaticCastSharedPtr<SConstraintCanvas>(PromptWidget)
	    ->AddSlot()
	    // 下方居中并向上留出 Hotbar 空间；限制宽度后长句会自动换行
	    .Anchors(FAnchors(0.5f, 1.0f))
	    .AutoSize(true)
	    .Alignment(FVector2D(0.5f, 1.0f))
	    .Offset(FMargin(0.0f, -105.0f, 0.0f, 0.0f))[SNew(SBox).WidthOverride(
	        920.0f)[SNew(SBorder)
			            .Visibility(SubtitleVisibility)
			            .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			            .BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.68f))
			            .Padding(FMargin(22.0f, 10.0f))
			                [SNew(SVerticalBox) +
			                 SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			                     [SNew(STextBlock)
			                          .Visibility(SubtitleSpeakerVisibility)
			                          .Text(SubtitleSpeakerAttr)
			                          .Font(FCoreStyle::GetDefaultFontStyle("Bold", 15))
			                          .ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.72f, 0.28f, 1.0f)))
			                          .Justification(ETextJustify::Center)] +
			                 SVerticalBox::Slot()
			                     .AutoHeight()
			                     .HAlign(HAlign_Fill)
			                     .Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
			                         [SNew(STextBlock)
			                              .Text(SubtitleTextAttr)
			                              .Font(FCoreStyle::GetDefaultFontStyle("Bold", 20))
			                              .ColorAndOpacity(FSlateColor(FLinearColor::White))
			                              .Justification(ETextJustify::Center)
			                              .AutoWrapText(true)
			                              .WrapTextAt(870.0f)
			                              .ShadowOffset(FVector2D(1.0f, 1.0f))
			                              .ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f))]]]];

	GEngine->GameViewport->AddViewportWidgetContent(PromptWidget.ToSharedRef(), 5);
}
