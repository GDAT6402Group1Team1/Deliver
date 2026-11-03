#include "Carry/DeliveryCarryComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Vehicle/DeliveryMotorbike.h"

// 查询角色正在驾驶的车辆，排除单纯附着在车上的情况
ADeliveryMotorbike *UDeliveryCarryComponent::GetVehicle() const
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character)
        return nullptr;

    // 角色必须附着在车辆上，并且被车辆记录为驾驶员
    ADeliveryMotorbike *Bike = Cast<ADeliveryMotorbike>(Character->GetAttachParentActor());

    return (Bike && Bike->GetDriver() == Character) ? Bike : nullptr;
}
// 统一查询车上和手中的快递
ADeliveryPackageItem *UDeliveryCarryComponent::GetPackage() const
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    UDeliveryInventoryComponent *Inventory = Character ? Character->GetInventoryComponent() : nullptr;
    if (!Character)
        return nullptr;

    // 车上优先：骑车时快递在车上、手上是空的
    if (const ADeliveryMotorbike *Bike = GetVehicle())
    {
        if (ADeliveryPackageItem *Cargo = Bike->GetCargo())
        {
            return Cargo;
        }
    }

    return Inventory ? Cast<ADeliveryPackageItem>(Inventory->GetHeldItem()) : nullptr;
}
// 取得任务所属玩家，上车后必要时从车辆获取
APlayerState *UDeliveryCarryComponent::GetPlayerState() const
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character)
        return nullptr;

    if (APlayerState *Mine = Character->GetPlayerState())
    {
        return Mine;
    }

    // 骑车后控制器改为控制车辆，任务结算所需的玩家状态也从车辆取得
    const ADeliveryMotorbike *Bike = GetVehicle();

    return Bike ? Bike->GetPlayerState() : nullptr;
}
// 在服务器接收快递，骑车时装上货架，步行时交给背包
bool UDeliveryCarryComponent::TryReceivePackage(ADeliveryPackageItem *Package, bool bPlayFeedback)
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    UDeliveryInventoryComponent *Inventory = Character ? Character->GetInventoryComponent() : nullptr;
    if (!Character)
        return false;

    if (!Character->HasAuthority() || !IsValid(Package))
    {
        return false;
    }

    // 每名玩家只能携带一件任务快递，防止手上和车上同时出现不同快递
    if (GetPackage())
    {
        return false;
    }

    UDeliveryItemComponent *Item = Package->GetDeliveryItemComponent();

    if (ADeliveryMotorbike *Bike = GetVehicle())
    {
        if (!Bike->AttachCargo(Package))
        {
            return false;
        }

        // 先把快递放到车上，再更新任务；任务不允许接取时，将快递从车辆卸下
        if (Item && !Item->NotifyAcquired(GetPlayerState()))
        {
            Bike->ReleaseCargo();

            return false;
        }

        if (bPlayFeedback)
            Character->ClientPickupSucceeded();

        return true;
    }

    // 步行时由背包接收快递，背包会同时更新任务状态
    if (Inventory && Inventory->TryPickup(Package))
    {
        if (bPlayFeedback)
            Character->ClientPickupSucceeded();

        return true;
    }

    return false;
}

// 解除指定快递的携带关系，供交付成功后销毁
bool UDeliveryCarryComponent::ReleasePackage(ADeliveryPackageItem *ExpectedPackage)
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character || !Character->HasAuthority() || !IsValid(ExpectedPackage) ||
        GetPackage() != ExpectedPackage)
        return false;
    if (ADeliveryMotorbike *Vehicle = GetVehicle())
    {
        if (Vehicle->GetCargo() == ExpectedPackage)
        {
            Vehicle->ReleaseCargo();
            return true;
        }
    }
    UDeliveryInventoryComponent *Inventory = Character->GetInventoryComponent();
    return Inventory && Inventory->ConsumeHeldItem(ExpectedPackage);
}
