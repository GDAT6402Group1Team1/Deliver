#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "DeliveryCompanionAnimationAssetTools.generated.h"

// 复制二狗当前主动画图，只在独立资源中增加家居混合并绑定二狗蓝图默认值
UCLASS()
class DELIVERYTUNING_API UDeliveryCompanionAnimationAssetTools : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    // 参数必须是已导入的成品动画，本工具不生成骨轨道；重复调用保留原图，不保存地图
    UFUNCTION(BlueprintCallable,Category="Delivery|Editor|Companion")
    static FString SetupCompanionBodyAnimation(const FString& SitDownPath,const FString& SitPath,
        const FString& StandUpPath,const FString& CarryPath);
    // 返回图连接、骨架与蓝图默认绑定的只读 JSON
    UFUNCTION(BlueprintCallable,Category="Delivery|Editor|Companion")
    static FString InspectCompanionBodyAnimation();
    // 在无 BeginPlay 的 EditorPreview 世界求值正式动画图，结果仅证明姿势与混合，不代替游戏验收
    UFUNCTION(BlueprintCallable,Category="Delivery|Editor|Companion")
    static FString ValidateCompanionBodyAnimation(const FString& SitDownPath,const FString& SitPath,
        const FString& StandUpPath,const FString& CarryPath,const FString& PickUpPath=TEXT(""));
};
