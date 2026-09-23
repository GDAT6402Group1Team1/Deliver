// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/DeliveryHotbarWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Styling/CoreStyle.h"

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
	SlotNames.Reset();
	SlotNumbers.Reset();
	DurabilityBars.Reset();

	UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("HotbarRoot"));
	WidgetTree->RootWidget = Root;
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("HotbarRow"));
	UCanvasPanelSlot* RowSlot = Root->AddChildToCanvas(Row);
	RowSlot->SetAnchors(FAnchors(0.5f, 1.0f));
	RowSlot->SetAlignment(FVector2D(0.5f, 1.0f));
	RowSlot->SetPosition(FVector2D(0.0f, -28.0f));
	RowSlot->SetAutoSize(true);

	for (int32 Index = 0; Index < UDeliveryInventoryComponent::SlotCount; ++Index)
	{
		USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>();
		Size->SetWidthOverride(Index == 0 ? 118.0f : 102.0f);
		Size->SetHeightOverride(Index == 0 ? 112.0f : 98.0f);
		UHorizontalBoxSlot* SizeSlot = Row->AddChildToHorizontalBox(Size);
		SizeSlot->SetPadding(FMargin(5.0f, Index == 0 ? 0.0f : 12.0f, 5.0f, 0.0f));

		UBorder* Border = WidgetTree->ConstructWidget<UBorder>();
		Border->SetPadding(FMargin(8.0f, 5.0f));
		Border->SetHorizontalAlignment(HAlign_Fill);
		Border->SetVerticalAlignment(VAlign_Fill);
		Size->SetContent(Border);
		SlotBorders.Add(Border);

		UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();
		Border->SetContent(Column);

		UTextBlock* Number = WidgetTree->ConstructWidget<UTextBlock>();
		Number->SetText(FText::AsNumber(Index + 1));
		Number->SetColorAndOpacity(Index == 0 ? FLinearColor(1.0f, 0.82f, 0.23f) : FLinearColor(0.55f, 0.92f, 0.95f));
		Number->SetJustification(ETextJustify::Left);
		Number->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", Index == 0 ? 18 : 15));
		Column->AddChildToVerticalBox(Number)->SetPadding(FMargin(1.0f, 0.0f, 0.0f, 2.0f));
		SlotNumbers.Add(Number);

		UTextBlock* Name = WidgetTree->ConstructWidget<UTextBlock>();
		Name->SetJustification(ETextJustify::Center);
		Name->SetColorAndOpacity(FLinearColor::White);
		Name->SetAutoWrapText(true);
		Name->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", Index == 0 ? 16 : 14));
		UVerticalBoxSlot* NameSlot = Column->AddChildToVerticalBox(Name);
		NameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		NameSlot->SetHorizontalAlignment(HAlign_Center);
		NameSlot->SetVerticalAlignment(VAlign_Center);
		SlotNames.Add(Name);

		UProgressBar* Durability = WidgetTree->ConstructWidget<UProgressBar>();
		Durability->SetPercent(0.0f);
		Durability->SetFillColorAndOpacity(FLinearColor(0.35f, 0.95f, 0.45f));
		Column->AddChildToVerticalBox(Durability)->SetPadding(FMargin(2.0f, 3.0f, 2.0f, 1.0f));
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
		const FText EmptyLabel = Index == 0
			? NSLOCTEXT("DeliveryHotbar", "EmptyHand", "空手")
			: NSLOCTEXT("DeliveryHotbar", "EmptySlot", "空");
		SlotNames[Index]->SetText(Data ? Data->DisplayName : EmptyLabel);
		DurabilityBars[Index]->SetVisibility(Data ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
		if (Data) DurabilityBars[Index]->SetPercent(Data->GetDurabilityFraction());

		FLinearColor Base = Index == 0
			? FLinearColor(0.13f, 0.10f, 0.055f, 0.94f)
			: FLinearColor(0.035f, 0.085f, 0.10f, 0.90f);
		if (Index > 0 && FullFlash > 0.0f)
		{
			Base = FMath::Lerp(Base, FLinearColor(0.72f, 0.025f, 0.02f, 0.98f), FullFlash);
		}
		else if (Data)
		{
			Base += Index == 0 ? FLinearColor(0.14f, 0.08f, 0.0f, 0.0f) : FLinearColor(0.0f, 0.08f, 0.08f, 0.0f);
		}
		SlotBorders[Index]->SetBrushColor(Base);
	}
}
