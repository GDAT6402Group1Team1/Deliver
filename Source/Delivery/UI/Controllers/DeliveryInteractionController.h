#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "DeliveryInteractionController.generated.h"

class APawn;
class ADeliveryCharacter;
class UDeliveryInteractableComponent;

/**
 * @brief 交互浮窗的界面处理器：给出提示文字和长按进度；选哪个目标、能不能做由交互组件和服务器判断
 */
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryInteractionController : public UObject
{
    GENERATED_BODY()
  public:
    /// @brief 浮窗上的文字：E 类目标做不了时显示原因（例如骑车时不能领货），能做时显示快递或目标自己的提示
    /// @param Interactor 正在看提示的玩家角色，原因按他的状态算
    UFUNCTION(BlueprintPure, Category = "Interaction")
    FText GetPrompt(UDeliveryInteractableComponent *Target, bool bPickup,
                    const APawn *Interactor = nullptr) const;
    /// @brief 角色对 Target 的长按进度，0 到 1；没有角色时返回 -1
    UFUNCTION(BlueprintPure, Category = "Interaction")
    float GetProgress(const ADeliveryCharacter *Character, AActor *Target) const;
};
