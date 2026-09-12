// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryTaskTrackerComponent.h"

#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"
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
