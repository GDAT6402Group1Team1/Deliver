// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryItemComponent.h"

#include "Task/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"

UDeliveryItemComponent::UDeliveryItemComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

bool UDeliveryItemComponent::CanBeAcquired() const
{
	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);

	return Manager && Manager->CanAcquireItem(OwningTask);
}

FText UDeliveryItemComponent::GetPickupBlockedReason() const
{
	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);
	if (!OwningTask) return NSLOCTEXT("DeliveryPickup", "NoTask", "快递未配置任务");
	if (!Manager) return NSLOCTEXT("DeliveryPickup", "NoManager", "当前关卡未配置任务管理器");
	if (!Manager->IsTaskRegistered(OwningTask))
		return NSLOCTEXT("DeliveryPickup", "NotRegistered", "当前关卡未登记这份快递任务");
	if (Manager->CanAcquireItem(OwningTask)) return FText::GetEmpty();
	if (Manager->GetTaskStatus(OwningTask) == EDeliveryTaskStatus::Completed)
		return NSLOCTEXT("DeliveryPickup", "Completed", "这份快递的任务已完成");
	if (Manager->GetTaskStatus(OwningTask) == EDeliveryTaskStatus::Locked)
		return NSLOCTEXT("DeliveryPickup", "Locked", "任务尚未解锁，请等待任务来电");
	return NSLOCTEXT("DeliveryPickup", "OtherTask", "请先完成正在进行的快递任务");
}

bool UDeliveryItemComponent::NotifyAcquired(APlayerState* Player)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return false;
	}

	UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);

	return Manager && Manager->TryAcquireItem(OwningTask, Player);
}

void UDeliveryItemComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 只管"这个 Actor 被销毁了"。关卡切换、退出游戏、PIE 结束时整张图都在拆，
	// 那时候改任务状态没有意义，而且 GameState 可能已经先一步没了
	const bool bActorDestroyed = EndPlayReason == EEndPlayReason::Destroyed;

	if (bReportLostOnDestroy && bActorDestroyed && GetOwner() && GetOwner()->HasAuthority())
	{
		// 交付成功那条路上，TryDeliver 是先把任务标成已完成、再销毁快递的，
		// 所以走到这里时状态已经不是进行中，NotifyItemLost 会自己返回 false。
		// 这个先后顺序就是"正常交付"和"意外丢失"的判据，不需要额外加标志位
		if (UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this))
		{
			Manager->NotifyItemLost(OwningTask);
		}
	}

	Super::EndPlay(EndPlayReason);
}
