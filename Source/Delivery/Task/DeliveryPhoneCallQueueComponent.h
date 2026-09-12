// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryPhoneCallQueueComponent.generated.h"

class UDeliveryTaskDefinition;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDeliveryPhoneCallStarted, const FDeliveryPhoneCall&, Call);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDeliveryPhoneQueueDrained);

/**
 * 全局电话队列，和任务管理器一起挂在 GameState 上。
 * 解锁来电和超时催促都进这一条队列，按入队顺序一通一通放，不会两通叠在一起。
 *
 * 队列推进由服务器按每通电话配置的时长驱动，而不是等客户端播完回报：
 * 队列是所有人共享的，不能让某一个客户端的播放进度决定下一通什么时候响。
 * 客户端只负责把队首那通电话表现出来。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryPhoneCallQueueComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryPhoneCallQueueComponent();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 从任意对象拿到全局电话队列（挂在 GameState 上）。 */
	UFUNCTION(BlueprintPure, Category="Phone", meta=(WorldContext="WorldContextObject"))
	static UDeliveryPhoneCallQueueComponent* Get(const UObject* WorldContextObject);

	/** 服务器：来电入队。同一任务同一类型不会重复入队。 */
	void EnqueueCall(UDeliveryTaskDefinition* Task, EDeliveryPhoneCallType CallType);

	/** 当前正在播的那通电话（队首）。队列为空返回 false。 */
	UFUNCTION(BlueprintPure, Category="Phone")
	bool GetCurrentCall(FDeliveryPhoneCall& OutCall) const;

	UFUNCTION(BlueprintPure, Category="Phone")
	int32 GetPendingCallCount() const { return Queue.Num(); }

	/** 取出这通电话对应的台词和语音。 */
	UFUNCTION(BlueprintPure, Category="Phone")
	static FDeliveryPhoneCallContent GetCallContent(const FDeliveryPhoneCall& Call);

	/** 队首换人时广播，UI 接这个来响铃 / 播台词。 */
	UPROPERTY(BlueprintAssignable, Category="Phone")
	FOnDeliveryPhoneCallStarted OnCallStarted;

	/** 队列播空时广播，UI 接这个收线。 */
	UPROPERTY(BlueprintAssignable, Category="Phone")
	FOnDeliveryPhoneQueueDrained OnQueueDrained;

protected:

	UPROPERTY(ReplicatedUsing=OnRep_Queue)
	TArray<FDeliveryPhoneCall> Queue;

	UFUNCTION()
	void OnRep_Queue();

	/** 服务器：给队首电话起一个到时推进的定时器。 */
	void StartHeadCall();

	/** 服务器：队首播完，弹出并接下一通。 */
	void AdvanceQueue();

	/** 双端：队首变了就广播。客户端靠 CallId 而不是任务指针判断换没换人。 */
	void BroadcastHeadIfChanged();

	FTimerHandle CallTimerHandle;

	/** 服务器发号用。 */
	int32 NextCallId = 1;

	/** 双端各自记住上一次广播过的队首编号。 */
	int32 LastStartedCallId = 0;
};
