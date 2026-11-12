#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "DeliveryConversationAssetTools.generated.h"

// 创建可在 UMG 编辑器调整的交谈界面，发送与关闭操作沿用交谈 Widget
UCLASS()
class DELIVERYTUNING_API UDeliveryConversationAssetTools : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    // 默认保留已有布局；明确要求重建时，才替换这个界面的控件树
    UFUNCTION(BlueprintCallable, Category="Delivery|Editor")
    static FString CreateConversationWidget(bool bReplaceLayout=false);
};
