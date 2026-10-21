// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryPhoneCallQueueComponent.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"
#include "Delivery.h"
#include "Task/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "TimerManager.h"

UDeliveryPhoneCallQueueComponent::UDeliveryPhoneCallQueueComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UDeliveryPhoneCallQueueComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UDeliveryPhoneCallQueueComponent, Queue);
	DOREPLIFETIME(UDeliveryPhoneCallQueueComponent, CallState);
	DOREPLIFETIME(UDeliveryPhoneCallQueueComponent, StateStartServerTime);
	DOREPLIFETIME(UDeliveryPhoneCallQueueComponent, StateDurationSeconds);
}

UDeliveryPhoneCallQueueComponent* UDeliveryPhoneCallQueueComponent::Get(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	AGameStateBase* GameState = World ? World->GetGameState() : nullptr;

	return GameState ? GameState->FindComponentByClass<UDeliveryPhoneCallQueueComponent>() : nullptr;
}

float UDeliveryPhoneCallQueueComponent::GetServerTimeSeconds() const
{
	if (const AGameStateBase* GameState = Cast<AGameStateBase>(GetOwner()))
	{
		return GameState->GetServerWorldTimeSeconds();
	}

	return GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
}

FDeliveryPhoneCallContent UDeliveryPhoneCallQueueComponent::GetCallContent(const FDeliveryPhoneCall& Call)
{
	if (!Call.Task)
	{
		return FDeliveryPhoneCallContent();
	}

	return Call.CallType == EDeliveryPhoneCallType::Overdue ? Call.Task->OverdueCall : Call.Task->UnlockCall;
}

bool UDeliveryPhoneCallQueueComponent::GetCurrentCall(FDeliveryPhoneCall& OutCall) const
{
	if (Queue.Num() == 0 || CallState == EDeliveryPhoneCallState::Idle)
	{
		return false;
	}

	OutCall = Queue[0];

	return true;
}

float UDeliveryPhoneCallQueueComponent::GetStateRemainingSeconds() const
{
	if (CallState == EDeliveryPhoneCallState::Idle)
	{
		return 0.f;
	}

	return FMath::Max(0.f, StateDurationSeconds - (GetServerTimeSeconds() - StateStartServerTime));
}

void UDeliveryPhoneCallQueueComponent::EnqueueCall(UDeliveryTaskDefinition* Task, EDeliveryPhoneCallType CallType)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !Task)
	{
		return;
	}

	// 解锁条件会被反复评估，同一通电话不要排两次
	for (const FDeliveryPhoneCall& Existing : Queue)
	{
		if (Existing.Task == Task && Existing.CallType == CallType)
		{
			return;
		}
	}

	FDeliveryPhoneCall Call;
	Call.Task = Task;
	Call.CallType = CallType;
	Call.CallId = NextCallId++;

	if (CallType == EDeliveryPhoneCallType::Overdue)
	{
		// 催促电话属于当前订单，不能被后续订单的暂停来电堵在后面。
		// 当前正在响/通话时保留队首，在它后面插入；待机时直接插到最前。
		Queue.Insert(Call, CallState == EDeliveryPhoneCallState::Idle ? 0 : FMath::Min(1, Queue.Num()));
	}
	else
	{
		Queue.Add(Call);
	}

	TryStartNextCall();
}

void UDeliveryPhoneCallQueueComponent::TryStartNextCall()
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !GetWorld() ||
		CallState != EDeliveryPhoneCallState::Idle || Queue.IsEmpty())
	{
		return;
	}

	const FDeliveryPhoneCall& Next = Queue[0];
	if (Next.CallType == EDeliveryPhoneCallType::TaskUnlocked)
	{
		const UDeliveryTaskManagerComponent* Manager =
			GetOwner()->FindComponentByClass<UDeliveryTaskManagerComponent>();
		const UDeliveryTaskDefinition* TimedTask = Manager ? Manager->GetTimedTask() : nullptr;
		if (TimedTask && TimedTask != Next.Task)
		{
			// 当前订单完成后 Manager 会再次调用本函数。队列保留，不丢来电。
			return;
		}
	}

	StartRinging();
}

void UDeliveryPhoneCallQueueComponent::StartRinging()
{
	if (Queue.Num() == 0 || !GetWorld())
	{
		return;
	}

	const float RingDuration = FMath::Max(1.f, GetCallContent(Queue[0]).RingDurationSeconds);

	SetCallState(EDeliveryPhoneCallState::Ringing, RingDuration);

	GetWorld()->GetTimerManager().SetTimer(
		CallTimerHandle, this, &UDeliveryPhoneCallQueueComponent::HandleRingTimeout, RingDuration, false);
}

bool UDeliveryPhoneCallQueueComponent::AnswerCurrentCall()
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || CallState != EDeliveryPhoneCallState::Ringing)
	{
		return false;
	}

	const FDeliveryPhoneCall& Call = Queue[0];
	if (Call.CallType == EDeliveryPhoneCallType::TaskUnlocked)
	{
		UDeliveryTaskManagerComponent* Manager =
			GetOwner()->FindComponentByClass<UDeliveryTaskManagerComponent>();
		if (!Manager || !Manager->StartTaskTimerFromAcceptedCall(Call.Task))
		{
			return false;
		}
	}

	const float TalkDuration = FMath::Max(0.5f, GetCallContent(Call).DurationSeconds);

	SetCallState(EDeliveryPhoneCallState::InCall, TalkDuration);

	if (GetWorld())
	{
		GetWorld()->GetTimerManager().SetTimer(
			CallTimerHandle, this, &UDeliveryPhoneCallQueueComponent::HandleCallFinished, TalkDuration, false);
	}

	return true;
}

bool UDeliveryPhoneCallQueueComponent::HangUpCurrentCall()
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || CallState == EDeliveryPhoneCallState::Idle)
	{
		return false;
	}

	// 响铃时挂断算拒接，和没人接一样记为未接
	FinishCurrentCall(CallState == EDeliveryPhoneCallState::Ringing);

	return true;
}

void UDeliveryPhoneCallQueueComponent::HandleRingTimeout()
{
	FinishCurrentCall(true);
}

void UDeliveryPhoneCallQueueComponent::HandleCallFinished()
{
	FinishCurrentCall(false);
}

void UDeliveryPhoneCallQueueComponent::FinishCurrentCall(bool bMissed)
{
	if (Queue.Num() == 0)
	{
		SetCallState(EDeliveryPhoneCallState::Idle, 0.f);
		return;
	}

	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(CallTimerHandle);
	}

	const FDeliveryPhoneCall Finished = Queue[0];
	Queue.RemoveAt(0);

	if (bMissed)
	{
		OnCallMissed.Broadcast(Finished);

		// 拒接/漏接的解锁电话要重拨。取件现在要求接过电话，不重拨的话
		// 挂一次就把这个任务永久锁死
		if (Finished.CallType == EDeliveryPhoneCallType::TaskUnlocked)
		{
			ScheduleRedial(Finished.Task);
		}
	}

	SetCallState(EDeliveryPhoneCallState::Idle, 0.f);
	TryStartNextCall();
}

void UDeliveryPhoneCallQueueComponent::SetCallState(EDeliveryPhoneCallState NewState, float Duration)
{
	CallState = NewState;
	StateStartServerTime = GetServerTimeSeconds();
	StateDurationSeconds = Duration;

	BroadcastStateIfChanged();
}

void UDeliveryPhoneCallQueueComponent::BroadcastStateIfChanged()
{
	const int32 HeadId = Queue.Num() > 0 ? Queue[0].CallId : 0;

	// 状态没变、队首也没换，就不用再广播一次
	if (CallState == LastBroadcastState && HeadId == LastBroadcastCallId)
	{
		return;
	}

	LastBroadcastState = CallState;
	LastBroadcastCallId = HeadId;

	FDeliveryPhoneCall Call;
	GetCurrentCall(Call);

	OnPhoneStateChanged.Broadcast(CallState, Call);
}

void UDeliveryPhoneCallQueueComponent::OnRep_Queue()
{
	BroadcastStateIfChanged();
}

void UDeliveryPhoneCallQueueComponent::OnRep_CallState()
{
	BroadcastStateIfChanged();
}

void UDeliveryPhoneCallQueueComponent::ScheduleRedial(UDeliveryTaskDefinition* Task)
{
	UWorld* World = GetWorld();
	if (!Task || !World || RedialDelaySeconds <= 0.f)
	{
		return;
	}

	// 只用一个 handle：同一时刻最多排一通重拨就够，后面的解锁电话本来就在队列里等。
	// 每次重排都先 Clear——三处共用一个变量却只 Set 不 Clear 的话，旧 handle
	// 会被新值冲掉、旧计时器还在跑（交通车那边踩过一模一样的坑）
	World->GetTimerManager().ClearTimer(RedialTimerHandle);
	World->GetTimerManager().SetTimer(RedialTimerHandle,
		FTimerDelegate::CreateUObject(this, &UDeliveryPhoneCallQueueComponent::HandleRedial, Task),
		RedialDelaySeconds, false);

	UE_LOG(LogDelivery, Log, TEXT("[电话] %s 被拒接/漏接，%.0f 秒后重拨"),
		*Task->TaskId.ToString(), RedialDelaySeconds);
}

void UDeliveryPhoneCallQueueComponent::HandleRedial(UDeliveryTaskDefinition* Task)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !Task)
	{
		return;
	}

	const UDeliveryTaskManagerComponent* Manager =
		GetOwner()->FindComponentByClass<UDeliveryTaskManagerComponent>();
	if (!Manager)
	{
		return;
	}

	// 这段时间里玩家可能已经从别的途径接了单、或者任务已经完成。
	// 判据用 CanAcquireItem 的那一半——状态还是待取件、且还没接单
	// 还没接听的任务停在 Offered。接了（AwaitingPickup）或者已经做掉了就别再打
	if (Manager->GetTaskStatus(Task) != EDeliveryTaskStatus::Offered)
	{
		return;
	}

	EnqueueCall(Task, EDeliveryPhoneCallType::TaskUnlocked);
}
