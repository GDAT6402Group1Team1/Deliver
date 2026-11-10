// Copyright Epic Games, Inc. All Rights Reserved.

#include "Task/World/DeliveryItemComponent.h"

#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"

UDeliveryItemComponent::UDeliveryItemComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

// 向任务管理器查询这件快递是否允许领取
bool UDeliveryItemComponent::CanBeAcquired() const
{
	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);

	return Manager && Manager->CanAcquireItem(OwningTask);
}

// 给出任务快递暂时无法领取的原因
FText UDeliveryItemComponent::GetPickupBlockedReason() const
{
	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);
	if (!OwningTask)
		return NSLOCTEXT("DeliveryPickup", "NoTask", "Package Has No Delivery Assigned");
	if (!Manager)
		return NSLOCTEXT("DeliveryPickup", "NoManager", "Delivery Service Unavailable");
	if (!Manager->IsTaskRegistered(OwningTask))
		return NSLOCTEXT("DeliveryPickup", "NotRegistered", "Delivery Unavailable in This Area");
	if (Manager->CanAcquireItem(OwningTask))
		return FText::GetEmpty();
	if (Manager->GetTaskStatus(OwningTask) == EDeliveryTaskStatus::Completed)
		return NSLOCTEXT("DeliveryPickup", "Completed", "Delivery Already Completed");
	if (Manager->GetTaskStatus(OwningTask) == EDeliveryTaskStatus::Locked)
		return NSLOCTEXT("DeliveryPickup", "Locked", "Wait for the Delivery Call");
	if (!Manager->IsTaskAccepted(OwningTask))
	{
		return NSLOCTEXT("DeliveryPickup", "NotAnswered", "Answer the Call to Accept This Delivery");
	}

	return NSLOCTEXT("DeliveryPickup", "OtherTask", "Complete Your Current Delivery First");
}

// 由服务器通知任务管理器快递已被领取
bool UDeliveryItemComponent::NotifyAcquired(APlayerState* Player)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return false;
	}

	UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);

	return Manager && Manager->TryAcquireItem(OwningTask, Player);
}

// 快递意外销毁时上报丢件，关卡结束时不改变任务
void UDeliveryItemComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 只管"这个 Actor 被销毁了"；关卡切换、退出游戏、PIE 结束时整张图都在拆，
	// 那时候改任务状态没有意义，而且 GameState 可能已经先一步没了
	const bool bActorDestroyed = EndPlayReason == EEndPlayReason::Destroyed;

	if (bReportLostOnDestroy && bActorDestroyed && GetOwner() && GetOwner()->HasAuthority())
	{
		// 交付成功那条路上，TryDeliver 是先把任务标成已完成、再销毁快递的，
		// 所以走到这里时状态已经不是进行中，NotifyItemLost 会自己返回 假
		// 这个先后顺序就是"正常交付"和"意外丢失"的判据，不需要额外加标志位
		if (UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this))
		{
			Manager->NotifyItemLost(OwningTask);
		}
	}

	Super::EndPlay(EndPlayReason);
}
