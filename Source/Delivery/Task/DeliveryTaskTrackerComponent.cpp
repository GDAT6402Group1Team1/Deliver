// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryTaskTrackerComponent.h"

#include "Delivery.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"
#include "Task/DeliveryLocationRegistry.h"
#include "Task/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "TimerManager.h"

UDeliveryTaskTrackerComponent::UDeliveryTaskTrackerComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UDeliveryTaskTrackerComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// 追踪目标只影响本人的地图引导，没必要发给其他客户端
	DOREPLIFETIME_CONDITION(UDeliveryTaskTrackerComponent, TrackedTask, COND_OwnerOnly);
}

void UDeliveryTaskTrackerComponent::BeginPlay()
{
	Super::BeginPlay();

	if (!GetOwner() || !GetOwner()->HasAuthority() || !GetWorld())
	{
		return;
	}

	GetWorld()->GetTimerManager().SetTimerForNextTick(this, &UDeliveryTaskTrackerComponent::BindToManager);
}

void UDeliveryTaskTrackerComponent::BindToManager()
{
	UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);
	if (!Manager)
	{
		return;
	}

	Manager->OnTaskStatusChanged.AddDynamic(this, &UDeliveryTaskTrackerComponent::HandleTaskStatusChanged);

	// 中途进来的玩家也要立刻拿到正确的追踪目标
	RefreshAutoSelection();
}

UDeliveryTaskTrackerComponent* UDeliveryTaskTrackerComponent::FindTracker(AActor* Actor)
{
	if (!Actor)
	{
		return nullptr;
	}

	if (APawn* Pawn = Cast<APawn>(Actor))
	{
		Actor = Pawn->GetPlayerState();
	}
	else if (AController* Controller = Cast<AController>(Actor))
	{
		Actor = Controller->PlayerState;
	}

	return Actor ? Actor->FindComponentByClass<UDeliveryTaskTrackerComponent>() : nullptr;
}

bool UDeliveryTaskTrackerComponent::CanTrackTask(const UDeliveryTaskDefinition* Task) const
{
	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);
	if (!Manager || !Task)
	{
		return false;
	}

	// 有任务在进行时，别的任务的快递本来就拿不起来，追踪它们没有意义
	if (const UDeliveryTaskDefinition* Active = Manager->GetActiveTask())
	{
		return Task == Active;
	}

	return Manager->GetTaskStatus(Task) == EDeliveryTaskStatus::AwaitingPickup;
}

void UDeliveryTaskTrackerComponent::RequestTrackTask(UDeliveryTaskDefinition* Task)
{
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		if (CanTrackTask(Task))
		{
			SetTrackedTask(Task);
		}

		return;
	}

	ServerRequestTrackTask(Task);
}

void UDeliveryTaskTrackerComponent::ServerRequestTrackTask_Implementation(UDeliveryTaskDefinition* Task)
{
	if (CanTrackTask(Task))
	{
		SetTrackedTask(Task);
	}
}

void UDeliveryTaskTrackerComponent::SetTrackedTask(UDeliveryTaskDefinition* Task)
{
	if (TrackedTask == Task)
	{
		return;
	}

	TrackedTask = Task;

	// 服务器不会走 OnRep，这里直接广播一次
	OnTrackedTaskChanged.Broadcast(TrackedTask);
}

void UDeliveryTaskTrackerComponent::RefreshAutoSelection()
{
	UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);
	if (!Manager)
	{
		return;
	}

	// 有人取件之后全世界只剩这一个任务可做，所有人的追踪都切过去
	if (UDeliveryTaskDefinition* Active = Manager->GetActiveTask())
	{
		SetTrackedTask(Active);
		return;
	}

	// 原来追踪的任务已经完成（或者刚被别人做掉了）
	if (TrackedTask && Manager->GetTaskStatus(TrackedTask) != EDeliveryTaskStatus::AwaitingPickup)
	{
		SetTrackedTask(nullptr);
	}

	// 只剩一个待取件任务时默认选中它
	if (!TrackedTask)
	{
		TArray<UDeliveryTaskDefinition*> Awaiting;
		Manager->GetTasksByStatus(EDeliveryTaskStatus::AwaitingPickup, Awaiting);

		if (Awaiting.Num() == 1)
		{
			SetTrackedTask(Awaiting[0]);
		}
	}
}

void UDeliveryTaskTrackerComponent::HandleTaskStatusChanged(UDeliveryTaskDefinition* /*Task*/, EDeliveryTaskStatus /*NewStatus*/)
{
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		RefreshAutoSelection();
	}
}

void UDeliveryTaskTrackerComponent::OnRep_TrackedTask()
{
	OnTrackedTaskChanged.Broadcast(TrackedTask);
}

bool UDeliveryTaskTrackerComponent::GetTrackedTaskDestination(FVector& OutLocation, FName& OutLocationId) const
{
	return GetTaskDestination(TrackedTask, OutLocation, OutLocationId);
}

bool UDeliveryTaskTrackerComponent::GetTaskDestination(const UDeliveryTaskDefinition* Task,
	FVector& OutLocation, FName& OutLocationId) const
{
	OutLocation = FVector::ZeroVector;
	OutLocationId = NAME_None;

	if (!Task)
	{
		return false;
	}

	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);
	if (!Manager)
	{
		return false;
	}

	// 目的地跟着状态走：还没取件就去取件点，取了就去收件点。
	// 放在这里而不是让每个 UI 各判一遍——判错了的表现是"箭头指向已经拿过的地方"，
	// 而且四个界面会各错各的
	switch (Manager->GetTaskStatus(Task))
	{
	case EDeliveryTaskStatus::AwaitingPickup:
		OutLocationId = Task->PickupLocationId;
		break;

	case EDeliveryTaskStatus::InProgress:
		OutLocationId = Task->DeliveryLocationId;
		break;

	default:
		// 未解锁的不该被指引，已完成的没有目的地
		return false;
	}

	if (OutLocationId.IsNone())
	{
		UE_LOG(LogDelivery, Warning, TEXT("[Task] %s 没有填地点 ID，无法生成指引"),
			*Task->TaskId.ToString());

		return false;
	}

	const UDeliveryLocationRegistry* Registry = UDeliveryLocationRegistry::Get(this);
	if (!Registry || !Registry->ResolveLocation(OutLocationId, OutLocation))
	{
		// 静默失败的话，表现只是"箭头不显示"，根因几乎查不到——必须出声
		UE_LOG(LogDelivery, Warning,
			TEXT("[Task] %s 的地点 ID \"%s\" 在关卡里找不到对应的点。"
				 "检查那个点上有没有挂 DeliveryLocationComponent、ID 填得对不对"),
			*Task->TaskId.ToString(), *OutLocationId.ToString());

		return false;
	}

	return true;
}
