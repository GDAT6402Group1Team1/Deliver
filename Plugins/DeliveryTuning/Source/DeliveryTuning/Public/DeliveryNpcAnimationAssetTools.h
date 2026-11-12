#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "DeliveryNpcAnimationAssetTools.generated.h"

// 在 NPC 独立网格上配置交谈动画图，保留已有动作和原始模型
UCLASS()
class DELIVERYTUNING_API UDeliveryNpcAnimationAssetTools : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable,Category="Delivery|Editor")
    static FString CreateConversationAnimation(bool bReplaceGraph=false);
};
