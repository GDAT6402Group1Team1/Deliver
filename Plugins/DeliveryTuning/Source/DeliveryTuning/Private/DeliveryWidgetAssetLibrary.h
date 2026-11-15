#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "DeliveryWidgetAssetLibrary.generated.h"

class UWidget;
class UWidgetBlueprint;

// 为编辑器脚本添加的 UMG 控件登记蓝图变量标识
UCLASS()
class UDeliveryWidgetAssetLibrary final : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "Delivery|Editor")
    static bool RegisterWidget(UWidgetBlueprint* Blueprint, UWidget* Widget);
};
