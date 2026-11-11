#pragma once

#include "Inventory/DeliveryHandheldItem.h"
#include "DeliveryCaterpillarItem.generated.h"

/** 可拾取的毛毛虫，沿用背包复制，由外卖的服务器交互消耗 */
UCLASS(Blueprintable)
class DELIVERY_API ADeliveryCaterpillarItem : public ADeliveryHandheldItem
{
    GENERATED_BODY()
public:
    ADeliveryCaterpillarItem();
};
