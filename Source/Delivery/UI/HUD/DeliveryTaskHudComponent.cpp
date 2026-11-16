#include "UI/HUD/DeliveryTaskHudComponent.h"
#include "UI/Controllers/DeliveryTaskController.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "GameFramework/PlayerController.h"

// ── 生命周期 ──

/// @brief 每 0.05 秒更新一次，不同步
UDeliveryTaskHudComponent::UDeliveryTaskHudComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickInterval = 0.05f;
    SetIsReplicatedByDefault(false);
}

/// @brief 只为本机玩家创建任务界面控制器
void UDeliveryTaskHudComponent::BeginPlay()
{
    Super::BeginPlay();
    APlayerController *Player = Cast<APlayerController>(GetOwner());
    if (Player && Player->IsLocalController())
    {
        WidgetController = NewObject<UDeliveryTaskController>(this);
        WidgetController->Initialize(Player);
    }
}

/// @brief 关闭界面控制器并解除事件订阅
void UDeliveryTaskHudComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (WidgetController)
        WidgetController->Shutdown();
    Super::EndPlay(EndPlayReason);
}

/// @brief 读取控制器生成的内容，交给现有任务提示控件显示
void UDeliveryTaskHudComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                              FActorComponentTickFunction *ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!WidgetController)
        return;
    WidgetController->Update(CompletionBannerSeconds, UrgentRemainingSeconds);
    if (!bShowObjectiveBar || !WidgetController->bVisible)
        return;
    if (UDeliveryPromptSubsystem *Prompt = UDeliveryPromptSubsystem::Get(this))
    {
        Prompt->PushObjective(WidgetController->Title, WidgetController->Detail, WidgetController->bUrgent);
    }
}
