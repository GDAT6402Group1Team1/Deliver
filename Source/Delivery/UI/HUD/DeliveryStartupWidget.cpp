#include "UI/HUD/DeliveryStartupWidget.h"

// ── 生命周期 ──

/// @brief 找到开局流程并订阅阶段和进度变化，然后各调用一次两个蓝图事件
void UDeliveryStartupWidget::NativeConstruct()
{
    Flow = ADeliveryStartupFlow::GetDeliveryStartupFlow(this);
    if (Flow.IsValid())
    {
        Flow->OnPhaseChanged.AddUniqueDynamic(this, &UDeliveryStartupWidget::HandlePhaseChanged);
        Flow->OnLoadingProgressChanged.AddUniqueDynamic(this, &UDeliveryStartupWidget::HandleLoadingProgressChanged);
    }
    Super::NativeConstruct();
    RefreshStartupState();
    OnLoadingProgressChanged(GetLoadingProgress());
}

/// @brief 取消订阅
void UDeliveryStartupWidget::NativeDestruct()
{
    if (Flow.IsValid())
    {
        Flow->OnPhaseChanged.RemoveDynamic(this, &UDeliveryStartupWidget::HandlePhaseChanged);
        Flow->OnLoadingProgressChanged.RemoveDynamic(this, &UDeliveryStartupWidget::HandleLoadingProgressChanged);
    }
    Flow.Reset();
    Super::NativeDestruct();
}

/// @brief 请求开始游戏，转给开局流程的 RequestStartGame；返回请求是否被接受，接受不等于已经开局
bool UDeliveryStartupWidget::RequestStartGame()
{
    const bool bAccepted = Flow.IsValid() && Flow->RequestStartGame(GetOwningPlayer());
    RefreshStartupState();
    return bAccepted;
}

/// @brief 加载失败后重试，转给开局流程的 RetryLoading
bool UDeliveryStartupWidget::RetryLoading()
{
    const bool bAccepted = Flow.IsValid() && Flow->RetryLoading();
    RefreshStartupState();
    return bAccepted;
}

/// @brief 阶段变化时调用 RefreshStartupState
void UDeliveryStartupWidget::HandlePhaseChanged(EDeliveryStartupPhase Phase)
{
    RefreshStartupState();
}

/// @brief 读开局流程的当前状态，调用蓝图事件 OnStartupStateChanged
void UDeliveryStartupWidget::RefreshStartupState()
{
    if (Flow.IsValid()) OnStartupStateChanged(Flow->GetPhase(), Flow->IsSceneReady(),
        Flow->IsStartRequested(), Flow->GetLoadingError());
}

/// @brief 本机估算的加载进度（0 到 1），可以直接绑到进度条的 Percent
float UDeliveryStartupWidget::GetLoadingProgress() const
{
    return Flow.IsValid() ? Flow->GetLoadingProgress() : 0.f;
}

/// @brief 进度变化时调用蓝图事件 OnLoadingProgressChanged
void UDeliveryStartupWidget::HandleLoadingProgressChanged(float Progress)
{
    OnLoadingProgressChanged(Progress);
}
