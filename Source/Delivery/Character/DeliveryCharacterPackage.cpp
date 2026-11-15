#include "Character/DeliveryCharacter.h"
#include "Carry/DeliveryCarryComponent.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Task/World/DeliveryItemComponent.h"
#include "GameFramework/PlayerState.h"

bool ADeliveryCharacter::CanHandleTaskPackage() const
{
    const auto* Player = GetTaskPlayerState();
    return !IsIncapacitated() && !IsRidingVehicle() && IsValid(Player) &&
        Player->GetWorld() == GetWorld() && !Player->IsInactive();
}

bool ADeliveryCharacter::CanReceiveTaskPackage(ADeliveryPackageItem* Package) const
{
    const auto* Inventory = GetInventoryComponent();
    const auto* Item = IsValid(Package) ? Package->GetDeliveryItemComponent() : nullptr;
    return CanHandleTaskPackage() && Inventory && !Inventory->HasHeldItem() && !Inventory->GetStoredTaskPackage() &&
        Package && Package->GetWorld() == GetWorld() && Item && Item->CanBeAcquiredBy(this);
}

bool ADeliveryCharacter::CanReceiveTaskPackageInBackpack(ADeliveryPackageItem* Package) const
{
    const auto* Inventory = GetInventoryComponent();
    const auto* Item = IsValid(Package) ? Package->GetDeliveryItemComponent() : nullptr;
    return CanHandleTaskPackage() && Inventory && Inventory->CanStoreTaskPackageInBackpack() &&
        Item && Package->GetWorld() == GetWorld() && Item->CanBeAcquiredBy(this);
}

bool ADeliveryCharacter::TryReceiveTaskPackageInBackpack(ADeliveryPackageItem* Package)
{
    return HasAuthority() && CanReceiveTaskPackageInBackpack(Package) && !Package->GetOwner() &&
        GetInventoryComponent()->TryStoreTaskPackageInBackpack(Package) && IsTaskPackageInBackpack(Package);
}

bool ADeliveryCharacter::IsTaskPackageInBackpack(const ADeliveryPackageItem* Package) const
{
    const auto* Inventory = GetInventoryComponent();
    return Inventory && Inventory->IsTaskPackageInBackpack(Package);
}

bool ADeliveryCharacter::TryReceiveTaskPackage(ADeliveryPackageItem* Package)
{
    if (!HasAuthority() || !CanReceiveTaskPackage(Package) || Package->GetOwner()) return false;
    return CarryComponent->TryReceivePackage(Package, false) &&
        GetCarriedTaskPackage() == Package && Package->GetOwner() == this;
}

bool ADeliveryCharacter::ReleaseTaskPackage(ADeliveryPackageItem* Package)
{
    return HasAuthority() && IsValid(Package) && Package->GetOwner() == this &&
        CarryComponent->ReleasePackage(Package);
}

void ADeliveryCharacter::ApplyTaskPackagePresentation(ADeliveryPackageItem* Package)
{
    if (IsValid(Package) && Package->GetOwner() == this)
        GetInventoryComponent()->RefreshItemPresentation(Package);
}
