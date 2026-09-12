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
}

UDeliveryPhoneCallQueueComponent* UDeliveryPhoneCallQueueComponent::Get(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	AGameStateBase* GameState = World ? World->GetGameState() : nullptr;

	return GameState ? GameState->FindComponentByClass<UDeliveryPhoneCallQueueComponent>() : nullptr;
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
	if (Queue.Num() == 0)
	{
		return false;
	}

	OutCall = Queue[0];

	return true;
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

	// 原来是空队列，这通就是队首，立刻开始
	if (Queue.Num() == 1)
	{
		StartHeadCall();
	}
}

void UDeliveryPhoneCallQueueComponent::StartHeadCall()
{
	if (Queue.Num() == 0 || !GetWorld())
	{
		return;
	}

	const float Duration = FMath::Max(0.5f, GetCallContent(Queue[0]).DurationSeconds);
	GetWorld()->GetTimerManager().SetTimer(CallTimerHandle, this, &UDeliveryPhoneCallQueueComponent::AdvanceQueue, Duration, false);

	BroadcastHeadIfChanged();
}

void UDeliveryPhoneCallQueueComponent::AdvanceQueue()
{
	if (Queue.Num() > 0)
	{
		Queue.RemoveAt(0);
	}

	if (Queue.Num() > 0)
	{
		StartHeadCall();
	}
	else
	{
		BroadcastHeadIfChanged();
	}
}

void UDeliveryPhoneCallQueueComponent::BroadcastHeadIfChanged()
{
	const int32 HeadId = Queue.Num() > 0 ? Queue[0].CallId : 0;
	if (HeadId == LastStartedCallId)
	{
		return;
	}

	LastStartedCallId = HeadId;

	if (HeadId != 0)
	{
		OnCallStarted.Broadcast(Queue[0]);
	}
	else
	{
		OnQueueDrained.Broadcast();
	}
}

void UDeliveryPhoneCallQueueComponent::OnRep_Queue()
{
	BroadcastHeadIfChanged();
}
