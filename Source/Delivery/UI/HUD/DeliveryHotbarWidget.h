// 创建五槽快捷栏，并根据背包数据更新图标和耐久
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DeliveryHotbarWidget.generated.h"

class UDeliveryHotbarController;
class UBorder;
class UDeliveryInventoryComponent;
class UImage;
class UProgressBar;
class UTexture2D;

/** 五格快捷栏，由代码创建控件并根据背包状态更新 */
UCLASS(Blueprintable)

class DELIVERY_API UDeliveryHotbarWidget : public UUserWidget
{
    GENERATED_BODY()

  public:
    UFUNCTION(BlueprintPure, Category = "Inventory")
    UDeliveryHotbarController *GetWidgetController() const
    {
        return WidgetController;
    }
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryHotbarController> WidgetController;
    // 背包绑定
    void SetInventory(UDeliveryInventoryComponent *InInventory);

  protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeConstruct() override;
    virtual void NativeTick(const FGeometry &MyGeometry, float InDeltaTime) override;

  private:
    // 界面构建与刷新
    void BuildWidgetTree();
    void Refresh();

    // 数据源与控件缓存
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryInventoryComponent> Inventory;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UBorder>> SlotBorders;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UImage>> SlotIcons;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UProgressBar>> DurabilityBars;

    /** 快捷栏的连贯背景贴图 */
    UPROPERTY(Transient)
    TObjectPtr<UTexture2D> ConnectedHotbarTexture;
};
