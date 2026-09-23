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
