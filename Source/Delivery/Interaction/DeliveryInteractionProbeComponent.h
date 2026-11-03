// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryInteractionProbeComponent.generated.h"

class UDeliveryInteractableComponent;
class UPrimitiveComponent;

/**
 * 在本机查找角色附近的交互目标，并更新提示和高亮
 * 拾取目标优先选择准星附近的物体，其次选择最近的物体
 * 实际操作仍由服务器检查和执行
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryInteractionProbeComponent : public UActorComponent
{
    GENERATED_BODY()

  public:
    UDeliveryInteractionProbeComponent();

    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                               FActorComponentTickFunction *ThisTickFunction) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    // 当前交互目标
    /** 当前瞄到的可交互物；没有就是 空指针 */
    UDeliveryInteractableComponent *GetFocused() const
    {
        return FocusedGeneral.Get();
    }

    UFUNCTION(BlueprintPure, Category = "Interaction")
    AActor *GetFocusedActor() const;

    /** 当前范围内的 E 目标；与 F 目标分开保存，不会改动 F 绑定 */
    UFUNCTION(BlueprintPure, Category = "Interaction")
    AActor *GetFocusedPickupActor() const;

  protected:
    // 提示开关
    /** 关掉就只探测不显示浮窗，留给以后想用 UMG 自己画提示的情况 */
    UPROPERTY(EditAnywhere, Category = "Interaction")
    bool bShowPrompt = true;

  private:
    UPROPERTY(Transient)
    TObjectPtr<class UDeliveryInteractionController> WidgetController;

    // 目标选择与高亮恢复
    UDeliveryInteractableComponent *FindPreferredPickup(const APawn *Owner) const;
    void SetPickupHighlight(AActor *Actor);
    void RestorePickupHighlight();

    struct FPrimitiveHighlightState
    {
        TWeakObjectPtr<UPrimitiveComponent> Component;
        bool bRenderCustomDepth = false;
        int32 StencilValue = 0;
    };

    // 本机目标缓存
    TWeakObjectPtr<UDeliveryInteractableComponent> FocusedGeneral;
    TWeakObjectPtr<UDeliveryInteractableComponent> FocusedPickup;
    TWeakObjectPtr<AActor> HighlightedActor;
    TArray<FPrimitiveHighlightState> HighlightStates;
};
