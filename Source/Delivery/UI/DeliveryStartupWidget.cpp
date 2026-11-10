#include "UI/DeliveryStartupWidget.h"

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

bool UDeliveryStartupWidget::RequestStartGame()
{
    const bool bAccepted = Flow.IsValid() && Flow->RequestStartGame(GetOwningPlayer());
    RefreshStartupState();
    return bAccepted;
}

bool UDeliveryStartupWidget::RetryLoading()
{
    const bool bAccepted = Flow.IsValid() && Flow->RetryLoading();
    RefreshStartupState();
    return bAccepted;
}

void UDeliveryStartupWidget::HandlePhaseChanged(EDeliveryStartupPhase Phase)
{
    RefreshStartupState();
}

void UDeliveryStartupWidget::RefreshStartupState()
{
    if (Flow.IsValid()) OnStartupStateChanged(Flow->GetPhase(), Flow->IsSceneReady(),
        Flow->IsStartRequested(), Flow->GetLoadingError());
}

float UDeliveryStartupWidget::GetLoadingProgress() const
{
    return Flow.IsValid() ? Flow->GetLoadingProgress() : 0.f;
}

void UDeliveryStartupWidget::HandleLoadingProgressChanged(float Progress)
{
    OnLoadingProgressChanged(Progress);
}
