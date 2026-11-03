#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Events/DeliveryEventComponent.h"
#include "DeliveryHintComponent.generated.h"

class APlayerController;
class APawn;
class UInputAction;
class UEnhancedInputLocalPlayerSubsystem;
class UDeliveryVehicleSummonComponent;

/**
 * 监听玩家控制对象、驾驶视角、车辆召唤冷却和按键映射的变化
 * 更新左下角操作提示，召唤冷却期间用计时器更新剩余秒数
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryHintComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryHintComponent();

    /** 输入组件就绪后连接映射变化通知 */
    void BindInput();
    void SetInputDevice(bool bGamepad);
    FText GetKeyText(const UInputAction* Action) const;

    /** 重新读取状态和按键，只在收到变化通知或倒计时更新时调用 */
    UFUNCTION(BlueprintCallable, Category="提示")
    void RefreshHint();

    UFUNCTION(BlueprintPure, Category="提示")
    FText GetHintText() const { return CurrentText; }

    UPROPERTY(EditDefaultsOnly, Category="提示")
    FText ReadyText;
    UPROPERTY(EditDefaultsOnly, Category="提示")
    FText CooldownText;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    UFUNCTION()
    void HandlePawnChanged(APawn* OldPawn, APawn* NewPawn);
    UFUNCTION()
    void HandleVehicleEvent(EDeliveryEvent Event);
    void HandleVehicleChanged(AActor* Actor);
    void UnbindPawn();
    void ShowHint(const FText& Text, bool bDimmed = false);

    UPROPERTY(Transient)
    TObjectPtr<APlayerController> Player;
    UPROPERTY(Transient)
    TObjectPtr<UEnhancedInputLocalPlayerSubsystem> Input;
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryVehicleSummonComponent> Summon;
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryEventComponent> VehicleEvents;

    FText CurrentText;
    bool bUsingGamepad = false;
    FTimerHandle CountdownTimer;
    FTimerHandle RefreshTimer;
    FDelegateHandle SpawnHandle;
    FDelegateHandle DestroyHandle;
};
