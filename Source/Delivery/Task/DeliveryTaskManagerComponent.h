// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryTaskManagerComponent.generated.h"

class APlayerState;
class UDeliveryPhoneCallQueueComponent;
class UDeliveryTaskDefinition;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDeliveryTaskStatusChanged, UDeliveryTaskDefinition*, Task,
                                             EDeliveryTaskStatus, NewStatus);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnDeliveryTaskCompleted, UDeliveryTaskDefinition*, Task,
                                               const FDeliveryRewardBreakdown&, Reward, APlayerState*,
                                               Deliverer);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDeliveryTaskOverdue, UDeliveryTaskDefinition*, Task);

/**
 * 在游戏状态上保存所有玩家共享的任务进度
 * 服务器修改状态并同步给客户端，客户端通过通知更新界面
 * 接听后按服务器时间差计时，掉件、换手和晕倒均不暂停计时
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryTaskManagerComponent : public UActorComponent
{
	GENERATED_BODY()

  public:
	UDeliveryTaskManagerComponent();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 从任意对象拿到全局任务管理器（挂在 GameState 上） */
	UFUNCTION(BlueprintPure, Category = "Task",
	          meta = (WorldContext = "WorldContextObject", DisplayName = "Get Delivery Task Manager"))
	static UDeliveryTaskManagerComponent* Get(const UObject* WorldContextObject);

	// 服务器任务操作
	/** —— 取件与交付（服务器）—— */

	/**
	 * 接通任务解锁电话时开始计时；幂等：同一任务重复调用不会重置
	 * 已有另一项任务在计时时返回 假
	 */
	bool StartTaskTimerFromAcceptedCall(UDeliveryTaskDefinition* Task);

	/**
	 * 这件快递现在能不能被拿起来；交互 / 背包系统在允许拾取前问这一句
	 * 规则：已经进行中的那个任务的快递随便捡（掉了还能捡回来）
	 * 只要世界上存在进行中的任务，其他任务的快递一律拿不起来
	 */
	UFUNCTION(BlueprintPure, Category = "Task")
	bool CanAcquireItem(const UDeliveryTaskDefinition* Task) const;

	/** 玩家取得快递后进入配送中，沿用接听时的计时，换手和重新拾取不重置 */
	UFUNCTION(BlueprintPure, Category = "Task")
	bool IsTaskAccepted(const UDeliveryTaskDefinition* Task) const;

	bool IsTaskRegistered(const UDeliveryTaskDefinition* Task) const;

	/**
	 * 服务器：玩家拿到了某个任务的快递；首次取件让任务进入进行中，但不会重置接电话时
	 * 已经开始的计时；漏接后直接取件时才以取件时刻兜底启动；换手、捡回不会重置
	 */
	UFUNCTION(BlueprintCallable, Category = "Task")
	bool TryAcquireItem(UDeliveryTaskDefinition* Task, APlayerState* Player);

	/** 服务器：在正确的收件人处完成交付；奖励结算给 Deliverer，任务对所有人结束 */
	UFUNCTION(BlueprintCallable, Category = "Task")
	bool TryCompleteDelivery(UDeliveryTaskDefinition* Task, APlayerState* Deliverer);

	/** 服务器：上报一个影响奖励的特殊事件（沿途恶作剧、完好送达之类）；重复的 Tag 只记一次 */
	UFUNCTION(BlueprintCallable, Category = "Task")
	void ReportSpecialEvent(UDeliveryTaskDefinition* Task, FGameplayTag EventTag);

	/**
	 * 服务器在快递丢失时将任务退回待取件，允许玩家回取货点重新领取
	 * 保留原计时，已完成的任务不受影响
	 */
	UFUNCTION(BlueprintCallable, Category = "Task")
	bool NotifyItemLost(UDeliveryTaskDefinition* Task);

	/**
	 * 服务器：重新评估所有未解锁任务的解锁条件
	 * 任务完成后会自动调一次；外部系统（剧情、金钱）改变了解锁条件时也要主动调
	 */
	UFUNCTION(BlueprintCallable, Category = "Task")
	void ReevaluateUnlocks();

	// 任务状态查询
	/** —— 查询（双端）—— */

	UFUNCTION(BlueprintPure, Category = "Task")
	EDeliveryTaskStatus GetTaskStatus(const UDeliveryTaskDefinition* Task) const;

	/** 当前进行中的任务，没有则为空；全世界最多一个 */
	UFUNCTION(BlueprintPure, Category = "Task")
	UDeliveryTaskDefinition* GetActiveTask() const;

	/** 当前已经接单并在计时的任务；待取件和进行中都算，全世界最多一个 */
	UFUNCTION(BlueprintPure, Category = "Task")
	UDeliveryTaskDefinition* GetTimedTask() const;

	UFUNCTION(BlueprintPure, Category = "Task")
	void GetTasksByStatus(EDeliveryTaskStatus Status, TArray<UDeliveryTaskDefinition*>& OutTasks) const;

	/**
	 * 取得全部配置任务，包括未解锁任务，供检查工具使用
	 * 玩家界面需要按任务状态筛选
	 */
	UFUNCTION(BlueprintPure, Category = "Task")
	const TArray<UDeliveryTaskDefinition*>& GetAllTaskDefinitions() const;

	// 时间与奖励计算
	/** UI 每帧拿倒计时和配色用；接听前 bRunning=假，接听后待取件阶段也为 真 */
	UFUNCTION(BlueprintPure, Category = "Task")
	FDeliveryTaskTimeSnapshot GetTimeSnapshot(const UDeliveryTaskDefinition* Task) const;

	/** 纯计算，不读状态；完成结算和 UI 预览都走它 */
	UFUNCTION(BlueprintPure, Category = "Task")
	FDeliveryRewardBreakdown EvaluateReward(const UDeliveryTaskDefinition* Task, float ElapsedSeconds,
	                                        const TArray<FGameplayTag>& SpecialEvents) const;

	/** 按任务当前的用时和已累计的特殊事件预览奖励，给手机 UI 显示"现在送到能拿多少" */
	UFUNCTION(BlueprintPure, Category = "Task")
	FDeliveryRewardBreakdown PreviewReward(const UDeliveryTaskDefinition* Task) const;

	/**
	 * 返回管理器开始运行后的秒数
	 * 以自身开始运行的时刻为基准，避免将世界时钟的初始偏移计入解锁条件
	 */
	UFUNCTION(BlueprintPure, Category = "Task")
	float GetTimeSinceLevelStart() const;

	/**
	 * 把 GetTimeSnapshot 的剩余秒数格式化成 UI 显示的样子：`05:00`
	 * 超时后剩余为负，输出 `+00:07` 并继续往上走
	 */
	UFUNCTION(BlueprintPure, Category = "Task")
	static FText FormatCountdown(float RemainingSeconds);

	// 任务事件
	/** —— 事件（服务器与客户端都会广播）—— */

	UPROPERTY(BlueprintAssignable, Category = "Task")
	FOnDeliveryTaskStatusChanged OnTaskStatusChanged;

	UPROPERTY(BlueprintAssignable, Category = "Task")
	FOnDeliveryTaskCompleted OnTaskCompleted;

	UPROPERTY(BlueprintAssignable, Category = "Task")
	FOnDeliveryTaskOverdue OnTaskOverdue;

  protected:
	// 关卡配置与同步进度
	/**
	 * 本关卡的全部任务；在 GameState 蓝图上配置
	 * 数组顺序就是同时解锁时进入电话队列的顺序
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Task")
	TArray<TObjectPtr<UDeliveryTaskDefinition>> TaskDefinitions;

	UPROPERTY(ReplicatedUsing = OnRep_Tasks)
	TArray<FDeliveryTaskState> Tasks;

	// 状态更新与客户端通知
	UFUNCTION()
	void OnRep_Tasks();

	FDeliveryTaskState* FindState(const UDeliveryTaskDefinition* Task);
	const FDeliveryTaskState* FindState(const UDeliveryTaskDefinition* Task) const;

	/** 服务器改状态的唯一入口，负责广播 */
	void SetTaskStatus(FDeliveryTaskState& State, EDeliveryTaskStatus NewStatus);

	/** 限时到点：打一次催促电话，任务继续 */
	void HandleOverdue(UDeliveryTaskDefinition* Task);

	UDeliveryPhoneCallQueueComponent* GetPhoneQueue() const;

	/** 服务器时间；客户端拿到的是同一条时间轴，倒计时不会各算各的 */
	float GetServerTimeSeconds() const;

	/**
	 * 还有未解锁任务时，每秒重新评估一次解锁条件
	 * 像"开局 15 秒后"这类条件不会有人来通知我们，只能主动轮询
	 * 全部解锁完就停掉，不留常驻定时器
	 */
	void UpdateUnlockPolling();

	// 服务器定时器
	/** 同一时间只有一个已经接单并计时的任务，一个句柄就够 */
	FTimerHandle OverdueTimerHandle;

	FTimerHandle UnlockPollTimerHandle;

	// 客户端变化比较
	/** 客户端上一次看到的状态，用来在 OnRep 里做差分并广播事件 */
	UPROPERTY(Transient)
	TMap<TObjectPtr<UDeliveryTaskDefinition>, FDeliveryTaskState> KnownStates;

	/** 客户端是否已经收到过首次同步；首次同步是"现状"，不是"刚刚发生的变化" */
	bool bReceivedInitialState = false;

	/** 本组件 BeginPlay 的时刻，作为"关卡开始"的基准 */
	float LevelStartServerTime = 0.f;
};
