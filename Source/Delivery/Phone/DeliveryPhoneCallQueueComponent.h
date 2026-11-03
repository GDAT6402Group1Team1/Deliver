// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryPhoneCallQueueComponent.generated.h"

class UDeliveryTaskDefinition;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDeliveryPhoneStateChanged, EDeliveryPhoneCallState, NewState,
                                             const FDeliveryPhoneCall&, Call);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDeliveryPhoneCallMissed, const FDeliveryPhoneCall&, Call);

/**
 * 管理所有玩家共享的电话队列，依次处理响铃、接听和挂断
 * 服务器统一计时，客户端按同步状态播放声音和界面
 * 已有任务计时时暂停后续解锁电话，但允许当前任务的催促电话进入
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryPhoneCallQueueComponent : public UActorComponent
{
	GENERATED_BODY()

  public:
	UDeliveryPhoneCallQueueComponent();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 从任意对象拿到全局电话队列（挂在 GameState 上） */
	UFUNCTION(BlueprintPure, Category = "Phone",
	          meta = (WorldContext = "WorldContextObject", DisplayName = "Get Delivery Phone Queue"))
	static UDeliveryPhoneCallQueueComponent* Get(const UObject* WorldContextObject);

	// 来电入队与接听
	// 服务器队列操作
	/** 服务器：来电入队；同一任务同一类型不会重复入队 */
	void EnqueueCall(UDeliveryTaskDefinition* Task, EDeliveryPhoneCallType CallType);

	/** 当前没有通话且没有其他订单计时时，尝试让队首开始响铃；任务完成后由 Manager 调用 */
	void TryStartNextCall();

	/**
	 * 服务器：接听当前来电，只有 Ringing 状态下有效
	 * 客户端不要直接调——走 PlayerController 的 RequestAnswerCall
	 */
	bool AnswerCurrentCall();

	/** 服务器：挂断；Ringing 时等于拒接，InCall 时等于提前结束，两种都直接进下一通 */
	bool HangUpCurrentCall();

	// 电话查询
	/** —— UI 查询 —— */

	UFUNCTION(BlueprintPure, Category = "Phone")

	EDeliveryPhoneCallState GetCallState() const
	{
		return CallState;
	}

	// 通话状态查询
	/** 当前这通电话；Idle 时返回 假 */
	UFUNCTION(BlueprintPure, Category = "Phone")
	bool GetCurrentCall(FDeliveryPhoneCall& OutCall) const;

	UFUNCTION(BlueprintPure, Category = "Phone")

	int32 GetPendingCallCount() const
	{
		return Queue.Num();
	}

	/** 取出这通电话的台词、语音和时长 */
	UFUNCTION(BlueprintPure, Category = "Phone")
	static FDeliveryPhoneCallContent GetCallContent(const FDeliveryPhoneCall& Call);

	/**
	 * 当前阶段还剩多少秒：响铃时是还能接多久，通话时是台词还有多久播完
	 * 做响铃倒计时条或通话进度用得上；Idle 时返回 0
	 */
	UFUNCTION(BlueprintPure, Category = "Phone")
	float GetStateRemainingSeconds() const;

	// 状态通知
	/** —— 事件（服务器与客户端都会广播）—— */

	/** 状态变化；来电界面 / 通话界面 / 待机界面的切换接这一个就够 */
	UPROPERTY(BlueprintAssignable, Category = "Phone")
	FOnDeliveryPhoneStateChanged OnPhoneStateChanged;

	/** 响铃超时没人接；UI 可以据此显示"未接来电" */
	UPROPERTY(BlueprintAssignable, Category = "Phone")
	FOnDeliveryPhoneCallMissed OnCallMissed;

  protected:
	// 重拨配置
	/**
	 * 未接听任务解锁电话后的重拨间隔，单位为秒
	 * 任务需要接听电话才能继续，因此解锁电话必须重拨，催促电话不重拨
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Delivery|Phone",
	          meta = (ClampMin = "1.0", Units = "s"))
	float RedialDelaySeconds = 20.f;

	// 同步队列和计时
	UPROPERTY(ReplicatedUsing = OnRep_Queue)
	TArray<FDeliveryPhoneCall> Queue;

	UPROPERTY(ReplicatedUsing = OnRep_CallState)
	EDeliveryPhoneCallState CallState = EDeliveryPhoneCallState::Idle;

	/** 当前阶段从什么时候开始；和任务计时一样用服务器时间戳，客户端算出来的剩余时间才一致 */
	UPROPERTY(Replicated)
	float StateStartServerTime = 0.f;

	/** 当前阶段总共多久；存下来省得 UI 再去翻配置 */
	UPROPERTY(Replicated)
	float StateDurationSeconds = 0.f;

	// 同步通知与状态推进
	UFUNCTION()
	void OnRep_Queue();

	// 客户端状态通知
	UFUNCTION()
	void OnRep_CallState();

	/** 服务器：让队首开始响铃 */
	void StartRinging();

	/** 服务器：结束当前这通，弹出并接下一通 */
	void FinishCurrentCall(bool bMissed);

	void HandleRingTimeout();
	void HandleCallFinished();

	/** 服务器改状态的唯一入口，负责记时间戳和广播 */
	void SetCallState(EDeliveryPhoneCallState NewState, float Duration);

	// 未接来电处理
	// 未接任务的重拨
	/** 拒接/漏接之后，若任务还没被接单就排一次重拨 */
	void ScheduleRedial(UDeliveryTaskDefinition* Task);

	UFUNCTION()
	void HandleRedial(UDeliveryTaskDefinition* Task);

	FTimerHandle RedialTimerHandle;

	// 通知去重与服务器时钟
	/** 双端：状态或队首变了就广播一次 */
	void BroadcastStateIfChanged();

	float GetServerTimeSeconds() const;

	FTimerHandle CallTimerHandle;

	/** 服务器发号用 */
	int32 NextCallId = 1;

	/** 双端各自记住上次广播的内容，避免同一个状态重复广播 */
	EDeliveryPhoneCallState LastBroadcastState = EDeliveryPhoneCallState::Idle;
	int32 LastBroadcastCallId = 0;
};
