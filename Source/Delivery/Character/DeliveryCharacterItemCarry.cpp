#include "Character/DeliveryCharacter.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/Items/DeliveryHandheldItem.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/PlayerController.h"

ADeliveryHandheldItem* ADeliveryCharacter::GetCarriedHandheldItem() const
{
    auto* Inventory=GetInventoryComponent();
    auto* Item=Inventory?Inventory->GetHeldItem():nullptr;
    return OwnsHandheldItem(Item)?Item:nullptr;
}
APlayerState* ADeliveryCharacter::GetHandheldOwnerPlayerState() const { return GetTaskPlayerState(); }
bool ADeliveryCharacter::CanHandleHandheldItem() const
{
    const auto* Player=GetHandheldOwnerPlayerState();
    const auto* PC=IsValid(Player)?Player->GetPlayerController():nullptr;
    return CanHandleTaskPackage() && IsValid(PC) && PC->GetPawn()==this && GetController()==PC;
}
bool ADeliveryCharacter::OwnsHandheldItem(const ADeliveryHandheldItem* Item) const
{
    return GetInventoryComponent() && GetInventoryComponent()->ContainsItem(Item);
}
bool ADeliveryCharacter::CanReceiveHandheldItem(ADeliveryHandheldItem* Item) const
{
    return CanHandleHandheldItem() && GetInventoryComponent() && GetInventoryComponent()->CanReceiveFetchedItem(Item);
}
bool ADeliveryCharacter::TryReceiveHandheldItem(ADeliveryHandheldItem* Item)
{
    if (!HasAuthority() || !CanReceiveHandheldItem(Item) || Item->GetOwner()) return false;
    const TWeakObjectPtr<ADeliveryCharacter> WeakThis(this);
    const TWeakObjectPtr<ADeliveryHandheldItem> WeakItem(Item);
    const bool bReceived=GetInventoryComponent()->TryPickup(Item);
    return WeakThis.IsValid() && WeakItem.IsValid() && bReceived && OwnsHandheldItem(Item);
}
bool ADeliveryCharacter::ReleaseHandheldItem(ADeliveryHandheldItem* Item)
{
    return HasAuthority() && GetInventoryComponent() && GetInventoryComponent()->ReleaseFetchedItem(Item);
}
void ADeliveryCharacter::ApplyHandheldItemPresentation(ADeliveryHandheldItem* Item)
{
    if (GetInventoryComponent()) GetInventoryComponent()->RefreshItemPresentation(Item);
}
