// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryPhoneCallQueueComponent.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"
#include "Task/DeliveryTaskDefinition.h"
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

	FDeliveryPhoneCall& Call = Queue.AddDefaulted_GetRef();
	Call.Task = Task;
	Call.CallType = CallType;
	Call.CallId = NextCallId++;

	// 原来是空队列，这通就是队首，立刻开始响
	if (Queue.Num() == 1)
	{
		StartRinging();
	}
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

	const float TalkDuration = FMath::Max(0.5f, GetCallContent(Queue[0]).DurationSeconds);

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
	}

	if (Queue.Num() > 0)
	{
		StartRinging();
	}
	else
	{
		SetCallState(EDeliveryPhoneCallState::Idle, 0.f);
	}
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
