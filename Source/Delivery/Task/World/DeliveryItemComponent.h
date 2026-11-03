// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryItemComponent.generated.h"

class APlayerState;
class UDeliveryTaskDefinition;

/**
 * 挂在快递 Actor 上，声明"这件快递属于哪个任务"
 * 本组件不管拿在手里、掉在地上、塞进后备箱这些事，那是交互 / 背包系统的活
 * 它只回答两个问题：现在能不能捡，以及捡起来这件事要不要让任务进入进行中
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryItemComponent : public UActorComponent
{
	GENERATED_BODY()

  public:
	UDeliveryItemComponent();

	// 关联任务
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Task")
	TObjectPtr<UDeliveryTaskDefinition> OwningTask;

	// 领取条件与任务更新
	/**
	 * 交互系统在允许玩家拾取之前问这一句
	 * 世界上已经有接单计时任务时，其他任务的快递会在这里被拦住
	 */
	UFUNCTION(BlueprintPure, Category = "Task")
	bool CanBeAcquired() const;

	/** 允许拾取时返回空文本，否则返回无法拾取的原因 */
	FText GetPickupBlockedReason() const;

	/** 服务器记录玩家已取得快递，首次领取进入配送中，保留接听时开始的计时 */
	UFUNCTION(BlueprintCallable, Category = "Task")
	bool NotifyAcquired(APlayerState* Player);

	// 丢件处理
	/**
	 * 这份快递被销毁时，要不要把任务退回待取件
	 * 
	 * 默认开；关掉的场合：快递是"用完就换一份"的流程的一部分，销毁属于正常步骤
	 * 交付时的销毁不受影响——那时任务已经是已完成，NotifyItemLost 会自己忽略
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Task")
	bool bReportLostOnDestroy = true;

  protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
};
