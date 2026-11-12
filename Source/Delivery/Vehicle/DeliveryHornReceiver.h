#pragma once
#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "DeliveryHornReceiver.generated.h"

UINTERFACE(MinimalAPI)
class UDeliveryHornReceiver : public UInterface
{
    GENERATED_BODY()
};

/** 接收服务器鸣笛提示，接收者自行维护临时加速与恢复 */
class DELIVERY_API IDeliveryHornReceiver
{
    GENERATED_BODY()
public:
    virtual void ReceiveHorn(float SpeedMultiplier) = 0;
    virtual void ReceiveHornFrom(float Multiplier,AActor* Vehicle,AActor* Driver) { ReceiveHorn(Multiplier); }
};
