#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "DeliveryPackageTransferProbe.generated.h"

class ADeliveryCharacter;
class ADeliveryPackageItem;

// 回归夹具在真实背包通知中尝试抢件并移动接收者，检查同步交接的预约与回滚
UCLASS(Transient, NotBlueprintable)
class UDeliveryPackageTransferProbe : public UObject
{
    GENERATED_BODY()
public:
    TWeakObjectPtr<ADeliveryCharacter> ThirdParty;
    TWeakObjectPtr<ADeliveryPackageItem> Package;
    TWeakObjectPtr<AActor> Receiver;
    bool bAttempted = false;
    bool bThirdPartyReceived = false;
    UFUNCTION() void OnInventoryChanged();
};
