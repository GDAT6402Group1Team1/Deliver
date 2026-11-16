#include "Inventory/Items/DeliveryCaterpillarItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"

ADeliveryCaterpillarItem::ADeliveryCaterpillarItem()
{
    ItemComponent->DisplayName = NSLOCTEXT("DeliveryItems", "Caterpillar", "毛毛虫");
    ItemComponent->bUsesDurability = false;
    ItemComponent->bCompanionCanFetch = true;
    bUseNpcGroundPickupPose = true;
    InteractableComponent->PromptText = NSLOCTEXT("DeliveryItems", "PickupCaterpillar", "[E] 拾取毛毛虫，拿在手上靠近外卖");
    HeldRelativeTransform = FTransform(FRotator::ZeroRotator, FVector(5,0,0));
}
