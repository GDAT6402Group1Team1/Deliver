#pragma once

#include "Task/Data/DeliveryTaskDefinition.h"
#include "UObject/Object.h"
#include "DeliveryWalletPaymentProbe.generated.h"

// 记录真实钱包的结算通知，比较服务器与远端收到的订单和原始明细
UCLASS(Transient, NotBlueprintable)
class UDeliveryWalletPaymentProbe : public UObject
{
    GENERATED_BODY()

public:
    int32 PaymentCount = 0;
    TWeakObjectPtr<UDeliveryTaskDefinition> LastTask;
    FDeliveryRewardBreakdown LastReward;

    UFUNCTION()
    void RecordPayment(UDeliveryTaskDefinition* Task, const FDeliveryRewardBreakdown& Reward)
    {
        ++PaymentCount;
        LastTask = Task;
        LastReward = Reward;
    }
};
