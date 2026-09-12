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
	UFUNCTION(BlueprintPure, Category="Task")
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
