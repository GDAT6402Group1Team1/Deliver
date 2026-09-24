// Copyright Epic Games, Inc. All Rights Reserved.

#include "Delivery.h"
#include "DeliveryItemComponent.h"
#include "DeliveryLocationComponent.h"
#include "DeliveryLocationRegistry.h"
#include "DeliveryTargetComponent.h"
#include "DeliveryTaskDefinition.h"
#include "DeliveryTaskManagerComponent.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if !UE_BUILD_SHIPPING

/**
 * Delivery.Task.Validate —— 拿关卡里的任务清单逐条对账。
 *
 * 摆取件点/收件点的时候用：一条命令就能知道哪个任务的哪一项还没接上，
 * 比在 PIE 里一个个试快得多。所有问题都在一次输出里列全，不是遇到第一个就停——
 * 摆点是批量工作，一次修一个太慢。
 *
 * 查四件事：
 *   1. 任务定义本身填全了没（TaskId / 地点 ID）
 *   2. 地点 ID 在关卡里能不能解析出位置
 *   3. 收件点的 ExpectedTask 配没配、和任务对不对得上
 *   4. 快递物品上有没有 DeliveryItemComponent、OwningTask 指向谁
 */

namespace
{
	struct FValidationCounters
	{
		int32 Errors = 0;
		int32 Warnings = 0;
	};

	void ReportError(FValidationCounters& Counters, const FString& Message)
	{
		++Counters.Errors;
		UE_LOG(LogDelivery, Error, TEXT("[校验] %s"), *Message);
	}

	void ReportWarning(FValidationCounters& Counters, const FString& Message)
	{
		++Counters.Warnings;
		UE_LOG(LogDelivery, Warning, TEXT("[校验] %s"), *Message);
	}

	/** 关卡里所有收件点，按 ExpectedTask 归类。一个任务配了多个收件点也能看出来。 */
	TMultiMap<const UDeliveryTaskDefinition*, AActor*> CollectTargets(UWorld* World)
	{
		TMultiMap<const UDeliveryTaskDefinition*, AActor*> Result;

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UDeliveryTargetComponent*> Components;
			It->GetComponents(Components);
			for (const UDeliveryTargetComponent* Target : Components)
			{
				Result.Add(Target->ExpectedTask, *It);
			}
		}

		return Result;
	}

	/** 关卡里所有快递物品，按 OwningTask 归类。 */
	TMultiMap<const UDeliveryTaskDefinition*, AActor*> CollectItems(UWorld* World)
	{
		TMultiMap<const UDeliveryTaskDefinition*, AActor*> Result;

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UDeliveryItemComponent*> Components;
			It->GetComponents(Components);
			for (const UDeliveryItemComponent* Item : Components)
			{
				Result.Add(Item->OwningTask, *It);
			}
		}

		return Result;
	}

	void ValidateLocation(FValidationCounters& Counters, const UDeliveryLocationRegistry* Registry,
		const UDeliveryTaskDefinition* Task, FName LocationId, const TCHAR* FieldName)
	{
		const FString TaskName = Task->TaskId.ToString();

		if (LocationId.IsNone())
		{
			ReportWarning(Counters, FString::Printf(
				TEXT("%s：%s 没填。这个任务没法生成地图指引"), *TaskName, FieldName));

			return;
		}

		FVector Location = FVector::ZeroVector;
		if (!Registry || !Registry->ResolveLocation(LocationId, Location))
		{
			ReportError(Counters, FString::Printf(
				TEXT("%s：%s = \"%s\"，但关卡里没有对应的点。"
					 "需要在那个位置放一个 Actor 并挂 DeliveryLocationComponent，LocationId 填成一样的值"),
				*TaskName, FieldName, *LocationId.ToString()));
		}
	}
}

static FAutoConsoleCommandWithWorldAndArgs GDeliveryTaskValidate(
	TEXT("Delivery.Task.Validate"),
	TEXT("对账关卡里的任务配置：地点 ID 能不能解析、收件点和快递有没有接上。"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& /*Args*/, UWorld* InWorld)
		{
			UWorld* World = InWorld ? InWorld : (GEngine ? GEngine->GetCurrentPlayWorld() : nullptr);
			if (!World)
			{
				UE_LOG(LogDelivery, Error, TEXT("[校验] 拿不到世界，要在 PIE 里跑"));

				return;
			}

			const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(World);
			if (!Manager)
			{
				UE_LOG(LogDelivery, Error,
					TEXT("[校验] 拿不到任务管理器。GameState 蓝图是不是没设成 BP_DeliveryGameState？"));

				return;
			}

			const UDeliveryLocationRegistry* Registry = UDeliveryLocationRegistry::Get(World);
			const TArray<UDeliveryTaskDefinition*>& Tasks = Manager->GetAllTaskDefinitions();

			FValidationCounters Counters;

			UE_LOG(LogDelivery, Log, TEXT("================ 任务配置校验 ================"));
			UE_LOG(LogDelivery, Log, TEXT("关卡里配了 %d 个任务，注册了 %d 个地点"),
				Tasks.Num(), Registry ? Registry->GetRegisteredIds().Num() : 0);

			if (Tasks.Num() == 0)
			{
				ReportError(Counters, TEXT("GameState 蓝图的 TaskDefinitions 是空的，一个任务都没配"));
			}

			const TMultiMap<const UDeliveryTaskDefinition*, AActor*> Targets = CollectTargets(World);
			const TMultiMap<const UDeliveryTaskDefinition*, AActor*> Items = CollectItems(World);

			// 没绑任何任务的收件点/快递：摆了却忘了填 ExpectedTask / OwningTask，
			// 这种在游戏里表现为"这个点按了没反应"，很难想到是配置漏了
			TArray<AActor*> Orphans;
			Targets.MultiFind(nullptr, Orphans);
			for (const AActor* Actor : Orphans)
			{
				ReportWarning(Counters, FString::Printf(
					TEXT("收件点 %s 的 ExpectedTask 没填，永远不会接受任何交付"), *Actor->GetName()));
			}

			Orphans.Reset();
			Items.MultiFind(nullptr, Orphans);
			for (const AActor* Actor : Orphans)
			{
				ReportWarning(Counters, FString::Printf(
					TEXT("快递 %s 的 OwningTask 没填，取件时不会推进任何任务"), *Actor->GetName()));
			}

			for (const UDeliveryTaskDefinition* Task : Tasks)
			{
				if (!Task)
				{
					ReportError(Counters, TEXT("TaskDefinitions 里有一项是空的"));

					continue;
				}

				const FString TaskName = Task->TaskId.IsNone()
					? FString::Printf(TEXT("<没填 TaskId 的资产 %s>"), *Task->GetName())
					: Task->TaskId.ToString();

				if (Task->TaskId.IsNone())
				{
					ReportError(Counters, FString::Printf(
						TEXT("资产 %s 没填 TaskId。导入脚本按 TaskId 增量更新，空的会每次新建一份"),
						*Task->GetName()));
				}

				ValidateLocation(Counters, Registry, Task, Task->PickupLocationId, TEXT("PickupLocationId"));
				ValidateLocation(Counters, Registry, Task, Task->DeliveryLocationId, TEXT("DeliveryLocationId"));

				TArray<AActor*> TaskTargets;
				Targets.MultiFind(Task, TaskTargets);
				if (TaskTargets.Num() == 0)
				{
					ReportError(Counters, FString::Printf(
						TEXT("%s：关卡里没有任何收件点的 ExpectedTask 指向它，这个任务交不了"), *TaskName));
				}
				else if (TaskTargets.Num() > 1)
				{
					// 不是错误：一个收件点由几个触发体组成是合理的。
					// 但更常见的是复制 Actor 时忘了改 ExpectedTask，所以要提一句
					ReportWarning(Counters, FString::Printf(
						TEXT("%s：有 %d 个收件点指向它。确认是故意的，不是复制 Actor 时忘了改"),
						*TaskName, TaskTargets.Num()));
				}

				TArray<AActor*> TaskItems;
				Items.MultiFind(Task, TaskItems);
				if (TaskItems.Num() == 0)
				{
					ReportWarning(Counters, FString::Printf(
						TEXT("%s：关卡里没有挂着它的快递物品。如果快递是运行时生成的就正常，"
							 "手摆的话是漏了"), *TaskName));
				}

				if (Task->TimeGrades.Num() == 0)
				{
					ReportWarning(Counters, FString::Printf(
						TEXT("%s：没配时间评价档位，交付一律按底薪 %d 结算"),
						*TaskName, Task->BaseReward));
				}
			}

			UE_LOG(LogDelivery, Log, TEXT("=============================================="));
			if (Counters.Errors == 0 && Counters.Warnings == 0)
			{
				UE_LOG(LogDelivery, Log, TEXT("[校验] 全部通过"));
			}
			else
			{
				UE_LOG(LogDelivery, Log, TEXT("[校验] %d 个错误，%d 个警告"),
					Counters.Errors, Counters.Warnings);
			}
		}));

#endif // !UE_BUILD_SHIPPING
