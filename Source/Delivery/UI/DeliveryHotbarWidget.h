// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DeliveryHotbarWidget.generated.h"

class UBorder;
class UDeliveryInventoryComponent;
class UProgressBar;
class UTextBlock;
class UTexture2D;

/** Always-visible, compact five-slot UMG hotbar. Its tree is built here so every pawn gets identical UI. */
UCLASS(Blueprintable)
class DELIVERY_API UDeliveryHotbarWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetInventory(UDeliveryInventoryComponent* InInventory);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	void BuildWidgetTree();
	void Refresh();

	UPROPERTY(Transient)
	TObjectPtr<UDeliveryInventoryComponent> Inventory;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UBorder>> SlotBorders;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextBlock>> SlotNames;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextBlock>> SlotNumbers;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UProgressBar>> DurabilityBars;

	/** Original connected hotbar artwork generated for the game's soft cartoon visual style. */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> ConnectedHotbarTexture;

};
