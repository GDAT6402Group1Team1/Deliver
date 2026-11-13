#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "DeliveryNpcAnimationAssetTools.generated.h"

// 创建 NPC 的主动画图与交谈后处理图，资源保存在编辑器中供继续调整
UCLASS()
class DELIVERYTUNING_API UDeliveryNpcAnimationAssetTools : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable,Category="Delivery|Editor")
    static FString CreateConversationAnimation(bool bReplaceGraph=false);
    // 默认保留已有主图；明确要求重建时才重新生成节点
    UFUNCTION(BlueprintCallable,Category="Delivery|Editor")
    static FString CreateBodyAnimation(bool bReplaceGraph=false);
    // 检查保存的主图输入和受击时间连接，不修改资源
    UFUNCTION(BlueprintCallable,Category="Delivery|Editor")
    static FString CheckBodyAnimation();
};
