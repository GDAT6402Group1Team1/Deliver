#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "DeliveryHomeObjectAssetTools.generated.h"

class ADeliveryHome;
class UBlueprint;

// 家具资产只在编辑器世界组装，调用脚本负责备份、参数标定及显式保存
UCLASS()
class DELIVERYTUNING_API UDeliveryHomeObjectAssetTools : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    // 读真实组件、材质、坐标和用途，不创建占用、不调用 BeginPlay
    UFUNCTION(BlueprintCallable,Category="Delivery|Editor|Home")
    static FString InspectFurniture(AActor* Actor);
    // 只读检查拟定室内脚下点；临时预览门洞导航边后恢复原状态，不破门或重建导航
    UFUNCTION(BlueprintCallable,Category="Delivery|Editor|Home")
    static FString InspectInteriorAnchor(ADeliveryHome* Home,FVector ProposedWorldLocation);
    // JSON 保存明确的用途和局部坐标，返回本次创建的资源清单，不保存地图或资产
    // carry_scale_multiplier 默认 .35，restore_scale_from_support_bottom 默认 true，仅实物 TakeProp 使用
    UFUNCTION(BlueprintCallable,Category="Delivery|Editor|Home")
    static FString BuildFurnitureAssets(AActor* SourceActor,const FString& Configuration,bool bReplaceOwnedAssets=false);
    // 使用编辑器标准替换流程保留关卡引用；只接已经编译成功的独立家具蓝图
    UFUNCTION(BlueprintCallable,Category="Delivery|Editor|Home")
    static AActor* InstallFurniture(AActor* SourceActor,UBlueprint* Blueprint,ADeliveryHome* Home,FGuid ObjectId);
};
