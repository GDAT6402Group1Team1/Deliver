#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "DeliveryHotbarController.generated.h"

class UDeliveryInventoryComponent;
class UTexture2D;

USTRUCT(BlueprintType)
struct FDeliveryHotbarSlot
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) TObjectPtr<UTexture2D> Icon;
    UPROPERTY(BlueprintReadOnly) bool bOccupied = false;
    UPROPERTY(BlueprintReadOnly) bool bShowDurability = false;
    UPROPERTY(BlueprintReadOnly) float Durability = 0.f;
};

/** 将背包数据转换为界面数据，不修改物品状态 */
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryHotbarController : public UObject
{
    GENERATED_BODY()
  public:
    void SetInventory(UDeliveryInventoryComponent *InInventory);
    UFUNCTION(BlueprintPure, Category = "Inventory")
    FDeliveryHotbarSlot GetSlot(int32 Index) const;
    UFUNCTION(BlueprintPure, Category = "Inventory")
    float GetFullFeedback() const;
    UFUNCTION(BlueprintCallable, Category = "Inventory")
    void SelectSlot(int32 Index);

  private:
    TWeakObjectPtr<UDeliveryInventoryComponent> Inventory;
};
