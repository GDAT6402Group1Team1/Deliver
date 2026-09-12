// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryTaskManagerComponent.generated.h"

class APlayerState;
class UDeliveryPhoneCallQueueComponent;
class UDeliveryTaskDefinition;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDeliveryTaskStatusChanged, UDeliveryTaskDefinition*, Task, EDeliveryTaskStatus, NewStatus);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnDeliveryTaskCompleted, UDeliveryTaskDefinition*, Task, const FDeliveryRewardBreakdown&, Reward, APlayerState*, Deliverer);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDeliveryTaskOverdue, UDeliveryTaskDefinition*, Task);

/**
 * 全局任务状态的唯一权威，挂在 GameState 上，所以多人模式下所有人共享同一份解锁、计时和完成结果。
 * 所有改状态的接口只在服务器生效；客户端通过复制 + OnRep 得到同样的事件。
 *
 * 计时不用 Tick：取件时记一个服务器时间戳，之后所有人按"当前服务器时间 - 时间戳"算，
 * 快递掉落、换手、进后备箱、玩家晕倒都影响不到它，正好符合"计时不停"的规则。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryTaskManagerComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryTaskManagerComponent();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 从任意对象拿到全局任务管理器（挂在 GameState 上）。 */
	UFUNCTION(BlueprintPure, Category="Task", meta=(WorldContext="WorldContextObject"))
	static UDeliveryTaskManagerComponent* Get(const UObject* WorldContextObject);

	/** —— 取件与交付（服务器）—— */

	/**
	 * 这件快递现在能不能被拿起来。交互 / 背包系统在允许拾取前问这一句。
	 * 规则：已经进行中的那个任务的快递随便捡（掉了还能捡回来）；
	 * 只要世界上存在进行中的任务，其他任务的快递一律拿不起来。
	 */
	UFUNCTION(BlueprintPure, Category="Task")
	bool CanAcquireItem(const UDeliveryTaskDefinition* Task) const;

	/**
	 * 服务器：玩家拿到了某个任务的快递。首次取件会让任务进入进行中并开始全局计时；
	 * 之后的换手、捡回都会走到这里，但不会重置计时。返回值表示这次是否真的接取了任务。
	 */
	UFUNCTION(BlueprintCallable, Category="Task")
	bool TryAcquireItem(UDeliveryTaskDefinition* Task, APlayerState* Player);

	/** 服务器：在正确的收件人处完成交付。奖励结算给 Deliverer，任务对所有人结束。 */
	UFUNCTION(BlueprintCallable, Category="Task")
	bool TryCompleteDelivery(UDeliveryTaskDefinition* Task, APlayerState* Deliverer);

	/** 服务器：上报一个影响奖励的特殊事件（沿途恶作剧、完好送达之类）。重复的 Tag 只记一次。 */
	UFUNCTION(BlueprintCallable, Category="Task")
	void ReportSpecialEvent(UDeliveryTaskDefinition* Task, FGameplayTag EventTag);

	/**
	 * 服务器：重新评估所有未解锁任务的解锁条件。
	 * 任务完成后会自动调一次；外部系统（剧情、金钱）改变了解锁条件时也要主动调。
	 */
	UFUNCTION(BlueprintCallable, Category="Task")
	void ReevaluateUnlocks();

	/** —— 查询（双端）—— */

	UFUNCTION(BlueprintPure, Category="Task")
	EDeliveryTaskStatus GetTaskStatus(const UDeliveryTaskDefinition* Task) const;

	/** 当前进行中的任务，没有则为空。全世界最多一个。 */
	UFUNCTION(BlueprintPure, Category="Task")
	UDeliveryTaskDefinition* GetActiveTask() const;

	UFUNCTION(BlueprintPure, Category="Task")
	void GetTasksByStatus(EDeliveryTaskStatus Status, TArray<UDeliveryTaskDefinition*>& OutTasks) const;

	/** UI 每帧拿倒计时和配色用。未取件的任务返回 bRunning=false。 */
	UFUNCTION(BlueprintPure, Category="Task")
	FDeliveryTaskTimeSnapshot GetTimeSnapshot(const UDeliveryTaskDefinition* Task) const;

	/** 纯计算，不读状态。完成结算和 UI 预览都走它。 */
	UFUNCTION(BlueprintPure, Category="Task")
	FDeliveryRewardBreakdown EvaluateReward(const UDeliveryTaskDefinition* Task, float ElapsedSeconds, const TArray<FGameplayTag>& SpecialEvents) const;

	/** 按任务当前的用时和已累计的特殊事件预览奖励，给手机 UI 显示"现在送到能拿多少"。 */
	UFUNCTION(BlueprintPure, Category="Task")
	FDeliveryRewardBreakdown PreviewReward(const UDeliveryTaskDefinition* Task) const;

	/** —— 事件（服务器与客户端都会广播）—— */

	UPROPERTY(BlueprintAssignable, Category="Task")
	FOnDeliveryTaskStatusChanged OnTaskStatusChanged;

	UPROPERTY(BlueprintAssignable, Category="Task")
	FOnDeliveryTaskCompleted OnTaskCompleted;

	UPROPERTY(BlueprintAssignable, Category="Task")
	FOnDeliveryTaskOverdue OnTaskOverdue;

protected:

	/**
	 * 本关卡的全部任务。在 GameState 蓝图上配置。
	 * 数组顺序就是同时解锁时进入电话队列的顺序。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Task")
	TArray<TObjectPtr<UDeliveryTaskDefinition>> TaskDefinitions;

	UPROPERTY(ReplicatedUsing=OnRep_Tasks)
	TArray<FDeliveryTaskState> Tasks;

	UFUNCTION()
	void OnRep_Tasks();

	FDeliveryTaskState* FindState(const UDeliveryTaskDefinition* Task);
	const FDeliveryTaskState* FindState(const UDeliveryTaskDefinition* Task) const;

	/** 服务器改状态的唯一入口，负责广播。 */
	void SetTaskStatus(FDeliveryTaskState& State, EDeliveryTaskStatus NewStatus);

	/** 限时到点：打一次催促电话，任务继续。 */
	void HandleOverdue(UDeliveryTaskDefinition* Task);

	UDeliveryPhoneCallQueueComponent* GetPhoneQueue() const;

	/** 服务器时间。客户端拿到的是同一条时间轴，倒计时不会各算各的。 */
	float GetServerTimeSeconds() const;

	/** 同一时间只有一个进行中任务，一个句柄就够。 */
	FTimerHandle OverdueTimerHandle;

	/** 客户端上一次看到的状态，用来在 OnRep 里做差分并广播事件。 */
	UPROPERTY(Transient)
	TMap<TObjectPtr<UDeliveryTaskDefinition>, FDeliveryTaskState> KnownStates;
};
