// Copyright Epic Games, Inc. All Rights Reserved.

/**
 * 管理任务解锁、接听、取件和交付
 * 接听时记录服务器时间，之后用时间差计算剩余时长，掉件和换手不会重置计时
 * 同一时间只允许一个任务计时
 * 客户端接收状态后比较前后变化，再通知界面更新
 */

#include "Task/DeliveryTaskManagerComponent.h"

#include "Delivery.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"
#include "Phone/DeliveryPhoneCallQueueComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "TimerManager.h"

UDeliveryTaskManagerComponent::UDeliveryTaskManagerComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

// 同步共享任务状态，具体配置仍由任务资产提供
void UDeliveryTaskManagerComponent::GetLifetimeReplicatedProps(
    TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UDeliveryTaskManagerComponent, Tasks);
}

// 初始化任务与电话队列

// 为配置的任务建立运行状态，并检查初始解锁条件
void UDeliveryTaskManagerComponent::BeginPlay()
{
	Super::BeginPlay();

	// 两端都记，虽然目前只有服务器上的解锁判断用得到
	LevelStartServerTime = GetServerTimeSeconds();

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

// 从当前世界的游戏状态取得共享任务管理器
UDeliveryTaskManagerComponent* UDeliveryTaskManagerComponent::Get(const UObject* WorldContextObject)
{
	const UWorld* World =
	    GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
		        : nullptr;
	AGameStateBase* GameState = World ? World->GetGameState() : nullptr;

	return GameState ? GameState->FindComponentByClass<UDeliveryTaskManagerComponent>() : nullptr;
}

float UDeliveryTaskManagerComponent::GetTimeSinceLevelStart() const
{
	return FMath::Max(0.f, GetServerTimeSeconds() - LevelStartServerTime);
}

// 读取任务计时使用的服务器时间
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

// 按任务配置查找对应的运行状态
FDeliveryTaskState* UDeliveryTaskManagerComponent::FindState(const UDeliveryTaskDefinition* Task)
{
	return Tasks.FindByPredicate(
	    [Task](const FDeliveryTaskState& State)
	    {
		    return State.Definition == Task;
	    });
}

// 按任务配置查找对应的运行状态
const FDeliveryTaskState* UDeliveryTaskManagerComponent::FindState(const UDeliveryTaskDefinition* Task) const
{
	return Tasks.FindByPredicate(
	    [Task](const FDeliveryTaskState& State)
	    {
		    return State.Definition == Task;
	    });
}

// 任务状态与解锁

// 更新任务状态，并通知服务器上的监听者
void UDeliveryTaskManagerComponent::SetTaskStatus(FDeliveryTaskState& State, EDeliveryTaskStatus NewStatus)
{
	if (State.Status == NewStatus)
	{
		return;
	}

	State.Status = NewStatus;
	KnownStates.Add(State.Definition, State);

	UE_LOG(LogDelivery, Log, TEXT("[Task] %s -> %d"), *State.Definition->TaskId.ToString(),
	       static_cast<int32>(NewStatus));

	OnTaskStatusChanged.Broadcast(State.Definition, NewStatus);
}

// 检查未解锁任务，满足条件后安排来电
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

		// 解锁只到 Offered；接听才进 AwaitingPickup——
		// 在这里就设成待取件的话，铃一响整套取货引导就亮了
		SetTaskStatus(State, EDeliveryTaskStatus::Offered);

		if (UDeliveryPhoneCallQueueComponent* Phone = GetPhoneQueue())
		{
			Phone->EnqueueCall(State.Definition, EDeliveryPhoneCallType::TaskUnlocked);
		}
	}

	UpdateUnlockPolling();
}

// 仍有待解锁任务时定时检查，全部处理后停止计时器
void UDeliveryTaskManagerComponent::UpdateUnlockPolling()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const bool bHasLockedTask = Tasks.ContainsByPredicate(
	    [](const FDeliveryTaskState& State)
	    {
		    return State.Status == EDeliveryTaskStatus::Locked;
	    });

	if (!bHasLockedTask)
	{
		World->GetTimerManager().ClearTimer(UnlockPollTimerHandle);
		return;
	}

	if (!World->GetTimerManager().IsTimerActive(UnlockPollTimerHandle))
	{
		// 一秒一次足够：解锁条件是"到点了没"这种粒度，没必要每帧查
		World->GetTimerManager().SetTimer(
		    UnlockPollTimerHandle,
		    FTimerDelegate::CreateUObject(this, &UDeliveryTaskManagerComponent::ReevaluateUnlocks), 1.f,
		    true);
	}
}

// 取件条件与接单计时

// 判断任务快递现在能否领取或重新捡回
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

	// 状态是 AwaitingPickup 就已经意味着"接过电话"了（Offered 才是没接的那档），
	// 所以这里不用再额外判一个 bool
	const UDeliveryTaskDefinition* TimedTask = GetTimedTask();
	return TimedTask == nullptr || TimedTask == Task;
}

// 判断任务是否已经接听并开始计时
bool UDeliveryTaskManagerComponent::IsTaskAccepted(const UDeliveryTaskDefinition* Task) const
{
	const FDeliveryTaskState* State = FindState(Task);

	// bTimerStarted 只由 AnswerCurrentCall 置上，所以它就等于"按过接听"
	return State && State->bTimerStarted;
}

// 接听任务电话后开始计时，并进入待取件状态
bool UDeliveryTaskManagerComponent::StartTaskTimerFromAcceptedCall(UDeliveryTaskDefinition* Task)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !Task)
	{
		return false;
	}

	FDeliveryTaskState* State = FindState(Task);
	if (!State || State->Status != EDeliveryTaskStatus::Offered)
	{
		return false;
	}

	// 重复接听不能重置任务开始时间
	if (State->bTimerStarted)
	{
		return true;
	}

	if (const UDeliveryTaskDefinition* TimedTask = GetTimedTask())
	{
		return TimedTask == Task;
	}

	State->bTimerStarted = true;
	State->StartServerTime = GetServerTimeSeconds();
	State->CompleteServerTime = 0.f;
	State->bOverdueCallPlayed = false;
	State->SpecialEvents.Reset();
	State->Deliverer = nullptr;
	KnownStates.Add(State->Definition, *State);

	// 接听 = 正式接单，状态在这里才推进到待取件
	// SetTaskStatus 自己会广播，所以不用再单独广播一次
	SetTaskStatus(*State, EDeliveryTaskStatus::AwaitingPickup);
	GetOwner()->ForceNetUpdate();

	if (Task->TimeLimitSeconds > 0.f && GetWorld())
	{
		GetWorld()->GetTimerManager().SetTimer(
		    OverdueTimerHandle,
		    FTimerDelegate::CreateUObject(this, &UDeliveryTaskManagerComponent::HandleOverdue, Task),
		    Task->TimeLimitSeconds, false);
	}

	UE_LOG(LogDelivery, Log, TEXT("[Task] %s 已接听，开始计时"), *Task->TaskId.ToString());
	return true;
}

bool UDeliveryTaskManagerComponent::IsTaskRegistered(const UDeliveryTaskDefinition* Task) const
{
	return Task && FindState(Task) != nullptr;
}

// 取件与交付结算

// 确认任务允许领取后进入配送中，保留接听时的计时
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

	// 不再有"取件时兜底开始计时"那条路：计时只能由接听电话开启
	// CanAcquireItem 已经夹过一次，这里是服务器侧的第二道
	if (!State->bTimerStarted)
	{
		return false;
	}

	SetTaskStatus(*State, EDeliveryTaskStatus::InProgress);

	UE_LOG(LogDelivery, Log, TEXT("[Task] %s 已取件，玩家 %s（计时未重置）"), *Task->TaskId.ToString(),
	       Player ? *Player->GetPlayerName() : TEXT("None"));

	return true;
}

// 确认任务正在配送后完成结算，并检查后续任务解锁
bool UDeliveryTaskManagerComponent::TryCompleteDelivery(UDeliveryTaskDefinition* Task,
                                                        APlayerState* Deliverer)
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

	// 先标记完成再通知结算，避免回调中再次交付同一任务
	SetTaskStatus(*State, EDeliveryTaskStatus::Completed);
	OnTaskCompleted.Broadcast(Task, Reward, Deliverer);

	UE_LOG(LogDelivery, Log, TEXT("[Task] %s 完成，用时 %.1fs，奖励 %d，交付玩家 %s"),
	       *Task->TaskId.ToString(), Elapsed, Reward.FinalReward,
	       Deliverer ? *Deliverer->GetPlayerName() : TEXT("None"));

	// 后续任务的解锁条件可能刚好被这次完成满足，立刻进电话队列
	ReevaluateUnlocks();
	if (UDeliveryPhoneCallQueueComponent* Phone = GetPhoneQueue())
	{
		Phone->TryStartNextCall();
	}

	return true;
}

// 提供本关登记的任务配置清单
const TArray<UDeliveryTaskDefinition*>& UDeliveryTaskManagerComponent::GetAllTaskDefinitions() const
{
	// TObjectPtr<T> 和 T* 的内存布局一致，这个 reinterpret 是 UE 里的常规写法，
	// 用来避免为了一个只读清单再拷一份数组
	return reinterpret_cast<const TArray<UDeliveryTaskDefinition*>&>(TaskDefinitions);
}

// 丢件与特殊事件

// 快递意外丢失时退回待取件，保留原计时
bool UDeliveryTaskManagerComponent::NotifyItemLost(UDeliveryTaskDefinition* Task)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !Task)
	{
		return false;
	}

	FDeliveryTaskState* State = FindState(Task);
	if (!State || State->Status != EDeliveryTaskStatus::InProgress)
	{
		// 已完成的任务交付时本来就会销毁快递，不该被当成丢件
		return false;
	}

	// 只退回取件阶段；订单从接电话起就是同一次尝试，计时、催促状态和特殊事件全部保留
	State->CompleteServerTime = 0.f;
	State->Deliverer = nullptr;
	SetTaskStatus(*State, EDeliveryTaskStatus::AwaitingPickup);

	UE_LOG(LogDelivery, Warning, TEXT("[Task] %s 的快递丢失，任务退回待取件，计时继续"),
	       *Task->TaskId.ToString());

	return true;
}

// 记录影响奖励的事件，同一事件只登记一次
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

// 超时处理

// 首次超时时安排催促电话，任务本身继续进行
void UDeliveryTaskManagerComponent::HandleOverdue(UDeliveryTaskDefinition* Task)
{
	FDeliveryTaskState* State = FindState(Task);
	if (!State || !State->bTimerStarted ||
	    (State->Status != EDeliveryTaskStatus::AwaitingPickup &&
	     State->Status != EDeliveryTaskStatus::InProgress) ||
	    State->bOverdueCallPlayed)
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

// 任务状态查询

// 查询指定任务的当前状态
EDeliveryTaskStatus UDeliveryTaskManagerComponent::GetTaskStatus(const UDeliveryTaskDefinition* Task) const
{
	const FDeliveryTaskState* State = FindState(Task);

	return State ? State->Status : EDeliveryTaskStatus::Locked;
}

// 查找当前正在配送的任务
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

// 查找已经接单并计时的任务，包括待取件阶段
UDeliveryTaskDefinition* UDeliveryTaskManagerComponent::GetTimedTask() const
{
	for (const FDeliveryTaskState& State : Tasks)
	{
		if (State.bTimerStarted && (State.Status == EDeliveryTaskStatus::AwaitingPickup ||
		                            State.Status == EDeliveryTaskStatus::InProgress))
		{
			return State.Definition;
		}
	}

	return nullptr;
}

// 收集处于指定阶段的任务
void UDeliveryTaskManagerComponent::GetTasksByStatus(EDeliveryTaskStatus Status,
                                                     TArray<UDeliveryTaskDefinition*>& OutTasks) const
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

// 根据服务器时间生成任务计时结果
FDeliveryTaskTimeSnapshot UDeliveryTaskManagerComponent::GetTimeSnapshot(
    const UDeliveryTaskDefinition* Task) const
{
	FDeliveryTaskTimeSnapshot Snapshot;

	const FDeliveryTaskState* State = FindState(Task);
	if (!State || !State->Definition)
	{
		return Snapshot;
	}

	if ((State->Status == EDeliveryTaskStatus::AwaitingPickup ||
	     State->Status == EDeliveryTaskStatus::InProgress) &&
	    State->bTimerStarted)
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
		// 还没接听任务电话，也没有通过取件兜底开始计时
		return Snapshot;
	}

	Snapshot.RemainingSeconds = State->Definition->TimeLimitSeconds - Snapshot.ElapsedSeconds;
	Snapshot.bOverdue = Snapshot.RemainingSeconds <= 0.f;
	Snapshot.Urgency = State->Definition->GetUrgency(Snapshot.RemainingSeconds);

	return Snapshot;
}

// 奖励计算与预览

// 根据用时和特殊事件计算奖励明细
FDeliveryRewardBreakdown UDeliveryTaskManagerComponent::EvaluateReward(
    const UDeliveryTaskDefinition* Task, float ElapsedSeconds,
    const TArray<FGameplayTag>& SpecialEvents) const
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

	// 档位按剩余时间表达，超时就是负的剩余时间，所以超时分档不需要额外分支
	if (const FDeliveryTimeGrade* Grade = Task->FindTimeGrade(Task->TimeLimitSeconds - ElapsedSeconds))
	{
		Result.TimeMultiplier = Grade->Multiplier;
		Result.TimeGradeName = Grade->GradeName;
	}

	// 命中的特殊事件倍率相乘，再与基础奖励和时间倍率一起结算
	for (const FDeliverySpecialEventRule& Rule : Task->SpecialEventRules)
	{
		if (SpecialEvents.Contains(Rule.EventTag))
		{
			Result.SpecialMultiplier *= Rule.Multiplier;
		}
	}

	Result.FinalReward =
	    FMath::RoundToInt(Result.BaseReward * Result.TimeMultiplier * Result.SpecialMultiplier);

	return Result;
}

// 将剩余秒数转换为倒计时文字，超时用加号显示
FText UDeliveryTaskManagerComponent::FormatCountdown(float RemainingSeconds)
{
	const bool bOverdue = RemainingSeconds < 0.f;

	// 倒计时向上取整：还剩 0.4 秒也要显示 00:01，归零和真的到点对上
	// 超时后的正计时向下取整：刚过线显示 +00:00，走满一秒才变 +00:01
	const int32 TotalSeconds =
	    bOverdue ? FMath::FloorToInt(-RemainingSeconds) : FMath::CeilToInt(RemainingSeconds);

	return FText::FromString(FString::Printf(TEXT("%s%02d:%02d"), bOverdue ? TEXT("+") : TEXT(""),
	                                         TotalSeconds / 60, TotalSeconds % 60));
}

// 根据当前进度估算现在交付可获得的奖励
FDeliveryRewardBreakdown UDeliveryTaskManagerComponent::PreviewReward(
    const UDeliveryTaskDefinition* Task) const
{
	const FDeliveryTaskState* State = FindState(Task);
	if (!State)
	{
		return FDeliveryRewardBreakdown();
	}

	return EvaluateReward(Task, GetTimeSnapshot(Task).ElapsedSeconds, State->SpecialEvents);
}

// 客户端任务更新

// 比较新旧任务状态，在客户端补发变化通知
void UDeliveryTaskManagerComponent::OnRep_Tasks()
{
	/**
	 * 首次同步只刷新当前任务状态，不补播完成和超时事件
	 * 避免中途加入的玩家看到进入游戏前已经发生的结算提示
	 */
	const bool bInitialSync = !bReceivedInitialState;
	bReceivedInitialState = true;

	// 跟上一次的快照逐个比对，补广播出跟服务器一致的事件
	for (const FDeliveryTaskState& State : Tasks)
	{
		if (!State.Definition)
		{
			continue;
		}

		const FDeliveryTaskState* Known = KnownStates.Find(State.Definition);
		const bool bStatusChanged = !Known || Known->Status != State.Status;
		const bool bTimerJustStarted = State.bTimerStarted && (!Known || !Known->bTimerStarted);
		const bool bJustOverdue = State.bOverdueCallPlayed && (!Known || !Known->bOverdueCallPlayed);

		KnownStates.Add(State.Definition, State);

		if (bStatusChanged || bTimerJustStarted)
		{
			OnTaskStatusChanged.Broadcast(State.Definition, State.Status);

			if (!bInitialSync && State.Status == EDeliveryTaskStatus::Completed)
			{
				const float Elapsed = FMath::Max(0.f, State.CompleteServerTime - State.StartServerTime);
				OnTaskCompleted.Broadcast(State.Definition,
				                          EvaluateReward(State.Definition, Elapsed, State.SpecialEvents),
				                          State.Deliverer);
			}
		}

		if (bJustOverdue && !bInitialSync)
		{
			OnTaskOverdue.Broadcast(State.Definition);
		}
	}
}
