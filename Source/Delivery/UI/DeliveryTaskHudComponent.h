// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryTaskHudComponent.generated.h"

class UDeliveryTaskController;

/**
 * 根据电话和任务状态更新顶部提示，包括来电、取件、送达和结算
 * 组件挂在玩家控制器上，骑车时继续显示
 * 只在本机读取数据和更新界面，不修改任务状态
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryTaskHudComponent : public UActorComponent
{
    GENERATED_BODY()

  public:
    UDeliveryTaskHudComponent();

    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                               FActorComponentTickFunction *ThisTickFunction) override;

    // 提示配置
    /** 关掉就不显示任务条（想自己做 UMG 界面时用） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Task HUD")
    bool bShowObjectiveBar = true;

    /** 送达之后"送达！+120"停留多久 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Task HUD",
              meta = (ClampMin = "0.0", Units = "s"))
    float CompletionBannerSeconds = 4.f;

    /** 剩余时间少于这个值就把计时标红 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Task HUD",
              meta = (ClampMin = "0.0", Units = "s"))
    float UrgentRemainingSeconds = 60.f;

    UFUNCTION(BlueprintPure, Category = "Task")
    UDeliveryTaskController *GetWidgetController() const
    {
        return WidgetController;
    }

  protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

  private:
    UPROPERTY(Transient)
    TObjectPtr<class UDeliveryTaskController> WidgetController;
};
