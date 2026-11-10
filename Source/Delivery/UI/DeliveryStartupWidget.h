#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Game/DeliveryStartupFlow.h"
#include "DeliveryStartupWidget.generated.h"

// 开始菜单的空布局基类，负责绑定世界服务，用户蓝图负责按钮与加载表现
UCLASS(Abstract, Blueprintable)
class DELIVERY_API UDeliveryStartupWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="Delivery|Startup")
    bool RequestStartGame();
    UFUNCTION(BlueprintCallable, Category="Delivery|Startup")
    bool RetryLoading();
    UFUNCTION(BlueprintPure, Category="Delivery|Startup")
    ADeliveryStartupFlow* GetStartupFlow() const { return Flow.Get(); }
    UFUNCTION(BlueprintCallable, Category="Delivery|Startup")
    void RefreshStartupState();
    // 返回当前本机估算进度，可直接绑定 Progress Bar 的 Percent
    UFUNCTION(BlueprintPure, Category="Delivery|Startup")
    float GetLoadingProgress() const;

    // 初次创建会同步当前状态，之后随状态变化调用，不需要 Widget Tick
    UFUNCTION(BlueprintImplementableEvent, Category="Delivery|Startup")
    void OnStartupStateChanged(EDeliveryStartupPhase Phase, bool bSceneReady,
        bool bStartRequested, const FText& LoadingError);
    // 初次构建同步一次，之后最多约每 0.1 秒随进度变化通知，数值范围 0~1
    UFUNCTION(BlueprintImplementableEvent, Category="Delivery|Startup")
    void OnLoadingProgressChanged(float Progress);
protected:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
private:
    UFUNCTION()
    void HandlePhaseChanged(EDeliveryStartupPhase Phase);
    UFUNCTION()
    void HandleLoadingProgressChanged(float Progress);
    TWeakObjectPtr<ADeliveryStartupFlow> Flow;
};
