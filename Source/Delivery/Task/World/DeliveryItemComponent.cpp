// Copyright Epic Games, Inc. All Rights Reserved.

#include "Task/World/DeliveryItemComponent.h"
#include "Inventory/DeliveryInventoryItemComponent.h"

#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Net/UnrealNetwork.h"

UDeliveryItemComponent::UDeliveryItemComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
    SetIsReplicatedByDefault(true);
}

void UDeliveryItemComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UDeliveryItemComponent, OwningTask);
}

// 向任务管理器查询这件快递是否允许领取
bool UDeliveryItemComponent::CanBeAcquired() const
{
	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);

	return !IsTransferInProgress() && Manager && Manager->CanAcquireItem(OwningTask);
}

bool UDeliveryItemComponent::CanBeAcquiredBy(const AActor* Receiver) const
{
    const auto* Item=GetOwner()?GetOwner()->FindComponentByClass<UDeliveryInventoryItemComponent>():nullptr;
    if (!IsValid(Receiver) || Receiver->GetWorld()!=GetWorld() || (Item && !Item->CanTransferTo(Receiver))) return false;
    const auto* Manager = UDeliveryTaskManagerComponent::Get(this);
    return Manager && Manager->CanAcquireItem(OwningTask);
}

bool UDeliveryItemComponent::BeginTransfer(AActor* Receiver)
{
    auto* Item=GetOwner()?GetOwner()->FindComponentByClass<UDeliveryInventoryItemComponent>():nullptr;
    if (!Item || TaskTransferId.IsValid()) return false;
    TaskTransferId=Item->BeginTransfer(Receiver);
    return TaskTransferId.IsValid();
}

void UDeliveryItemComponent::EndTransfer()
{
    if (!GetOwner() || !GetOwner()->HasAuthority()) return;
    const FGuid OldId=TaskTransferId;
    TaskTransferId.Invalidate();
    if (auto* Item=GetOwner()->FindComponentByClass<UDeliveryInventoryItemComponent>()) Item->EndTransfer(OldId);
}

bool UDeliveryItemComponent::IsTransferInProgress() const
{
    const auto* Item=GetOwner()?GetOwner()->FindComponentByClass<UDeliveryInventoryItemComponent>():nullptr;
    return Item && Item->IsTransferInProgress();
}

// 给出任务快递暂时无法领取的原因
FText UDeliveryItemComponent::GetPickupBlockedReason() const
{
	if (IsTransferInProgress())
        return NSLOCTEXT("DeliveryPickup", "TransferInProgress", "正在交接包裹");
	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);
	if (!OwningTask)
		return NSLOCTEXT("DeliveryPickup", "NoTask", "快递未配置任务");
	if (!Manager)
		return NSLOCTEXT("DeliveryPickup", "NoManager", "当前关卡未配置任务管理器");
	if (!Manager->IsTaskRegistered(OwningTask))
		return NSLOCTEXT("DeliveryPickup", "NotRegistered", "当前关卡未登记这份快递任务");
	if (Manager->CanAcquireItem(OwningTask))
		return FText::GetEmpty();
	if (Manager->GetTaskStatus(OwningTask) == EDeliveryTaskStatus::Completed)
		return NSLOCTEXT("DeliveryPickup", "Completed", "这份快递的任务已完成");
	if (Manager->GetTaskStatus(OwningTask) == EDeliveryTaskStatus::Locked)
		return NSLOCTEXT("DeliveryPickup", "Locked", "任务尚未解锁，请等待任务来电");
	if (!Manager->IsTaskAccepted(OwningTask))
	{
		return NSLOCTEXT("DeliveryPickup", "NotAnswered", "先接电话才能接这一单");
	}

	return NSLOCTEXT("DeliveryPickup", "OtherTask", "请先完成正在进行的快递任务");
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
