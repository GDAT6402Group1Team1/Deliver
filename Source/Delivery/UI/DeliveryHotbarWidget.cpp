// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/DeliveryHotbarWidget.h"
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

void UDeliveryHotbarWidget::SetInventory(UDeliveryInventoryComponent* InInventory)
{
	Inventory = InInventory;
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

void UDeliveryHotbarWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	Refresh();
}

void UDeliveryHotbarWidget::BuildWidgetTree()
{
	if (!WidgetTree || SlotBorders.Num() == UDeliveryInventoryComponent::SlotCount) return;
	WidgetTree->RootWidget = nullptr;
	SlotBorders.Reset();
	SlotIcons.Reset();
	DurabilityBars.Reset();
	ConnectedHotbarTexture = LoadObject<UTexture2D>(nullptr,
		TEXT("/Game/UI/Inventory/Textures/T_HotbarConnectedCartoon.T_HotbarConnectedCartoon"));

	UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("HotbarRoot"));
	WidgetTree->RootWidget = Root;
	UBorder* Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("HotbarFrame"));
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
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("HotbarRow"));
	Frame->SetContent(Row);
	UCanvasPanelSlot* FrameSlot = Root->AddChildToCanvas(Frame);
	FrameSlot->SetAnchors(FAnchors(0.5f, 1.0f));
	FrameSlot->SetAlignment(FVector2D(0.5f, 1.0f));
	FrameSlot->SetPosition(FVector2D(0.0f, -24.0f));
	FrameSlot->SetAutoSize(true);

	for (int32 Index = 0; Index < UDeliveryInventoryComponent::SlotCount; ++Index)
	{
		USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>();
		Size->SetWidthOverride(76.0f);
		Size->SetHeightOverride(78.0f);
		UHorizontalBoxSlot* SizeSlot = Row->AddChildToHorizontalBox(Size);
		SizeSlot->SetPadding(FMargin(0.0f));

		UBorder* Border = WidgetTree->ConstructWidget<UBorder>();
		Border->SetPadding(FMargin(8.0f, 7.0f));
		Border->SetHorizontalAlignment(HAlign_Fill);
		Border->SetVerticalAlignment(VAlign_Fill);
		Size->SetContent(Border);
		SlotBorders.Add(Border);

		UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();
		Border->SetContent(Column);

		USizeBox* IconSize = WidgetTree->ConstructWidget<USizeBox>();
		IconSize->SetWidthOverride(52.0f);
		IconSize->SetHeightOverride(52.0f);
		UImage* Icon = WidgetTree->ConstructWidget<UImage>();
		Icon->SetVisibility(ESlateVisibility::Collapsed);
		IconSize->SetContent(Icon);
		UVerticalBoxSlot* IconSlot = Column->AddChildToVerticalBox(IconSize);
		IconSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		IconSlot->SetHorizontalAlignment(HAlign_Center);
		IconSlot->SetVerticalAlignment(VAlign_Center);
		SlotIcons.Add(Icon);

		UProgressBar* Durability = WidgetTree->ConstructWidget<UProgressBar>();
		Durability->SetPercent(0.0f);
		Durability->SetFillColorAndOpacity(FLinearColor(0.35f, 0.95f, 0.45f));
		Column->AddChildToVerticalBox(Durability)->SetPadding(FMargin(1.0f, 2.0f, 1.0f, 0.0f));
		DurabilityBars.Add(Durability);
	}
}

void UDeliveryHotbarWidget::Refresh()
{
	if (!Inventory || SlotBorders.Num() != UDeliveryInventoryComponent::SlotCount) return;
	const float FullFlash = Inventory->GetFullFeedbackAlpha();
	for (int32 Index = 0; Index < UDeliveryInventoryComponent::SlotCount; ++Index)
	{
		ADeliveryHandheldItem* Item = Inventory->GetItemInSlot(Index);
		UDeliveryInventoryItemComponent* Data = Item ? Item->GetItemComponent() : nullptr;
		UTexture2D* IconTexture = Data ? Data->Icon.Get() : nullptr;
		SlotIcons[Index]->SetVisibility(IconTexture ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (IconTexture && SlotIcons[Index]->GetBrush().GetResourceObject() != IconTexture)
		{
			SlotIcons[Index]->SetBrushFromTexture(IconTexture, false);
		}
		const bool bShowDurability = Data && Data->UsesDurability();
		DurabilityBars[Index]->SetVisibility(bShowDurability
			? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bShowDurability) DurabilityBars[Index]->SetPercent(Data->GetDurabilityFraction());

		FLinearColor Base = FLinearColor(1.0f, 1.0f, 1.0f, Data ? 0.10f : 0.0f);
		if (Index > 0 && FullFlash > 0.0f)
		{
			Base = FMath::Lerp(Base, FLinearColor(1.0f, 0.05f, 0.02f, 0.70f), FullFlash);
		}
		else if (Data)
		{
			Base += Index == 0 ? FLinearColor(0.12f, 0.06f, 0.0f, 0.0f) : FLinearColor(0.0f, 0.04f, 0.05f, 0.0f);
		}
		SlotBorders[Index]->SetBrushColor(Base);
	}
}
