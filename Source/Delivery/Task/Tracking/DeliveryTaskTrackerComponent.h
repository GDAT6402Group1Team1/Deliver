// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryTaskTrackerComponent.generated.h"

class UDeliveryTaskDefinition;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDeliveryTrackedTaskChanged, UDeliveryTaskDefinition*, Task);

/**
 * 保存玩家当前追踪的任务，挂在玩家状态上
 * 客户端请求切换，服务器统一选择并同步结果，界面只显示追踪任务的指引
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryTaskTrackerComponent : public UActorComponent
{
	GENERATED_BODY()

  public:
	UDeliveryTaskTrackerComponent();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 从 Pawn / Controller / PlayerState 任意一个拿到这个玩家的追踪组件 */
	UFUNCTION(BlueprintPure, Category = "Task", meta = (DisplayName = "Get Delivery Task Tracker"))
	static UDeliveryTaskTrackerComponent* FindTracker(AActor* Actor);

	// 追踪选择与查询
	UFUNCTION(BlueprintPure, Category = "Task")

	UDeliveryTaskDefinition* GetTrackedTask() const
	{
		return TrackedTask;
	}

	// 选择跟踪任务
	/** 手机 UI 选中某个任务；客户端调用会转成 Server RPC */
	UFUNCTION(BlueprintCallable, Category = "Task")
	void RequestTrackTask(UDeliveryTaskDefinition* Task);

	/** 这个任务现在允不允许被选为追踪目标；有任务已接单计时时只能追踪它 */
	UFUNCTION(BlueprintPure, Category = "Task")
	bool CanTrackTask(const UDeliveryTaskDefinition* Task) const;

	UPROPERTY(BlueprintAssignable, Category = "Task")
	FOnDeliveryTrackedTaskChanged OnTrackedTaskChanged;

	// 任务目的地
	/**
	 * 取得追踪任务的目的地，供所有导航界面共用
	 * 待取件时指向取货点，配送中指向收件点，其他状态不提供目标
	 * 地点编号无法解析时返回失败并记录日志
	 */
	UFUNCTION(BlueprintPure, Category = "Task")
	bool GetTrackedTaskDestination(FVector& OutLocation, FName& OutLocationId) const;

	// 目的地查询
	/** 同上，但指定任务而不是当前追踪的那个；任务列表 UI 里显示各任务距离时用 */
	UFUNCTION(BlueprintPure, Category = "Task")
	bool GetTaskDestination(const UDeliveryTaskDefinition* Task, FVector& OutLocation,
	                        FName& OutLocationId) const;

  protected:
	// 服务器选择与自动切换
	UFUNCTION(Server, Reliable)
	void ServerRequestTrackTask(UDeliveryTaskDefinition* Task);

	/** 服务器权威地设置追踪目标 */
	void SetTrackedTask(UDeliveryTaskDefinition* Task);

	// 自动选择有效任务
	/** 服务器：按当前全局任务状态修正追踪目标 */
	void RefreshAutoSelection();

	UFUNCTION()
	void HandleTaskStatusChanged(UDeliveryTaskDefinition* Task, EDeliveryTaskStatus NewStatus);

	/** GameState 和 PlayerState 的 BeginPlay 先后没有保证，延到下一帧再绑管理器 */
	void BindToManager();

	// 同步的追踪任务
	UPROPERTY(ReplicatedUsing = OnRep_TrackedTask)
	TObjectPtr<UDeliveryTaskDefinition> TrackedTask;

	UFUNCTION()
	void OnRep_TrackedTask();
};
