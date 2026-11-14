#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "DeliveryAiInspectorAssetTools.generated.h"

// 创建可编辑的 AI 展示面板，只修改专用 WBP，不改变玩法或模型配置
UCLASS()
class DELIVERYTUNING_API UDeliveryAiInspectorAssetTools : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="Delivery|Editor")
    static FString CreateAiInspectorWidget(bool bReplaceLayout = false);
};
