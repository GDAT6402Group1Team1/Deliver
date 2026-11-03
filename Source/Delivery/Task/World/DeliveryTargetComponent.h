// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryTargetComponent.generated.h"

class APawn;
class APlayerState;
class UDeliveryTaskDefinition;

/**
 * 挂在收件人（NPC 或投递点）身上，声明"这里收哪个任务的快递"
 * 交互系统在玩家按下交付键时调 TryDeliver，交付成功由本组件销毁快递 Actor
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryTargetComponent : public UActorComponent
{
    GENERATED_BODY()

  public:
    // 长按交互的目标实现
    bool CanDeliver(APawn *Interactor, bool bCheckTaskAvailability) const;
    bool Deliver(APawn *Interactor);

    UDeliveryTargetComponent();

    // 收件任务与距离
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Task")
    TObjectPtr<UDeliveryTaskDefinition> ExpectedTask;

    /** 交付判定距离，从收件人算起 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Task", meta = (ClampMin = "0.0", Units = "cm"))
    float DeliveryRadius = 100.f;

    /**
     * 交付允许的距离，单位为厘米，从地点组件的位置测量
     * 进入游戏时同步到交互组件，避免提示范围与实际交付范围不一致
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Task", meta = (ClampMin = "0.0", Units = "cm"))
    float InteractRadius = 180.f;

    // 交付检查与执行
    /** 手里这件快递现在能不能交给我；UI 用它决定要不要显示交付提示 */
    UFUNCTION(BlueprintPure, Category = "Task")
    bool CanAcceptDelivery(AActor *ItemActor, APlayerState *Player) const;

    /**
     * 取得交付距离的测量起点，与交互提示使用同一位置
     * 优先使用指定组件，其次使用碰撞表面，最后使用对象原点
     */
    UFUNCTION(BlueprintPure, Category = "Task")
    FVector GetDeliveryPoint(FVector FromLocation) const;

    /** 服务器：完成交付；成功后销毁快递 Actor，任务对所有玩家结束，奖励结算给 Player */
    UFUNCTION(BlueprintCallable, Category = "Task")
    bool TryDeliver(AActor *ItemActor, APlayerState *Player);

    // 交互组件配置
    /** 将默认交互配置调整为长按 E 键，并统一交互距离与交付距离 */
    void EnsureInteractableConfigured();

  protected:
    virtual void BeginPlay() override;
};
