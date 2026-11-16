#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Game/DeliveryStartupFlow.h"
#include "DeliveryStartupWidget.generated.h"

/**
 * @brief 开始菜单 WBP 的父类，只有空布局；按钮和加载表现在蓝图里做
 *
 * NativeConstruct 找到场景里的 ADeliveryStartupFlow，订阅它的阶段和加载进度变化，变化时调用下面两个蓝图事件
 * 蓝图的开始按钮调用 RequestStartGame，重试按钮调用 RetryLoading
 */
UCLASS(Abstract, Blueprintable)
class DELIVERY_API UDeliveryStartupWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    /// @brief 请求开始游戏，转给开局流程的 RequestStartGame；返回请求是否被接受，接受不等于已经开局
    UFUNCTION(BlueprintCallable, Category="Delivery|Startup")
    bool RequestStartGame();

    /// @brief 加载失败后重试，转给开局流程的 RetryLoading
    UFUNCTION(BlueprintCallable, Category="Delivery|Startup")
    bool RetryLoading();

    /// @brief 场景里的开局流程
    UFUNCTION(BlueprintPure, Category="Delivery|Startup")
    ADeliveryStartupFlow* GetStartupFlow() const { return Flow.Get(); }

    /// @brief 读开局流程的当前状态，调用蓝图事件 OnStartupStateChanged
    UFUNCTION(BlueprintCallable, Category="Delivery|Startup")
    void RefreshStartupState();

    /// @brief 本机估算的加载进度（0 到 1），可以直接绑到进度条的 Percent
    UFUNCTION(BlueprintPure, Category="Delivery|Startup")
    float GetLoadingProgress() const;

    /// @brief 蓝图事件：控件建好时调用一次，之后阶段变化时调用
    UFUNCTION(BlueprintImplementableEvent, Category="Delivery|Startup")
    void OnStartupStateChanged(EDeliveryStartupPhase Phase, bool bSceneReady,
        bool bStartRequested, const FText& LoadingError);

    /// @brief 蓝图事件：控件建好时调用一次，之后进度变化时调用，最多约每 0.1 秒一次，Progress 为 0 到 1
    UFUNCTION(BlueprintImplementableEvent, Category="Delivery|Startup")
    void OnLoadingProgressChanged(float Progress);
protected:
    // ── 生命周期 ──

    /// @brief 找到开局流程并订阅阶段和进度变化，然后各调用一次两个蓝图事件
    virtual void NativeConstruct() override;

    /// @brief 取消订阅
    virtual void NativeDestruct() override;
private:
    /// @brief 阶段变化时调用 RefreshStartupState
    UFUNCTION()
    void HandlePhaseChanged(EDeliveryStartupPhase Phase);

    /// @brief 进度变化时调用蓝图事件 OnLoadingProgressChanged
    UFUNCTION()
    void HandleLoadingProgressChanged(float Progress);

    /// 场景里的开局流程
    TWeakObjectPtr<ADeliveryStartupFlow> Flow;
};
