// Copyright Epic Games, Inc. All Rights Reserved.

/*
任务系统的测试用控制台命令。

交互和背包系统还没有，没有这几条命令的话，光是想跑一遍"解锁→取件→交付"，
就得先在蓝图里搭一套按键或碰撞逻辑去调 NotifyAcquired / TryDeliver，纯属浪费。

在 PIE 里按 ` 打开控制台：

  Delivery.Task.Dump              看全部任务的状态、倒计时、当前追踪目标
  Delivery.Task.Acquire [TaskId]  模拟取件，不填 TaskId 就取第一个待取件的任务
  Delivery.Task.Deliver           模拟把进行中的任务交付掉
  Delivery.Task.Event <Tag>       上报一个特殊事件，用来验证奖励倍率
  Delivery.Task.LoseItem          模拟快递丢失，任务退回待取件（验证丢件恢复）
  Delivery.Phone.Call [Id] [overdue]  强制打一通电话进来，测 UI 用
  Delivery.Phone.Answer           接听当前来电
  Delivery.Phone.HangUp           挂断（响铃时是拒接）

这些都是权威操作，只在有权威的那一端有效——用 Standalone 或 Play As Listen Server 跑。
选 Dedicated Server 的话客户端窗口敲了不会有反应。
*/

#if !UE_BUILD_SHIPPING

#include "Delivery.h"
#include "DeliveryPlayerController.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameplayTagContainer.h"
#include "HAL/IConsoleManager.h"
#include "Task/DeliveryPhoneCallQueueComponent.h"
#include "Task/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/DeliveryTaskTrackerComponent.h"

namespace DeliveryTaskDebug
{
	void Print(const FString& Message)
	{
		UE_LOG(LogDelivery, Display, TEXT("%s"), *Message);

		// PIE 里日志窗口经常被别的东西刷掉，屏幕上再打一份
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 8.f, FColor::Cyan, Message);
		}
	}

	const TCHAR* StatusToString(EDeliveryTaskStatus Status)
	{
		switch (Status)
		{
		case EDeliveryTaskStatus::Locked:         return TEXT("未解锁");
		case EDeliveryTaskStatus::AwaitingPickup: return TEXT("待取件");
		case EDeliveryTaskStatus::InProgress:     return TEXT("进行中");
		case EDeliveryTaskStatus::Completed:      return TEXT("已完成");
		default:                                  return TEXT("?");
		}
	}

	APlayerState* GetLocalPlayerState(UWorld* World)
	{
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;

		return PC ? PC->PlayerState : nullptr;
	}

	/**
	 * 拿管理器。三种失败原因的排查方向完全不同，所以分开报：
	 * 没在跑游戏、GameState 不是我们这个类、类对了但组件没挂上。
	 */
	UDeliveryTaskManagerComponent* GetManagerChecked(UWorld* World)
	{
		AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
		if (!GameState)
		{
			Print(TEXT("[Task] 当前世界没有 GameState —— 多半是没在 PIE 里跑。"
				"点 Play 之后在游戏窗口里按 ` 敲命令，编辑器的 Cmd 输入框用的是编辑器世界，没有 GameMode。"));
			return nullptr;
		}

		UDeliveryTaskManagerComponent* Manager = GameState->FindComponentByClass<UDeliveryTaskManagerComponent>();
		if (!Manager)
		{
			Print(FString::Printf(
				TEXT("[Task] 当前 GameState 是 %s，上面没有 TaskManager 组件。"
					"检查：① BP_DeliverGameMode 的 GameStateClass 指向了 BP_DeliverGameState 吗；"
					"② 地图 World Settings 里的 GameMode Override 用的是不是这个 GameMode。"),
				*GameState->GetClass()->GetName()));
		}

		return Manager;
	}

	void CollectAllTasks(const UDeliveryTaskManagerComponent* Manager, TArray<UDeliveryTaskDefinition*>& OutTasks)
	{
		OutTasks.Reset();

		for (EDeliveryTaskStatus Status : {
			EDeliveryTaskStatus::Locked,
			EDeliveryTaskStatus::AwaitingPickup,
			EDeliveryTaskStatus::InProgress,
			EDeliveryTaskStatus::Completed })
		{
			TArray<UDeliveryTaskDefinition*> Bucket;
			Manager->GetTasksByStatus(Status, Bucket);
			OutTasks.Append(Bucket);
		}
	}

	UDeliveryTaskDefinition* FindByTaskId(const UDeliveryTaskManagerComponent* Manager, FName TaskId)
	{
		TArray<UDeliveryTaskDefinition*> All;
		CollectAllTasks(Manager, All);

		for (UDeliveryTaskDefinition* Task : All)
		{
			if (Task && Task->TaskId == TaskId)
			{
				return Task;
			}
		}

		return nullptr;
	}

	void DumpCommand(const TArray<FString>& /*Args*/, UWorld* World)
	{
		UDeliveryTaskManagerComponent* Manager = GetManagerChecked(World);
		if (!Manager)
		{
			return;
		}

		TArray<UDeliveryTaskDefinition*> All;
		CollectAllTasks(Manager, All);

		if (All.Num() == 0)
		{
			Print(TEXT("[Task] 任务列表为空。检查 GameState 蓝图上 TaskManager 的 TaskDefinitions 填了没有。"));
			return;
		}

		for (UDeliveryTaskDefinition* Task : All)
		{
			FString Line = FString::Printf(TEXT("[Task] %-16s %s"),
				*Task->TaskId.ToString(),
				StatusToString(Manager->GetTaskStatus(Task)));

			const FDeliveryTaskTimeSnapshot Snapshot = Manager->GetTimeSnapshot(Task);
			if (Snapshot.bRunning)
			{
				Line += FString::Printf(TEXT("  %s  已用 %.0fs  奖励预览 %d"),
					*UDeliveryTaskManagerComponent::FormatCountdown(Snapshot.RemainingSeconds).ToString(),
					Snapshot.ElapsedSeconds,
					Manager->PreviewReward(Task).FinalReward);
			}

			Print(Line);
		}

		if (UDeliveryTaskTrackerComponent* Tracker = UDeliveryTaskTrackerComponent::FindTracker(GetLocalPlayerState(World)))
		{
			UDeliveryTaskDefinition* Tracked = Tracker->GetTrackedTask();
			Print(FString::Printf(TEXT("[Task] 当前追踪：%s"), Tracked ? *Tracked->TaskId.ToString() : TEXT("(无)")));
		}

		// 电话状态在有 UI 之前完全看不见，这里顺手报一下
		if (UDeliveryPhoneCallQueueComponent* Phone = UDeliveryPhoneCallQueueComponent::Get(World))
		{
			FDeliveryPhoneCall Call;
			if (Phone->GetCurrentCall(Call) && Call.Task)
			{
				Print(FString::Printf(TEXT("[Task] 手机：%s  %s（%s）剩 %.0fs，队列共 %d 通"),
					Phone->GetCallState() == EDeliveryPhoneCallState::Ringing ? TEXT("响铃中") : TEXT("通话中"),
					*Call.Task->TaskId.ToString(),
					Call.CallType == EDeliveryPhoneCallType::Overdue ? TEXT("超时催促") : TEXT("任务解锁"),
					Phone->GetStateRemainingSeconds(),
					Phone->GetPendingCallCount()));
			}
			else
			{
				Print(TEXT("[Task] 手机：待机"));
			}
		}
	}

	void AcquireCommand(const TArray<FString>& Args, UWorld* World)
	{
		UDeliveryTaskManagerComponent* Manager = GetManagerChecked(World);
		if (!Manager)
		{
			return;
		}

		UDeliveryTaskDefinition* Task = nullptr;

		if (Args.Num() > 0)
		{
			Task = FindByTaskId(Manager, FName(*Args[0]));
			if (!Task)
			{
				Print(FString::Printf(TEXT("[Task] 没有 TaskId 叫 %s 的任务。"), *Args[0]));
				return;
			}
		}
		else
		{
			TArray<UDeliveryTaskDefinition*> Awaiting;
			Manager->GetTasksByStatus(EDeliveryTaskStatus::AwaitingPickup, Awaiting);

			if (Awaiting.Num() == 0)
			{
				Print(TEXT("[Task] 没有待取件的任务。先看 Delivery.Task.Dump，可能全都还没解锁。"));
				return;
			}

			Task = Awaiting[0];
		}

		const bool bAccepted = Manager->TryAcquireItem(Task, GetLocalPlayerState(World));

		Print(FString::Printf(TEXT("[Task] 取件 %s：%s"),
			*Task->TaskId.ToString(),
			bAccepted
				? TEXT("成功，任务进入进行中，计时开始")
				: TEXT("没有接取（已有进行中任务 / 该任务未解锁 / 这一端没有权威）")));
	}

	void DeliverCommand(const TArray<FString>& /*Args*/, UWorld* World)
	{
		UDeliveryTaskManagerComponent* Manager = GetManagerChecked(World);
		if (!Manager)
		{
			return;
		}

		UDeliveryTaskDefinition* Active = Manager->GetActiveTask();
		if (!Active)
		{
			Print(TEXT("[Task] 当前没有进行中的任务，先 Delivery.Task.Acquire。"));
			return;
		}

		// 完成之后状态就变了，奖励要在交付前取
		const FDeliveryRewardBreakdown Reward = Manager->PreviewReward(Active);
		const bool bCompleted = Manager->TryCompleteDelivery(Active, GetLocalPlayerState(World));

		Print(FString::Printf(TEXT("[Task] 交付 %s：%s  基础 %d × 时间 %.2f × 事件 %.2f = %d"),
			*Active->TaskId.ToString(),
			bCompleted ? TEXT("完成") : TEXT("失败"),
			Reward.BaseReward,
			Reward.TimeMultiplier,
			Reward.SpecialMultiplier,
			Reward.FinalReward));
	}

	void CallCommand(const TArray<FString>& Args, UWorld* World)
	{
		UDeliveryTaskManagerComponent* Manager = GetManagerChecked(World);
		UDeliveryPhoneCallQueueComponent* Phone = UDeliveryPhoneCallQueueComponent::Get(World);
		if (!Manager || !Phone)
		{
			return;
		}

		// 不填 TaskId 就随便挑一个任务，反正测 UI 只关心电话本身
		UDeliveryTaskDefinition* Task = nullptr;
		if (Args.Num() > 0)
		{
			Task = FindByTaskId(Manager, FName(*Args[0]));
			if (!Task)
			{
				Print(FString::Printf(TEXT("[Task] 没有 TaskId 叫 %s 的任务。"), *Args[0]));
				return;
			}
		}
		else
		{
			TArray<UDeliveryTaskDefinition*> All;
			CollectAllTasks(Manager, All);
			if (All.Num() == 0)
			{
				Print(TEXT("[Task] 一个任务都没有，没法打电话。"));
				return;
			}

			Task = All[0];
		}

		const bool bOverdue = Args.Num() > 1 && Args[1].Equals(TEXT("overdue"), ESearchCase::IgnoreCase);
		const EDeliveryPhoneCallType CallType = bOverdue ? EDeliveryPhoneCallType::Overdue : EDeliveryPhoneCallType::TaskUnlocked;

		const int32 CountBefore = Phone->GetPendingCallCount();
		Phone->EnqueueCall(Task, CallType);

		if (Phone->GetPendingCallCount() == CountBefore)
		{
			// EnqueueCall 会去重，同一通已经在队列里就不会再排一次
			Print(FString::Printf(TEXT("[Task] %s 的%s来电已经在队列里了，没有重复入队。"),
				*Task->TaskId.ToString(), bOverdue ? TEXT("催促") : TEXT("解锁")));
			return;
		}

		Print(FString::Printf(TEXT("[Task] 已打入 %s 的%s来电，队列共 %d 通"),
			*Task->TaskId.ToString(),
			bOverdue ? TEXT("催促") : TEXT("解锁"),
			Phone->GetPendingCallCount()));
	}

	void AnswerCommand(const TArray<FString>& /*Args*/, UWorld* World)
	{
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		if (!PC)
		{
			Print(TEXT("[Task] 没有 PlayerController，是不是没在 PIE 里跑？"));
			return;
		}

		if (ADeliveryPlayerController* DeliveryPC = Cast<ADeliveryPlayerController>(PC))
		{
			DeliveryPC->RequestAnswerCall();
			Print(TEXT("[Task] 已请求接听"));
		}
		else
		{
			Print(TEXT("[Task] 当前 PlayerController 不是 ADeliveryPlayerController 的子类"));
		}
	}

	void HangUpCommand(const TArray<FString>& /*Args*/, UWorld* World)
	{
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		if (ADeliveryPlayerController* DeliveryPC = Cast<ADeliveryPlayerController>(PC))
		{
			DeliveryPC->RequestHangUpCall();
			Print(TEXT("[Task] 已请求挂断"));
		}
	}

	void LoseItemCommand(const TArray<FString>& /*Args*/, UWorld* World)
	{
		UDeliveryTaskManagerComponent* Manager = GetManagerChecked(World);
		if (!Manager)
		{
			return;
		}

		UDeliveryTaskDefinition* Active = Manager->GetActiveTask();
		if (!Active)
		{
			Print(TEXT("[Task] 现在没有进行中的任务，没什么可丢的"));
			return;
		}

		if (Manager->NotifyItemLost(Active))
		{
			Print(FString::Printf(
				TEXT("[Task] %s 的快递已按丢失处理：退回待取件，计时重置。"
					 "用 Delivery.Task.Dump 核对状态"), *Active->TaskId.ToString()));
		}
	}

	void EventCommand(const TArray<FString>& Args, UWorld* World)
	{
		UDeliveryTaskManagerComponent* Manager = GetManagerChecked(World);
		if (!Manager)
		{
			return;
		}

		if (Args.Num() == 0)
		{
			Print(TEXT("[Task] 用法：Delivery.Task.Event <GameplayTag>"));
			return;
		}

		const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Args[0]), false);
		if (!Tag.IsValid())
		{
			Print(FString::Printf(TEXT("[Task] Tag %s 不存在，先在项目设置的 GameplayTags 里加。"), *Args[0]));
			return;
		}

		UDeliveryTaskDefinition* Active = Manager->GetActiveTask();
		if (!Active)
		{
			Print(TEXT("[Task] 当前没有进行中的任务，特殊事件只对进行中的任务记账。"));
			return;
		}

		Manager->ReportSpecialEvent(Active, Tag);

		Print(FString::Printf(TEXT("[Task] 已记录事件 %s，奖励预览 %d"),
			*Tag.ToString(),
			Manager->PreviewReward(Active).FinalReward));
	}
}

static FAutoConsoleCommandWithWorldAndArgs GDeliveryTaskDumpCmd(
	TEXT("Delivery.Task.Dump"),
	TEXT("打印所有任务的状态、倒计时和当前追踪目标"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryTaskDebug::DumpCommand));

static FAutoConsoleCommandWithWorldAndArgs GDeliveryTaskAcquireCmd(
	TEXT("Delivery.Task.Acquire"),
	TEXT("模拟取件。可选参数 TaskId，不填则取第一个待取件的任务"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryTaskDebug::AcquireCommand));

static FAutoConsoleCommandWithWorldAndArgs GDeliveryTaskDeliverCmd(
	TEXT("Delivery.Task.Deliver"),
	TEXT("模拟交付当前进行中的任务"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryTaskDebug::DeliverCommand));

static FAutoConsoleCommandWithWorldAndArgs GDeliveryPhoneCallCmd(
	TEXT("Delivery.Phone.Call"),
	TEXT("强制打一通电话进来测 UI。可选参数：TaskId、overdue（打催促来电）"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryTaskDebug::CallCommand));

static FAutoConsoleCommandWithWorldAndArgs GDeliveryPhoneAnswerCmd(
	TEXT("Delivery.Phone.Answer"),
	TEXT("接听当前来电"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryTaskDebug::AnswerCommand));

static FAutoConsoleCommandWithWorldAndArgs GDeliveryPhoneHangUpCmd(
	TEXT("Delivery.Phone.HangUp"),
	TEXT("挂断当前电话（响铃时是拒接）"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryTaskDebug::HangUpCommand));

static FAutoConsoleCommandWithWorldAndArgs GDeliveryTaskEventCmd(
	TEXT("Delivery.Task.Event"),
	TEXT("给进行中的任务上报一个特殊事件 Tag，验证奖励倍率"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryTaskDebug::EventCommand));

static FAutoConsoleCommandWithWorldAndArgs GDeliveryTaskLoseItemCmd(
	TEXT("Delivery.Task.LoseItem"),
	TEXT("模拟进行中任务的快递丢失，任务退回待取件"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeliveryTaskDebug::LoseItemCommand));

#endif // !UE_BUILD_SHIPPING
