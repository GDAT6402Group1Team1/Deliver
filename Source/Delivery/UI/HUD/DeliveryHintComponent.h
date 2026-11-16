#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Events/DeliveryEventComponent.h"
#include "DeliveryHintComponent.generated.h"

class ADeliveryCompanionEncounterScene;
class APlayerController;
class APawn;
class USceneComponent;
class UInputAction;
class UEnhancedInputLocalPlayerSubsystem;
class UDeliveryVehicleSummonComponent;

/**
 * @brief 屏幕左下角的一行操作提示，挂在玩家控制器上，只在本机运行
 *
 * 步行时显示召唤摩托车的键或剩余冷却，骑车时显示切换视角的键和当前视角，靠近被挡住的二狗初遇场景时提示让开
 * 不每帧更新：控制对象换了、召唤冷却变了、车辆视角变了、按键映射重建了、换了输入设备、地图上的车增减了，才调用 RefreshHint
 * 冷却期间用计时器在剩余秒数变化时再刷新；显示交给 DeliveryPromptSubsystem 的 SetCornerHint
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryHintComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    // ── 生命周期 ──

    /// @brief 不需要每帧更新；设置召唤的默认提示文字
    UDeliveryHintComponent();

    // ── 刷新 ──

    /// @brief 本机：第一次调用时订阅场景里所有二狗初遇场景；订阅 Enhanced Input 的映射重建通知，然后调用 RefreshHint
    void BindInput();

    /// @brief 玩家换了输入设备（手柄或键鼠），换了就调用 RefreshHint 显示对应的键名
    void SetInputDevice(bool bGamepad);

    /// @brief Action 在当前设备上映射的键名；没有映射时返回空
    FText GetKeyText(const UInputAction* Action) const;

    /// @brief 按当前状态重新算提示文字：初遇提示优先，其次骑车的视角提示，最后步行的召唤提示
    UFUNCTION(BlueprintCallable, Category="提示")
    void RefreshHint();

    /// @brief 当前显示的提示文字
    UFUNCTION(BlueprintPure, Category="提示")
    FText GetHintText() const { return CurrentText; }

    // ── 配置 ──

    /// 可以召唤时的提示，{0} 换成按键名
    UPROPERTY(EditDefaultsOnly, Category="提示")
    FText ReadyText;

    /// 冷却中的提示，{0} 换成剩余秒数
    UPROPERTY(EditDefaultsOnly, Category="提示")
    FText CooldownText;

protected:
    // ── 生命周期 ──

    /// @brief 订阅控制对象变化和 Actor 生成、销毁，调用 BindInput，再按当前控制对象调用 HandlePawnChanged
    virtual void BeginPlay() override;

    /// @brief 取消所有订阅和计时器，清掉左下角提示
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    // ── 通知 ──

    /// @brief 控制对象换了：取消旧对象的订阅；新对象是角色时订阅召唤组件的冷却变化，是摩托车时订阅车辆事件
    UFUNCTION()
    void HandlePawnChanged(APawn* OldPawn, APawn* NewPawn);

    /// @brief 车辆事件：切换视角 CameraChanged 时调用 RefreshHint
    UFUNCTION()
    void HandleVehicleEvent(EDeliveryEvent Event);

    /// @brief 初遇场景被销毁时取消订阅，再调用 UpdateEncounterHint
    UFUNCTION()
    void HandleEncounterEndPlay(AActor* Actor, EEndPlayReason::Type Reason);

    /// @brief 生成了初遇场景就订阅它；生成了摩托车就调用 HandleVehicleChanged
    void HandleActorSpawned(AActor* Actor);

    /// @brief 地图上的摩托车生成或销毁时，下一帧调用 RefreshHint（召唤能不能用和场上有没有车有关）
    void HandleVehicleChanged(AActor* Actor);

    // ── 二狗初遇 ──

    /// @brief 订阅初遇场景的受阻变化和销毁
    void BindEncounterScene(ADeliveryCompanionEncounterScene* Scene);

    /// @brief 订阅玩家身体的位置变化，玩家走动时调用 UpdateEncounterHint；传入空时取消订阅
    void BindEncounterPawnRoot(USceneComponent* Root);

    /// @brief 找到 800 cm 内最近的、被挡住的初遇场景：挡着门口提示让开门口，挡着弹出路线提示留出空地
    /// 只有存在受阻场景时才订阅玩家位置；提示变了才调用 RefreshHint
    void UpdateEncounterHint();

    // ── 通知 ──

    /// @brief 取消对旧控制对象（召唤组件、车辆事件、身体位置）的订阅
    void UnbindPawn();

    // ── 刷新 ──

    /// @brief 记下文字，交给 DeliveryPromptSubsystem 显示；文字为空时清掉提示；bDimmed 为 true 时显示成灰色（冷却中）
    void ShowHint(const FText& Text, bool bDimmed = false);

    // ── 状态 ──

    /// 拥有这个组件的玩家控制器
    UPROPERTY(Transient)
    TObjectPtr<APlayerController> Player;

    /// 本机 Enhanced Input 子系统，查按键映射用
    UPROPERTY(Transient)
    TObjectPtr<UEnhancedInputLocalPlayerSubsystem> Input;

    /// 当前角色的召唤组件
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryVehicleSummonComponent> Summon;

    /// 当前骑的摩托车上的事件组件
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryEventComponent> VehicleEvents;

    /// 当前显示的文字
    FText CurrentText;

    /// 初遇场景的提示；非空时优先显示
    FText EncounterHint;

    /// 订阅了的初遇场景
    TArray<TWeakObjectPtr<ADeliveryCompanionEncounterScene>> EncounterScenes;

    /// 订阅了位置变化的玩家身体
    TWeakObjectPtr<USceneComponent> EncounterPawnRoot;
    FDelegateHandle EncounterPawnTransformHandle;

    /// 是否已经找过场景里已有的初遇场景
    bool bEncounterScenesInitialized = false;

    /// 当前用的是手柄
    bool bUsingGamepad = false;

    /// 冷却倒计时，到时调用 RefreshHint
    FTimerHandle CountdownTimer;

    /// 车辆增减后下一帧调用 RefreshHint
    FTimerHandle RefreshTimer;

    /// Actor 生成和销毁通知的订阅句柄
    FDelegateHandle SpawnHandle;
    FDelegateHandle DestroyHandle;
};
