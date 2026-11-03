#include "UI/Controllers/DeliveryHotbarController.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Engine/Texture2D.h"

// 替换界面读取的背包，不保存额外的槽位副本
void UDeliveryHotbarController::SetInventory(UDeliveryInventoryComponent *InInventory)
{
    Inventory = InInventory;
}

// 将指定物品转换为图标、占用状态和耐久比例
FDeliveryHotbarSlot UDeliveryHotbarController::GetSlot(int32 Index) const
{
    FDeliveryHotbarSlot Result;
    if (!Inventory.IsValid())
        return Result;
    const ADeliveryHandheldItem *Item = Inventory->GetItemInSlot(Index);
    const UDeliveryInventoryItemComponent *Data = Item ? Item->GetItemComponent() : nullptr;
    if (Data)
    {
        Result.Icon = Data->Icon;
        Result.bOccupied = true;
        Result.bShowDurability = Data->UsesDurability();
        Result.Durability = Data->GetDurabilityFraction();
    }
    return Result;
}

// 读取背包已满提示当前的闪烁强度
float UDeliveryHotbarController::GetFullFeedback() const
{
    return Inventory.IsValid() ? Inventory->GetFullFeedbackAlpha() : 0.f;
}

// 把界面槽位操作交给背包自己的请求入口
void UDeliveryHotbarController::SelectSlot(int32 Index)
{
    if (Inventory.IsValid())
        Inventory->RequestSlotAction(Index);
}
