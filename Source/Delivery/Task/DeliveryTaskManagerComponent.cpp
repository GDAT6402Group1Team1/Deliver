// Copyright Epic Games, Inc. All Rights Reserved.

/*
状态机只有四个状态，没有失败态：

Locked        解锁条件没满足。
AwaitingPickup 解锁了、来电已入队，但还没人取件。不计时、不过期。
InProgress    有人取过件。全局计时中，全世界同时只允许一个。
Completed     交付完成。

关于计时：取件那一刻记下服务器时间戳，之后谁都不再碰它。倒计时是"现在减去它"算出来的，
不是每帧累加的。这样快递掉地上、被别人抢走、塞进车后备箱、持有者晕倒，计时都不会停，
也不需要为这些情况各写一遍同步逻辑——它们根本碰不到计时。

关于"只有一个进行中任务"：这条规则落在 CanAcquireItem 上。只要世界上存在进行中的任务，
其他任务的快递就拿不起来，所以不需要额外的互斥状态，也不会出现两个任务同时计时。

关于客户端：所有状态变化都通过复制 Tasks 数组下发，OnRep 里跟上一次的快照做差分，
补广播出跟服务器同样的事件，所以 UI 只需要监听这三个委托，不用区分自己跑在哪一端。
*/

#include "DeliveryTaskManagerComponent.h"

#include "Delivery.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"
#include "Task/DeliveryPhoneCallQueueComponent.h"
#include "Task/DeliveryTaskDefinition.h"
#include "TimerManager.h"

UDeliveryTaskManagerComponent::UDeliveryTaskManagerComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UDeliveryTaskManagerComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UDeliveryTaskManagerComponent, Tasks);
}

void UDeliveryTaskManagerComponent::BeginPlay()
{
	Super::BeginPlay();

	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	Tasks.Reset();
	for (UDeliveryTaskDefinition* Definition : TaskDefinitions)
	{
		if (!Definition)
		{
			continue;
		}

		FDeliveryTaskState& State = Tasks.AddDefaulted_GetRef();
		State.Definition = Definition;
		State.Status = EDeliveryTaskStatus::Locked;
	}

	// 开局没有前置条件的任务在这里就解锁并打来电
	ReevaluateUnlocks();
}

UDeliveryTaskManagerComponent* UDeliveryTaskManagerComponent::Get(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	AGameStateBase* GameState = World ? World->GetGameState() : nullptr;

	return GameState ? GameState->FindComponentByClass<UDeliveryTaskManagerComponent>() : nullptr;
}

float UDeliveryTaskManagerComponent::GetServerTimeSeconds() const
{
	if (const AGameStateBase* GameState = Cast<AGameStateBase>(GetOwner()))
	{
		return GameState->GetServerWorldTimeSeconds();
	}

	return GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
}

UDeliveryPhoneCallQueueComponent* UDeliveryTaskManagerComponent::GetPhoneQueue() const
{
	return GetOwner() ? GetOwner()->FindComponentByClass<UDeliveryPhoneCallQueueComponent>() : nullptr;
}

FDeliveryTaskState* UDeliveryTaskManagerComponent::FindState(const UDeliveryTaskDefinition* Task)
{
	return Tasks.FindByPredicate([Task](const FDeliveryTaskState& State) { return State.Definition == Task; });
}

const FDeliveryTaskState* UDeliveryTaskManagerComponent::FindState(const UDeliveryTaskDefinition* Task) const
{
	return Tasks.FindByPredicate([Task](const FDeliveryTaskState& State) { return State.Definition == Task; });
}

void UDeliveryTaskManagerComponent::SetTaskStatus(FDeliveryTaskState& State, EDeliveryTaskStatus NewStatus)
{
	if (State.Status == NewStatus)
	{
		return;
	}

	State.Status = NewStatus;
	KnownStates.Add(State.Definition, State);

	UE_LOG(LogDelivery, Log, TEXT("[Task] %s -> %d"), *State.Definition->TaskId.ToString(), static_cast<int32>(NewStatus));

	OnTaskStatusChanged.Broadcast(State.Definition, NewStatus);
}

void UDeliveryTaskManagerComponent::ReevaluateUnlocks()
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	// 数组顺序即解锁顺序，也就是同时解锁时的来电顺序
	for (FDeliveryTaskState& State : Tasks)
	{
		if (State.Status != EDeliveryTaskStatus::Locked || !State.Definition)
		{
			continue;
		}

		if (!State.Definition->AreUnlockConditionsMet(this))
		{
			continue;
		}

		SetTaskStatus(State, EDeliveryTaskStatus::AwaitingPickup);

		if (UDeliveryPhoneCallQueueComponent* Phone = GetPhoneQueue())
		{
			Phone->EnqueueCall(State.Definition, EDeliveryPhoneCallType::TaskUnlocked);
		}
	}
}

bool UDeliveryTaskManagerComponent::CanAcquireItem(const UDeliveryTaskDefinition* Task) const
{
	const FDeliveryTaskState* State = FindState(Task);
	if (!State)
	{
		return false;
	}

	// 进行中任务自己的快递随时可以再捡起来：掉落、换手、从后备箱取出都走这里
	if (State->Status == EDeliveryTaskStatus::InProgress)
	{
		return true;
	}

	if (State->Status != EDeliveryTaskStatus::AwaitingPickup)
	{
		return false;
	}

	// 已经有任务在进行时，别的任务的快递一律拿不起来
	return GetActiveTask() == nullptr;
}

bool UDeliveryTaskManagerComponent::TryAcquireItem(UDeliveryTaskDefinition* Task, APlayerState* Player)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !Task)
	{
		return false;
	}

	FDeliveryTaskState* State = FindState(Task);
	if (!State || !CanAcquireItem(Task))
	{
		return false;
	}

	// 已经在进行中：这只是一次换手或捡回，计时不重置，也不算重新接取
	if (State->Status == EDeliveryTaskStatus::InProgress)
	{
		return false;
	}

	State->StartServerTime = GetServerTimeSeconds();
	State->CompleteServerTime = 0.f;
	State->bOverdueCallPlayed = false;
	State->SpecialEvents.Reset();
	State->Deliverer = nullptr;
	SetTaskStatus(*State, EDeliveryTaskStatus::InProgress);

	// 到点只打一次催促电话，任务不会因此结束，之后转正计时
	if (Task->TimeLimitSeconds > 0.f && GetWorld())
	{
		GetWorld()->GetTimerManager().SetTimer(
			OverdueTimerHandle,
			FTimerDelegate::CreateUObject(this, &UDeliveryTaskManagerComponent::HandleOverdue, Task),
			Task->TimeLimitSeconds,
			false);
	}

	UE_LOG(LogDelivery, Log, TEXT("[Task] %s 接取，取件玩家 %s"),
		*Task->TaskId.ToString(), Player ? *Player->GetPlayerName() : TEXT("None"));

	return true;
}

bool UDeliveryTaskManagerComponent::TryCompleteDelivery(UDeliveryTaskDefinition* Task, APlayerState* Deliverer)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !Task)
	{
		return false;
	}

	FDeliveryTaskState* State = FindState(Task);
	if (!State || State->Status != EDeliveryTaskStatus::InProgress)
	{
		return false;
	}

	State->CompleteServerTime = GetServerTimeSeconds();
	State->Deliverer = Deliverer;

	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(OverdueTimerHandle);
	}

	const float Elapsed = FMath::Max(0.f, State->CompleteServerTime - State->StartServerTime);
	const FDeliveryRewardBreakdown Reward = EvaluateReward(Task, Elapsed, State->SpecialEvents);

	SetTaskStatus(*State, EDeliveryTaskStatus::Completed);
	OnTaskCompleted.Broadcast(Task, Reward, Deliverer);

	UE_LOG(LogDelivery, Log, TEXT("[Task] %s 完成，用时 %.1fs，奖励 %d，交付玩家 %s"),
		*Task->TaskId.ToString(), Elapsed, Reward.FinalReward, Deliverer ? *Deliverer->GetPlayerName() : TEXT("None"));

	// 后续任务的解锁条件可能刚好被这次完成满足，立刻进电话队列
	ReevaluateUnlocks();

	return true;
}

void UDeliveryTaskManagerComponent::ReportSpecialEvent(UDeliveryTaskDefinition* Task, FGameplayTag EventTag)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !EventTag.IsValid())
	{
		return;
	}

	FDeliveryTaskState* State = FindState(Task);
	if (!State || State->Status != EDeliveryTaskStatus::InProgress)
	{
		return;
	}

	// 同一个事件重复上报只算一次，免得沿途反复触发把倍率叠爆
	State->SpecialEvents.AddUnique(EventTag);
}

void UDeliveryTaskManagerComponent::HandleOverdue(UDeliveryTaskDefinition* Task)
{
	FDeliveryTaskState* State = FindState(Task);
	if (!State || State->Status != EDeliveryTaskStatus::InProgress || State->bOverdueCallPlayed)
	{
		return;
	}

	State->bOverdueCallPlayed = true;

	if (UDeliveryPhoneCallQueueComponent* Phone = GetPhoneQueue())
	{
		Phone->EnqueueCall(Task, EDeliveryPhoneCallType::Overdue);
	}

	OnTaskOverdue.Broadcast(Task);
}

EDeliveryTaskStatus UDeliveryTaskManagerComponent::GetTaskStatus(const UDeliveryTaskDefinition* Task) const
{
	const FDeliveryTaskState* State = FindState(Task);

	return State ? State->Status : EDeliveryTaskStatus::Locked;
}

UDeliveryTaskDefinition* UDeliveryTaskManagerComponent::GetActiveTask() const
{
	for (const FDeliveryTaskState& State : Tasks)
	{
		if (State.Status == EDeliveryTaskStatus::InProgress)
		{
			return State.Definition;
		}
	}

	return nullptr;
}

void UDeliveryTaskManagerComponent::GetTasksByStatus(EDeliveryTaskStatus Status, TArray<UDeliveryTaskDefinition*>& OutTasks) const
{
	OutTasks.Reset();

	for (const FDeliveryTaskState& State : Tasks)
	{
		if (State.Status == Status && State.Definition)
		{
			OutTasks.Add(State.Definition);
		}
	}
}

FDeliveryTaskTimeSnapshot UDeliveryTaskManagerComponent::GetTimeSnapshot(const UDeliveryTaskDefinition* Task) const
{
	FDeliveryTaskTimeSnapshot Snapshot;

	const FDeliveryTaskState* State = FindState(Task);
	if (!State || !State->Definition)
	{
		return Snapshot;
	}

	if (State->Status == EDeliveryTaskStatus::InProgress)
	{
		Snapshot.bRunning = true;
		Snapshot.ElapsedSeconds = FMath::Max(0.f, GetServerTimeSeconds() - State->StartServerTime);
	}
	else if (State->Status == EDeliveryTaskStatus::Completed)
	{
		Snapshot.ElapsedSeconds = FMath::Max(0.f, State->CompleteServerTime - State->StartServerTime);
	}
	else
	{
		// 未取件不计时
		return Snapshot;
	}

	Snapshot.RemainingSeconds = State->Definition->TimeLimitSeconds - Snapshot.ElapsedSeconds;
	Snapshot.bOverdue = Snapshot.RemainingSeconds <= 0.f;
	Snapshot.Urgency = State->Definition->GetUrgency(Snapshot.RemainingSeconds);

	return Snapshot;
}

FDeliveryRewardBreakdown UDeliveryTaskManagerComponent::EvaluateReward(const UDeliveryTaskDefinition* Task, float ElapsedSeconds, const TArray<FGameplayTag>& SpecialEvents) const
{
	FDeliveryRewardBreakdown Result;
	if (!Task)
	{
		return Result;
	}

	Result.BaseReward = Task->BaseReward;
	Result.ElapsedSeconds = ElapsedSeconds;
	Result.bOverdue = ElapsedSeconds > Task->TimeLimitSeconds;
	Result.SpecialEvents = SpecialEvents;

	if (const FDeliveryTimeGrade* Grade = Task->FindTimeGrade(ElapsedSeconds))
	{
		Result.TimeMultiplier = Grade->Multiplier;
		Result.TimeGradeName = Grade->GradeName;
	}
	else
	{
		Result.TimeMultiplier = Task->OvertimeMultiplier;
	}

	for (const FDeliverySpecialEventRule& Rule : Task->SpecialEventRules)
	{
		if (SpecialEvents.Contains(Rule.EventTag))
		{
			Result.SpecialMultiplier *= Rule.Multiplier;
		}
	}

	Result.FinalReward = FMath::RoundToInt(Result.BaseReward * Result.TimeMultiplier * Result.SpecialMultiplier);

	return Result;
}

FDeliveryRewardBreakdown UDeliveryTaskManagerComponent::PreviewReward(const UDeliveryTaskDefinition* Task) const
{
	const FDeliveryTaskState* State = FindState(Task);
	if (!State)
	{
		return FDeliveryRewardBreakdown();
	}

	return EvaluateReward(Task, GetTimeSnapshot(Task).ElapsedSeconds, State->SpecialEvents);
}

void UDeliveryTaskManagerComponent::OnRep_Tasks()
{
	// 数组整体复制下来，跟上一次的快照逐个比对，补广播出跟服务器一致的事件
	for (const FDeliveryTaskState& State : Tasks)
	{
		if (!State.Definition)
		{
			continue;
		}

		const FDeliveryTaskState* Known = KnownStates.Find(State.Definition);
		const bool bStatusChanged = !Known || Known->Status != State.Status;
		const bool bJustOverdue = State.bOverdueCallPlayed && (!Known || !Known->bOverdueCallPlayed);

		KnownStates.Add(State.Definition, State);

		if (bStatusChanged)
		{
			OnTaskStatusChanged.Broadcast(State.Definition, State.Status);

			if (State.Status == EDeliveryTaskStatus::Completed)
			{
				const float Elapsed = FMath::Max(0.f, State.CompleteServerTime - State.StartServerTime);
				OnTaskCompleted.Broadcast(State.Definition, EvaluateReward(State.Definition, Elapsed, State.SpecialEvents), State.Deliverer);
			}
		}

		if (bJustOverdue)
		{
			OnTaskOverdue.Broadcast(State.Definition);
		}
	}
}
