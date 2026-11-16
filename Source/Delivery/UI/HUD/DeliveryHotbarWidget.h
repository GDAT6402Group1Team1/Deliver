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

/**
 * @brief 屏幕底部的五格物品栏，由 HUD 创建
 *
 * 控件树在 RebuildWidget 里用代码建好（连贯背景、五个格子、图标、耐久条）
 * 显示的数据从 UDeliveryHotbarController 取；NativeTick 每帧调用 Refresh，耐久和背包满时的红色闪烁实时更新
 */
UCLASS(Blueprintable)

class DELIVERY_API UDeliveryHotbarWidget : public UUserWidget
{
    GENERATED_BODY()

  public:
    /// @brief 物品栏的界面处理器
    UFUNCTION(BlueprintPure, Category = "Inventory")
    UDeliveryHotbarController *GetWidgetController() const
    {
        return WidgetController;
    }

    /// 界面处理器，SetInventory 第一次调用时创建
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryHotbarController> WidgetController;

    /// @brief 换成显示这个背包，交给界面处理器，再调用 Refresh
    void SetInventory(UDeliveryInventoryComponent *InInventory);

  protected:
    // ── 生命周期 ──

    /// @brief 先调用 BuildWidgetTree 建好控件
    virtual TSharedRef<SWidget> RebuildWidget() override;

    /// @brief 控件建好后调用 Refresh
    virtual void NativeConstruct() override;

    /// @brief 每帧调用 Refresh
    virtual void NativeTick(const FGeometry &MyGeometry, float InDeltaTime) override;

  private:
    // ── 显示 ──

    /// @brief 建好外框、五个格子、图标和耐久条；已经建过时直接返回
    void BuildWidgetTree();

    /// @brief 从界面处理器取每格的图标、耐久和是否有东西，更新控件；背包满时其他四格闪红
    void Refresh();

    // ── 控件 ──

    /// 正在显示的背包
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryInventoryComponent> Inventory;

    /// 五个格子的边框，下标就是格子序号
    UPROPERTY(Transient)
    TArray<TObjectPtr<UBorder>> SlotBorders;

    /// 五个格子的图标
    UPROPERTY(Transient)
    TArray<TObjectPtr<UImage>> SlotIcons;

    /// 五个格子的耐久条
    UPROPERTY(Transient)
    TArray<TObjectPtr<UProgressBar>> DurabilityBars;

    /** 快捷栏的连贯背景贴图 */
    UPROPERTY(Transient)
    TObjectPtr<UTexture2D> ConnectedHotbarTexture;
};
