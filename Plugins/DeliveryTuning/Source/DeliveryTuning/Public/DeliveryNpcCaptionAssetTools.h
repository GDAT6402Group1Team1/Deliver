#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "DeliveryNpcCaptionAssetTools.generated.h"

// 创建可编辑的居民字幕气泡，播放进度和位置沿用表现组件
UCLASS()
class DELIVERYTUNING_API UDeliveryNpcCaptionAssetTools : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    // 默认保留已有布局，明确重建时才替换控件树
    UFUNCTION(BlueprintCallable, Category="Delivery|Editor")
    static FString CreateNpcCaptionWidget(bool bReplaceLayout = false);
};
