#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "DeliveryHotbarController.generated.h"

class UDeliveryInventoryComponent;
class UTexture2D;

/// @brief 物品栏一个格子要显示的内容
USTRUCT(BlueprintType)
struct FDeliveryHotbarSlot
{
    GENERATED_BODY()

    /// 物品图标，空格为空
    UPROPERTY(BlueprintReadOnly) TObjectPtr<UTexture2D> Icon;

    /// 格子里有东西
    UPROPERTY(BlueprintReadOnly) bool bOccupied = false;

    /// 这件物品有耐久，要显示耐久条
    UPROPERTY(BlueprintReadOnly) bool bShowDurability = false;

    /// 剩余耐久，0 到 1
    UPROPERTY(BlueprintReadOnly) float Durability = 0.f;
};

/**
 * @brief 物品栏的界面处理器：把背包里的物品换成格子要显示的内容，不改物品
 */
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryHotbarController : public UObject
{
    GENERATED_BODY()
  public:
    /// @brief 换成读这个背包
    void SetInventory(UDeliveryInventoryComponent *InInventory);

    /// @brief 第 Index 格要显示的图标、耐久和是否有东西
    UFUNCTION(BlueprintPure, Category = "Inventory")
    FDeliveryHotbarSlot GetSlot(int32 Index) const;

    /// @brief 背包满时红色闪烁的强度，0 到 1
    UFUNCTION(BlueprintPure, Category = "Inventory")
    float GetFullFeedback() const;

    /// @brief 点了第 Index 格，调用背包的 RequestSlotAction
    UFUNCTION(BlueprintCallable, Category = "Inventory")
    void SelectSlot(int32 Index);

  private:
    /// 正在读的背包
    TWeakObjectPtr<UDeliveryInventoryComponent> Inventory;
};
