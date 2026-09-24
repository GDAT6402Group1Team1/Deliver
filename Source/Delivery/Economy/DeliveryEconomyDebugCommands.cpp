// Copyright Epic Games, Inc. All Rights Reserved.

#include "Delivery.h"
#include "DeliveryWalletComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Task/DeliveryLocationRegistry.h"
#include "Task/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskTrackerComponent.h"

// 打包版里不需要这些命令，也不该让玩家能白给自己发钱
#if !UE_BUILD_SHIPPING

namespace
{
	APlayerController* FirstLocalController(UWorld* World)
	{
		return World ? World->GetFirstPlayerController() : nullptr;
	}

	UWorld* ResolveWorld(UWorld* InWorld)
	{
		if (InWorld)
		{
			return InWorld;
		}

		return GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
	}
}

static FAutoConsoleCommandWithWorldAndArgs GDeliveryWalletDump(
	TEXT("Delivery.Wallet.Dump"),
	TEXT("打印本地玩家的钱包余额。"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& /*Args*/, UWorld* InWorld)
		{
			UWorld* World = ResolveWorld(InWorld);
			APlayerController* Controller = FirstLocalController(World);
			UDeliveryWalletComponent* Wallet = UDeliveryWalletComponent::FindWallet(Controller);

			if (!Wallet)
			{
				UE_LOG(LogDelivery, Warning, TEXT("[Wallet] 找不到钱包组件（PlayerState 还没就绪？）"));

				return;
			}

			UE_LOG(LogDelivery, Log, TEXT("[Wallet] 余额 %d"), Wallet->GetBalance());
		}));

static FAutoConsoleCommandWithWorldAndArgs GDeliveryWalletAdd(
	TEXT("Delivery.Wallet.Add"),
	TEXT("给本地玩家加钱：Delivery.Wallet.Add <金额>（负数为扣款）。只在服务器/单机有效。"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* InWorld)
		{
			UWorld* World = ResolveWorld(InWorld);
			APlayerController* Controller = FirstLocalController(World);
			UDeliveryWalletComponent* Wallet = UDeliveryWalletComponent::FindWallet(Controller);

			if (!Wallet)
			{
				UE_LOG(LogDelivery, Warning, TEXT("[Wallet] 找不到钱包组件"));

				return;
			}

			const int32 Amount = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 100;
			Wallet->AddBalance(Amount);

			UE_LOG(LogDelivery, Log, TEXT("[Wallet] 加了 %d，现在余额 %d"), Amount, Wallet->GetBalance());
		}));

static FAutoConsoleCommandWithWorldAndArgs GDeliveryLocationsDump(
	TEXT("Delivery.Locations.Dump"),
	TEXT("列出关卡里注册了哪些地点 ID，以及当前追踪任务的目的地解析结果。"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& /*Args*/, UWorld* InWorld)
		{
			UWorld* World = ResolveWorld(InWorld);
			const UDeliveryLocationRegistry* Registry = UDeliveryLocationRegistry::Get(World);

			if (!Registry)
			{
				UE_LOG(LogDelivery, Warning, TEXT("[Location] 拿不到注册表"));

				return;
			}

			const TArray<FName> Ids = Registry->GetRegisteredIds();
			UE_LOG(LogDelivery, Log, TEXT("[Location] 关卡里注册了 %d 个地点："), Ids.Num());
			for (const FName& Id : Ids)
			{
				FVector Location = FVector::ZeroVector;
				Registry->ResolveLocation(Id, Location);
				UE_LOG(LogDelivery, Log, TEXT("    %s  (%.0f, %.0f, %.0f)"),
					*Id.ToString(), Location.X, Location.Y, Location.Z);
			}

			if (Ids.Num() == 0)
			{
				UE_LOG(LogDelivery, Warning,
					TEXT("[Location] 一个都没有。关卡里的取件点/收件点上要挂 DeliveryLocationComponent "
						 "并填上和 Tasks.csv 里一致的 ID。"));
			}

			// 顺带验证一遍"当前追踪的任务能不能解析出目的地"，
			// 这正是地图指引要问的问题
			APlayerController* Controller = FirstLocalController(World);
			APlayerState* State = Controller ? Controller->PlayerState : nullptr;
			const UDeliveryTaskTrackerComponent* Tracker =
				State ? State->FindComponentByClass<UDeliveryTaskTrackerComponent>() : nullptr;

			if (!Tracker)
			{
				return;
			}

			const UDeliveryTaskDefinition* Task = Tracker->GetTrackedTask();
			if (!Task)
			{
				UE_LOG(LogDelivery, Log, TEXT("[Location] 当前没有追踪任何任务"));

				return;
			}

			FVector Destination = FVector::ZeroVector;
			FName LocationId = NAME_None;
			if (Tracker->GetTrackedTaskDestination(Destination, LocationId))
			{
				UE_LOG(LogDelivery, Log, TEXT("[Location] 追踪中的 %s → 地点 %s (%.0f, %.0f, %.0f)"),
					*Task->TaskId.ToString(), *LocationId.ToString(),
					Destination.X, Destination.Y, Destination.Z);
			}
			else
			{
				UE_LOG(LogDelivery, Warning, TEXT("[Location] 追踪中的 %s 解析不出目的地，原因见上一条警告"),
					*Task->TaskId.ToString());
			}
		}));

#endif // !UE_BUILD_SHIPPING
