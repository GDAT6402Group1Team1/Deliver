#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "DeliverySpeechAssetTools.generated.h"

// 创建可编辑的本机语音状态界面和专用输入资产，不修改现有交谈界面布局
UCLASS()
class DELIVERYTUNING_API UDeliverySpeechAssetTools : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="Delivery|Editor")
    static FString CreateSpeechAssets(bool bReplaceLayout=false);
};
