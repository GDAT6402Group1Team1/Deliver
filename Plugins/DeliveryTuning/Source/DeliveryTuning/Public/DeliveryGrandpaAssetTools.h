#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "DeliveryGrandpaAssetTools.generated.h"

/** 在独立目录生成可编辑的爷爷行为树及蓝图任务，不改现有角色资产 */
UCLASS()
class DELIVERYTUNING_API UDeliveryGrandpaAssetTools : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="Delivery|Editor")
    static bool CreateBehaviorAssets();
    UFUNCTION(BlueprintCallable, Category="Delivery|Editor")
    static bool UpgradeExpressiveBehavior();
};
