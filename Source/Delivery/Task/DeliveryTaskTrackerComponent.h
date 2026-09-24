// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryTaskTrackerComponent.generated.h"

class UDeliveryTaskDefinition;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDeliveryTrackedTaskChanged, UDeliveryTaskDefinition*, Task);

/**
 * 每个玩家自己的"当前追踪任务"，挂在 PlayerState 上。
 * 任务状态是全局共享的，但追踪哪一个是各人各选，只有被追踪的任务显示地图引导。
 *
 * 选择权威在服务器：客户端 UI 走 RequestTrackTask 发请求，结果复制回来。
 * 自动选择规则（有人取件、只剩一个任务、追踪的任务完成了）也在服务器统一处理，
 * 避免两端各算一遍得出不同结果。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryTaskTrackerComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryTaskTrackerComponent();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 从 Pawn / Controller / PlayerState 任意一个拿到这个玩家的追踪组件。 */
	UFUNCTION(BlueprintPure, Category="Task", meta=(DisplayName="Get Delivery Task Tracker"))
	static UDeliveryTaskTrackerComponent* FindTracker(AActor* Actor);

	UFUNCTION(BlueprintPure, Category="Task")
	UDeliveryTaskDefinition* GetTrackedTask() const { return TrackedTask; }

	/** 手机 UI 选中某个任务。客户端调用会转成 Server RPC。 */
	UFUNCTION(BlueprintCallable, Category="Task")
	void RequestTrackTask(UDeliveryTaskDefinition* Task);

	/** 这个任务现在允不允许被选为追踪目标。有任务进行中时只能追踪它。 */
	UFUNCTION(BlueprintPure, Category="Task")
	bool CanTrackTask(const UDeliveryTaskDefinition* Task) const;

	UPROPERTY(BlueprintAssignable, Category="Task")
	FOnDeliveryTrackedTaskChanged OnTrackedTaskChanged;

	/**
	 * 当前该去哪。地图指引、方向箭头、距离提示都从这里取。
	 *
	 * 目的地按任务状态自动切换，调用方不需要自己判：
	 *   待取件 → PickupLocationId（去取货）
	 *   进行中 → DeliveryLocationId（去送货）
	 *   其他状态（未解锁 / 已完成）→ 返回 false
	 *
	 * 解析走 UDeliveryLocationRegistry，也就是关卡里挂了 UDeliveryLocationComponent
	 * 的那些点。策划表里填了 ID、关卡里却没有对应的点时返回 false 并在日志里点名——
	 * 这种配置漏项如果静默失败，表现是"箭头不显示"，几乎不可能查到根因。
	 */
	UFUNCTION(BlueprintPure, Category="Task")
	bool GetTrackedTaskDestination(FVector& OutLocation, FName& OutLocationId) const;

	/** 同上，但指定任务而不是当前追踪的那个。任务列表 UI 里显示各任务距离时用。 */
	UFUNCTION(BlueprintPure, Category="Task")
	bool GetTaskDestination(const UDeliveryTaskDefinition* Task, FVector& OutLocation,
		FName& OutLocationId) const;

protected:

	UFUNCTION(Server, Reliable)
	void ServerRequestTrackTask(UDeliveryTaskDefinition* Task);

	/** 服务器权威地设置追踪目标。 */
	void SetTrackedTask(UDeliveryTaskDefinition* Task);

	/** 服务器：按当前全局任务状态修正追踪目标。 */
	void RefreshAutoSelection();

	UFUNCTION()
	void HandleTaskStatusChanged(UDeliveryTaskDefinition* Task, EDeliveryTaskStatus NewStatus);

	/** GameState 和 PlayerState 的 BeginPlay 先后没有保证，延到下一帧再绑管理器。 */
	void BindToManager();

	UPROPERTY(ReplicatedUsing=OnRep_TrackedTask)
	TObjectPtr<UDeliveryTaskDefinition> TrackedTask;

	UFUNCTION()
	void OnRep_TrackedTask();
};
