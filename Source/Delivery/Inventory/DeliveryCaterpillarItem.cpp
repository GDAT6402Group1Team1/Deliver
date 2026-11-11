#include "Inventory/DeliveryCaterpillarItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"

ADeliveryCaterpillarItem::ADeliveryCaterpillarItem()
{
    ItemComponent->DisplayName = NSLOCTEXT("DeliveryItems", "Caterpillar", "Caterpillar");
    ItemComponent->bUsesDurability = false;
    InteractableComponent->PromptText = NSLOCTEXT("DeliveryItems", "PickupCaterpillar", "[E] Pick up caterpillar - hold it near food");
    HeldRelativeTransform = FTransform(FRotator::ZeroRotator, FVector(5,0,0));
}
