// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "DeliveryTaskTypes.generated.h"

class APlayerState;
class UDeliveryTaskDefinition;
class USoundBase;

/**
 * 任务状态。没有失败态：超时只影响奖励倍率，不会结束任务。
 * 流转：Locked →(解锁条件满足) AwaitingPickup →(有人拿到快递) InProgress →(交付) Completed
 */
UENUM(BlueprintType)
enum class EDeliveryTaskStatus : uint8
{
	/** 解锁条件未满足，手机里看不到。 */
	Locked,
	/** 已解锁、来电已入队，但还没人取件。不计时、不过期，可以一直挂着。 */
	AwaitingPickup,
	/** 有人取过件，全局计时中。同一时间全世界只允许一个。 */
	InProgress,
	/** 已交付。 */
	Completed
};

/** 倒计时配色阶段，只驱动 UI。 */
UENUM(BlueprintType)
enum class EDeliveryTaskUrgency : uint8
{
	Green,
	Yellow,
	Red
};

UENUM(BlueprintType)
enum class EDeliveryPhoneCallType : uint8
{
	/** 任务解锁来电。 */
	TaskUnlocked,
	/** 首次超时的催促来电，同一任务只会有一次。 */
	Overdue
};

/** 一通电话的内容。真正的播放由 UI / 音频层负责，这里只存数据。 */
USTRUCT(BlueprintType)
struct FDeliveryPhoneCallContent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Phone")
	FText CallerName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Phone", meta=(MultiLine="true"))
	FText Dialogue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Phone")
	TSoftObjectPtr<USoundBase> Voice;

	/**
	 * 这通电话占用队列多久。服务器按这个时长推进电话队列，队列是全局共享的，
	 * 不能让某一个客户端的播放进度决定下一通什么时候响。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Phone", meta=(ClampMin="0.5", Units="s"))
	float DurationSeconds = 6.f;
};

/** 时间评价的一档。按 WithinSeconds 从小到大配置。 */
USTRUCT(BlueprintType)
struct FDeliveryTimeGrade
{
	GENERATED_BODY()

	/** 完成用时不超过这个秒数就吃这一档。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Reward", meta=(ClampMin="0.0", Units="s"))
	float WithinSeconds = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Reward", meta=(ClampMin="0.0"))
	float Multiplier = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Reward")
	FText GradeName;
};

/** 特殊事件奖励规则：途中上报的事件 Tag 命中哪条就乘哪条倍率。 */
USTRUCT(BlueprintType)
struct FDeliverySpecialEventRule
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Reward")
	FGameplayTag EventTag;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Reward", meta=(ClampMin="0.0"))
	float Multiplier = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Reward")
	FText DisplayName;
};

/**
 * 一个任务的运行时状态。整个数组由 GameState 上的任务管理器持有并复制，
 * 所以所有客户端看到的解锁、计时、完成都是同一份数据。
 */
USTRUCT(BlueprintType)
struct FDeliveryTaskState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Task")
	TObjectPtr<UDeliveryTaskDefinition> Definition = nullptr;

	UPROPERTY(BlueprintReadOnly, Category="Task")
	EDeliveryTaskStatus Status = EDeliveryTaskStatus::Locked;

	/**
	 * 第一次取件时的服务器时间。倒计时是"当前服务器时间减去它"算出来的，不是每帧累加的，
	 * 所以快递掉落、换手、进后备箱、玩家晕倒都不会让计时停下，也不用为此写任何同步。
	 */
	UPROPERTY(BlueprintReadOnly, Category="Task")
	float StartServerTime = 0.f;

	/** 交付完成时的服务器时间，用于结算时间评价。 */
	UPROPERTY(BlueprintReadOnly, Category="Task")
	float CompleteServerTime = 0.f;

	/** 催促电话只打一次，打过就置位。 */
	UPROPERTY(BlueprintReadOnly, Category="Task")
	bool bOverdueCallPlayed = false;

	/** 途中累计的特殊事件，交付时参与奖励倍率计算。 */
	UPROPERTY(BlueprintReadOnly, Category="Task")
	TArray<FGameplayTag> SpecialEvents;

	/** 最终完成交付的玩家。奖励只结算给他，其他人只是任务一起结束。 */
	UPROPERTY(BlueprintReadOnly, Category="Task")
	TObjectPtr<APlayerState> Deliverer = nullptr;
};

/** UI 每帧查询用的计时快照。 */
USTRUCT(BlueprintType)
struct FDeliveryTaskTimeSnapshot
{
	GENERATED_BODY()

	/** 任务是否在计时（只有 InProgress 为 true）。 */
	UPROPERTY(BlueprintReadOnly, Category="Task")
	bool bRunning = false;

	UPROPERTY(BlueprintReadOnly, Category="Task")
	float ElapsedSeconds = 0.f;

	/** 剩余秒数。超时后为负值，UI 用它的绝对值配合 "+" 号做正计时显示。 */
	UPROPERTY(BlueprintReadOnly, Category="Task")
	float RemainingSeconds = 0.f;

	UPROPERTY(BlueprintReadOnly, Category="Task")
	bool bOverdue = false;

	UPROPERTY(BlueprintReadOnly, Category="Task")
	EDeliveryTaskUrgency Urgency = EDeliveryTaskUrgency::Green;
};

/** 奖励结算明细：最终奖励 = 基础奖励 × 时间评价 × 特殊事件。 */
USTRUCT(BlueprintType)
struct FDeliveryRewardBreakdown
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Reward")
	int32 BaseReward = 0;

	UPROPERTY(BlueprintReadOnly, Category="Reward")
	float TimeMultiplier = 1.f;

	UPROPERTY(BlueprintReadOnly, Category="Reward")
	float SpecialMultiplier = 1.f;

	UPROPERTY(BlueprintReadOnly, Category="Reward")
	int32 FinalReward = 0;

	/** 命中的时间评价档名字；超时交付时为空。 */
	UPROPERTY(BlueprintReadOnly, Category="Reward")
	FText TimeGradeName;

	UPROPERTY(BlueprintReadOnly, Category="Reward")
	TArray<FGameplayTag> SpecialEvents;

	UPROPERTY(BlueprintReadOnly, Category="Reward")
	float ElapsedSeconds = 0.f;

	UPROPERTY(BlueprintReadOnly, Category="Reward")
	bool bOverdue = false;
};

/** 电话队列里的一项。 */
USTRUCT(BlueprintType)
struct FDeliveryPhoneCall
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Phone")
	TObjectPtr<UDeliveryTaskDefinition> Task = nullptr;

	UPROPERTY(BlueprintReadOnly, Category="Phone")
	EDeliveryPhoneCallType CallType = EDeliveryPhoneCallType::TaskUnlocked;

	/** 递增编号。客户端靠它判断队首换人了没有，不能只比 Task 指针。 */
	UPROPERTY(BlueprintReadOnly, Category="Phone")
	int32 CallId = 0;
};
