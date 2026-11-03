// 显示背包图标与耐久；当前控件树由原生代码建立
// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/DeliveryHotbarWidget.h"
#include "UI/Controllers/DeliveryHotbarController.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Texture2D.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryInventoryItemComponent.h"

// 把背包交给界面控制器，并立即刷新已有控件
void UDeliveryHotbarWidget::SetInventory(UDeliveryInventoryComponent *InInventory)
{
    Inventory = InInventory;
    if (!WidgetController)
        WidgetController = NewObject<UDeliveryHotbarController>(this);
    WidgetController->SetInventory(InInventory);
    Refresh();
}

TSharedRef<SWidget> UDeliveryHotbarWidget::RebuildWidget()
{
    BuildWidgetTree();
    return Super::RebuildWidget();
}

void UDeliveryHotbarWidget::NativeConstruct()
{
    Super::NativeConstruct();
    Refresh();
}

void UDeliveryHotbarWidget::NativeTick(const FGeometry &MyGeometry, float InDeltaTime)
{
    Super::NativeTick(MyGeometry, InDeltaTime);
    Refresh();
}

// 创建快捷栏外框、槽位、图标和耐久条
void UDeliveryHotbarWidget::BuildWidgetTree()
{
    if (!WidgetTree || SlotBorders.Num() == UDeliveryInventoryComponent::SlotCount)
        return;
    // 重建前清空旧控件引用，避免槽位与图标数组错位
    WidgetTree->RootWidget = nullptr;
    SlotBorders.Reset();
    SlotIcons.Reset();
    DurabilityBars.Reset();
    ConnectedHotbarTexture = LoadObject<UTexture2D>(
        nullptr, TEXT("/Game/UI/Inventory/Textures/T_HotbarConnectedCartoon.T_HotbarConnectedCartoon"));

    UCanvasPanel *Root =
        WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("HotbarRoot"));
    WidgetTree->RootWidget = Root;
    UBorder *Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("HotbarFrame"));
    Frame->SetPadding(FMargin(6.0f, 5.0f));
    if (ConnectedHotbarTexture)
    {
        Frame->SetBrushFromTexture(ConnectedHotbarTexture);
        Frame->SetBrushColor(FLinearColor::White);
    }
    else
    {
        Frame->SetBrushColor(FLinearColor(0.025f, 0.08f, 0.11f, 0.96f));
    }
    UHorizontalBox *Row =
        WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("HotbarRow"));
    Frame->SetContent(Row);
    UCanvasPanelSlot *FrameSlot = Root->AddChildToCanvas(Frame);
    FrameSlot->SetAnchors(FAnchors(0.5f, 1.0f));
    FrameSlot->SetAlignment(FVector2D(0.5f, 1.0f));
    FrameSlot->SetPosition(FVector2D(0.0f, -24.0f));
    FrameSlot->SetAutoSize(true);

    // 按背包槽位顺序创建图标和耐久条，后续刷新使用相同索引
    for (int32 Index = 0; Index < UDeliveryInventoryComponent::SlotCount; ++Index)
    {
        USizeBox *Size = WidgetTree->ConstructWidget<USizeBox>();
        Size->SetWidthOverride(76.0f);
        Size->SetHeightOverride(78.0f);
        UHorizontalBoxSlot *SizeSlot = Row->AddChildToHorizontalBox(Size);
        SizeSlot->SetPadding(FMargin(0.0f));

        UBorder *Border = WidgetTree->ConstructWidget<UBorder>();
        Border->SetPadding(FMargin(8.0f, 7.0f));
        Border->SetHorizontalAlignment(HAlign_Fill);
        Border->SetVerticalAlignment(VAlign_Fill);
        Size->SetContent(Border);
        SlotBorders.Add(Border);

        UVerticalBox *Column = WidgetTree->ConstructWidget<UVerticalBox>();
        Border->SetContent(Column);

        USizeBox *IconSize = WidgetTree->ConstructWidget<USizeBox>();
        IconSize->SetWidthOverride(52.0f);
        IconSize->SetHeightOverride(52.0f);
        UImage *Icon = WidgetTree->ConstructWidget<UImage>();
        Icon->SetVisibility(ESlateVisibility::Collapsed);
        IconSize->SetContent(Icon);
        UVerticalBoxSlot *IconSlot = Column->AddChildToVerticalBox(IconSize);
        IconSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        IconSlot->SetHorizontalAlignment(HAlign_Center);
        IconSlot->SetVerticalAlignment(VAlign_Center);
        SlotIcons.Add(Icon);

        UProgressBar *Durability = WidgetTree->ConstructWidget<UProgressBar>();
        Durability->SetPercent(0.0f);
        Durability->SetFillColorAndOpacity(FLinearColor(0.35f, 0.95f, 0.45f));
        Column->AddChildToVerticalBox(Durability)->SetPadding(FMargin(1.0f, 2.0f, 1.0f, 0.0f));
        DurabilityBars.Add(Durability);
    }
}

// 控件从界面控制器取显示值，耐久和满包闪烁继续实时刷新
void UDeliveryHotbarWidget::Refresh()
{
    if (!WidgetController || SlotBorders.Num() != UDeliveryInventoryComponent::SlotCount)
        return;
    const float FullFlash = WidgetController->GetFullFeedback();
    for (int32 Index = 0; Index < UDeliveryInventoryComponent::SlotCount; ++Index)
    {
        const FDeliveryHotbarSlot SlotData = WidgetController->GetSlot(Index);
        UTexture2D *IconTexture = SlotData.Icon.Get();
        SlotIcons[Index]->SetVisibility(IconTexture ? ESlateVisibility::HitTestInvisible
                                                    : ESlateVisibility::Collapsed);
        if (IconTexture && SlotIcons[Index]->GetBrush().GetResourceObject() != IconTexture)
        {
            SlotIcons[Index]->SetBrushFromTexture(IconTexture, false);
        }
        const bool bShowDurability = SlotData.bShowDurability;
        DurabilityBars[Index]->SetVisibility(bShowDurability ? ESlateVisibility::HitTestInvisible
                                                             : ESlateVisibility::Collapsed);
        if (bShowDurability)
            DurabilityBars[Index]->SetPercent(SlotData.Durability);

        FLinearColor Base = FLinearColor(1.0f, 1.0f, 1.0f, SlotData.bOccupied ? 0.10f : 0.0f);
        if (Index > 0 && FullFlash > 0.0f)
        {
            Base = FMath::Lerp(Base, FLinearColor(1.0f, 0.05f, 0.02f, 0.70f), FullFlash);
        }
        else if (SlotData.bOccupied)
        {
            Base +=
                Index == 0 ? FLinearColor(0.12f, 0.06f, 0.0f, 0.0f) : FLinearColor(0.0f, 0.04f, 0.05f, 0.0f);
        }
        SlotBorders[Index]->SetBrushColor(Base);
    }
}
