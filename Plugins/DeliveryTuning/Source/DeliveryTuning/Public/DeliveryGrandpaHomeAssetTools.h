#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "DeliveryGrandpaHomeAssetTools.generated.h"

class ADeliveryHome;

// 只接爷爷收件后的搬离分支，其余活动、模型和声音资产保持原配置
UCLASS()
class DELIVERYTUNING_API UDeliveryGrandpaHomeAssetTools : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable,Category="Delivery|Editor") static bool ConfigureMoveOutBehavior();
    UFUNCTION(BlueprintCallable,Category="Delivery|Editor") static bool InspectMoveOutBehavior();
    // 显式重建当前编辑器地图导航，调用者负责备份和保存地图
    UFUNCTION(BlueprintCallable,Category="Delivery|Editor") static bool BakeDoorwayNavigation(ADeliveryHome* Home);
    // 临时切换导航边后恢复，不破门、不移动角色、不执行游戏逻辑
    UFUNCTION(BlueprintCallable,Category="Delivery|Editor") static FString InspectDoorwayNavigation(ADeliveryHome* Home);
};
