// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryTaskHudComponent.generated.h"

class UDeliveryTaskController;

/**
 * @brief 屏幕顶部的任务提示条：来电、去取件、去送货、送达奖励
 *
 * 挂在玩家控制器上，骑车时也在；只在本机创建 UDeliveryTaskController
 * 每 0.05 秒调用处理器的 Update 算出标题和说明，再交给 DeliveryPromptSubsystem 的 PushObjective 显示；不改任务状态
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryTaskHudComponent : public UActorComponent
{
    GENERATED_BODY()

  public:
    // ── 生命周期 ──

    /// @brief 每 0.05 秒更新一次，不同步
    UDeliveryTaskHudComponent();

    /// @brief 调用处理器的 Update，有内容要显示时调用 PushObjective
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                               FActorComponentTickFunction *ThisTickFunction) override;

    // ── 配置 ──
    /** 关掉就不显示任务条（想自己做 UMG 界面时用） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Task HUD")
    bool bShowObjectiveBar = true;

    /** 送达之后显示奖励金额的那条提示停留多久 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Task HUD",
              meta = (ClampMin = "0.0", Units = "s"))
    float CompletionBannerSeconds = 4.f;

    /** 剩余时间少于这个值就把计时标红 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Task HUD",
              meta = (ClampMin = "0.0", Units = "s"))
    float UrgentRemainingSeconds = 60.f;

    /// @brief 任务提示的界面处理器
    UFUNCTION(BlueprintPure, Category = "Task")
    UDeliveryTaskController *GetWidgetController() const
    {
        return WidgetController;
    }

  protected:
    /// @brief 本机控制器上创建界面处理器并调用 Initialize
    virtual void BeginPlay() override;

    /// @brief 调用处理器的 Shutdown 取消订阅
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

  private:
    /// 任务提示的界面处理器
    UPROPERTY(Transient)
    TObjectPtr<class UDeliveryTaskController> WidgetController;
};
