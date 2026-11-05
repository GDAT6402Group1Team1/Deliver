#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "DeliveryInteractionController.generated.h"

class APawn;
class ADeliveryCharacter;
class UDeliveryInteractableComponent;

/** 提供交互提示与长按进度，不参与目标选择和服务器判断 */
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryInteractionController : public UObject
{
    GENERATED_BODY()
  public:
    UFUNCTION(BlueprintPure, Category = "Interaction")
    /** Interactor 用于取"这个人现在为什么按不动"的原因文字 */
    FText GetPrompt(UDeliveryInteractableComponent *Target, bool bPickup,
                    const APawn *Interactor = nullptr) const;
    UFUNCTION(BlueprintPure, Category = "Interaction")
    float GetProgress(const ADeliveryCharacter *Character, AActor *Target) const;
};
