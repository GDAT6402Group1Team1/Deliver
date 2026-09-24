// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryWalletComponent.h"

#include "DeliveryGameState.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"
#include "Task/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeliveryWallet, Log, All);

namespace
{
	/** GameState 还没就绪时的重试间隔。PlayerState 可能比 GameState 先 BeginPlay。 */
	constexpr float BindRetrySeconds = 0.5f;
}

UDeliveryWalletComponent::UDeliveryWalletComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

UDeliveryWalletComponent* UDeliveryWalletComponent::FindWallet(AActor* Actor)
{
	if (!Actor)
	{
		return nullptr;
	}

	// 直接挂在这个 Actor 上（PlayerState 自己）
	if (UDeliveryWalletComponent* Wallet = Actor->FindComponentByClass<UDeliveryWalletComponent>())
	{
		return Wallet;
	}

	// 从 Pawn / Controller 往 PlayerState 找：调用方拿到的通常是角色，
	// 让它们各写一遍 GetPlayerState() 只会到处漏判空
	if (const APawn* Pawn = Cast<APawn>(Actor))
	{
		if (APlayerState* State = Pawn->GetPlayerState())
		{
			return State->FindComponentByClass<UDeliveryWalletComponent>();
		}
	}

	if (const AController* Controller = Cast<AController>(Actor))
	{
		if (APlayerState* State = Controller->PlayerState)
		{
			return State->FindComponentByClass<UDeliveryWalletComponent>();
		}
	}

	return nullptr;
}

void UDeliveryWalletComponent::BeginPlay()
{
	Super::BeginPlay();

	if (GetOwner() && GetOwner()->HasAuthority())
	{
		Balance = FMath::Max(0, StartingBalance);
	}

	BindToTaskManager();
}

void UDeliveryWalletComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// 只给拥有者：别人的余额不该出现在你的客户端上
	DOREPLIFETIME_CONDITION(UDeliveryWalletComponent, Balance, COND_OwnerOnly);
}

void UDeliveryWalletComponent::BindToTaskManager()
{
	// 只有服务器需要订阅：入账是权威侧的事，客户端靠 Balance 复制拿结果
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	ADeliveryGameState* GameState = World->GetGameState<ADeliveryGameState>();
	UDeliveryTaskManagerComponent* Manager = GameState ? GameState->GetTaskManager() : nullptr;

	if (!Manager)
	{
		// PlayerState 可能比 GameState 先 BeginPlay，这时候订阅不到。
		// 不能就此放弃——那样这个玩家整局都收不到钱，而且完全没有报错
		World->GetTimerManager().SetTimer(
			BindRetryTimer, this, &UDeliveryWalletComponent::BindToTaskManager, BindRetrySeconds, false);

		return;
	}

	World->GetTimerManager().ClearTimer(BindRetryTimer);
	Manager->OnTaskCompleted.AddUniqueDynamic(this, &UDeliveryWalletComponent::HandleTaskCompleted);
}

void UDeliveryWalletComponent::HandleTaskCompleted(UDeliveryTaskDefinition* Task,
	const FDeliveryRewardBreakdown& Reward, APlayerState* Deliverer)
{
	// 委托是广播给所有钱包的，这里挑出"是不是我送的"。
	// 多人抢单时只有送达者拿钱，判据直接用委托带过来的 Deliverer，
	// 不需要各调用点自己去比对
	if (Deliverer != GetOwner())
	{
		return;
	}

	AddBalance(Reward.FinalReward);
	OnTaskPaid.Broadcast(Task, Reward);

	UE_LOG(LogDeliveryWallet, Log, TEXT("[Wallet] %s 送达 %s，入账 %d（底薪 %d × 时间 %.2f × 特殊 %.2f），余额 %d"),
		*GetNameSafe(Deliverer), Task ? *Task->TaskId.ToString() : TEXT("<空>"),
		Reward.FinalReward, Reward.BaseReward, Reward.TimeMultiplier, Reward.SpecialMultiplier, Balance);
}

void UDeliveryWalletComponent::AddBalance(int32 Amount)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || Amount == 0)
	{
		return;
	}

	const int32 OldBalance = Balance;
	Balance = FMath::Max(0, Balance + Amount);

	const int32 Delta = Balance - OldBalance;
	if (Delta != 0)
	{
		// 服务器上 OnRep 不会触发，得自己广播一次，否则监听侧在
		// 单机/监听服上收不到事件——这类"只有客户端有反应"的 bug 很难查
		OnBalanceChanged.Broadcast(Balance, Delta);
	}
}

bool UDeliveryWalletComponent::TrySpend(int32 Amount)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || Amount <= 0)
	{
		return false;
	}

	if (Balance < Amount)
	{
		return false;
	}

	AddBalance(-Amount);

	return true;
}

void UDeliveryWalletComponent::OnRep_Balance(int32 OldBalance)
{
	OnBalanceChanged.Broadcast(Balance, Balance - OldBalance);
}
