// 管理服务器权威背包；槽位 0 为手持物，槽位 1 至 4 为背包
// Copyright Epic Games, Inc. All Rights Reserved.

#include "Inventory/DeliveryInventoryComponent.h"
#include "AbilitySystemComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Character/DeliveryCharacter.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Net/UnrealNetwork.h"
#include "Task/World/DeliveryItemComponent.h"

UDeliveryInventoryComponent::UDeliveryInventoryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
	Slots.SetNum(SlotCount);
}

// 初始化槽位，并根据当前物品恢复显示
void UDeliveryInventoryComponent::BeginPlay()
{
	Super::BeginPlay();
	Slots.SetNum(SlotCount);
	ApplyPresentation();
}

// 向客户端同步物品槽位
void UDeliveryInventoryComponent::GetLifetimeReplicatedProps(
    TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UDeliveryInventoryComponent, Slots);
}

ADeliveryCharacter* UDeliveryInventoryComponent::GetCharacter() const
{
	return Cast<ADeliveryCharacter>(GetOwner());
}

ADeliveryHandheldItem* UDeliveryInventoryComponent::GetItemInSlot(int32 SlotIndex) const
{
	return Slots.IsValidIndex(SlotIndex) ? Slots[SlotIndex].Get() : nullptr;
}

// 从背包槽位中寻找空位，不包含手持槽
int32 UDeliveryInventoryComponent::FindEmptyBackpackSlot() const
{
	for (int32 Index = FirstBackpackSlot; Index < SlotCount; ++Index)
	{
		if (!GetItemInSlot(Index))
			return Index;
	}
	return INDEX_NONE;
}

bool UDeliveryInventoryComponent::HasBackpackSpace() const
{
	return FindEmptyBackpackSlot() != INDEX_NONE;
}

// 计算满包提示的闪烁强度，提示结束后返回零
float UDeliveryInventoryComponent::GetFullFeedbackAlpha() const
{
	const UWorld* World = GetWorld();
	if (!World)
		return 0.0f;
	const float Age = World->GetTimeSeconds() - FullFeedbackStartedAt;
	return Age >= 0.0f && Age < 0.7f
	           ? (1.0f - Age / 0.7f) * (0.6f + 0.4f * FMath::Abs(FMath::Sin(Age * 18.0f)))
			   : 0.0f;
}

// 检查抓取和眩晕状态是否允许修改背包
bool UDeliveryInventoryComponent::CanMutateInventory() const
{
	const ADeliveryCharacter* Character = GetCharacter();
	if (!Character)
		return false;
	if (const UDeliveryGrabComponent* Grab = Character->GetGrabComponent(); Grab && Grab->IsGrabbing())
		return false;
	const UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent();
	return !ASC || !ASC->HasMatchingGameplayTag(TAG_State_Stunned);
}

// 在服务器检查物品归属和任务条件，分配槽位后同步表现与背包通知
bool UDeliveryInventoryComponent::TryPickup(ADeliveryHandheldItem* Item)
{
	ADeliveryCharacter* Character = GetCharacter();
	if (!Character || !Character->HasAuthority() || !IsValid(Item) || !CanMutateInventory())
		return false;
	UDeliveryInventoryItemComponent* ItemData = Item->GetItemComponent();
	if (!ItemData || Item->GetOwner())
		return false;
	UDeliveryItemComponent* DeliveryTaskItem = Item->FindComponentByClass<UDeliveryItemComponent>();
	if (ItemData->ItemType == EDeliveryInventoryItemType::DeliveryItem && DeliveryTaskItem &&
	    !DeliveryTaskItem->CanBeAcquired())
	{
		return false;
	}

	// 手持槽与背包槽分开处理；快递物品只能占用手持槽
	const int32 EmptyBackpack = FindEmptyBackpackSlot();
	const bool bBackpackItem = ItemData->CanEnterBackpack();
	if (bBackpackItem && EmptyBackpack == INDEX_NONE)
	{
		ClientInventoryFull();
		return false;
	}

	// 领取快递前先处理旧手持物：可入背包则收纳，否则丢弃
	if (ItemData->ItemType == EDeliveryInventoryItemType::DeliveryItem)
	{
		if (ADeliveryHandheldItem* OldHand = GetHeldItem())
		{
			if (OldHand->GetItemComponent() && OldHand->GetItemComponent()->CanEnterBackpack() &&
			    EmptyBackpack != INDEX_NONE)
			{
				StoreItem(EmptyBackpack, OldHand);
			}
			else
			{
				DropHeldItem();
			}
		}
		StoreItem(0, Item);
	}
	else if (!GetHeldItem())
	{
		StoreItem(0, Item);
	}
	else
	{
		StoreItem(EmptyBackpack, Item);
	}

	ApplyPresentation();
	if (DeliveryTaskItem)
	{
		DeliveryTaskItem->NotifyAcquired(Character->GetPlayerState());
	}
	OnInventoryChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
	return true;
}

// 校验槽位后在服务器执行切换或丢弃
void UDeliveryInventoryComponent::RequestSlotAction(int32 SlotIndex)
{
	if (!CanMutateInventory() || SlotIndex < 0 || SlotIndex >= SlotCount)
		return;
	if (GetOwner() && GetOwner()->HasAuthority())
		PerformSlotAction(SlotIndex);
	else
		ServerSlotAction(SlotIndex);
}

void UDeliveryInventoryComponent::ServerSlotAction_Implementation(int32 SlotIndex)
{
	if (CanMutateInventory())
		PerformSlotAction(SlotIndex);
}

// 槽位 0 执行丢弃，其余槽位与手持物交换；快递不允许换入背包
void UDeliveryInventoryComponent::PerformSlotAction(int32 SlotIndex)
{
	if (SlotIndex < 0 || SlotIndex >= SlotCount)
		return;
	if (SlotIndex == 0)
	{
		DropHeldItem();
		return;
	}

	ADeliveryHandheldItem* Hand = GetHeldItem();
	ADeliveryHandheldItem* Target = GetItemInSlot(SlotIndex);
	if (Hand && Hand->GetItemComponent() &&
	    Hand->GetItemComponent()->ItemType == EDeliveryInventoryItemType::DeliveryItem)
	{
		DropHeldItem();
		Slots[0] = Target;
		Slots[SlotIndex] = nullptr;
	}
	else
	{
		Slots[0] = Target;
		Slots[SlotIndex] = Hand;
	}
	ApplyPresentation();
	OnInventoryChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
}

// 记录槽位中的物品，并将物品归属设为角色
void UDeliveryInventoryComponent::StoreItem(int32 SlotIndex, ADeliveryHandheldItem* Item)
{
	if (!Slots.IsValidIndex(SlotIndex) || !Item)
		return;
	Slots[SlotIndex] = Item;
	Item->SetOwner(GetOwner());
}

// 从手持槽移除物品，再根据角色朝向丢出
void UDeliveryInventoryComponent::DropHeldItem()
{
	ADeliveryCharacter* Character = GetCharacter();
	ADeliveryHandheldItem* Item = GetHeldItem();
	if (!Character || !Character->HasAuthority() || !Item)
		return;
	Slots[0] = nullptr;
	const FVector Forward = Character->GetActorForwardVector().GetSafeNormal2D();
	const FVector Origin =
	    Character->GetActorLocation() + Forward * DropForwardDistance + FVector::UpVector * DropUpOffset;
	Item->DropFromInventory(Origin, Forward * DropForwardSpeed);
	ApplyPresentation();
	OnInventoryChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
}

// 移除指定物品的槽位引用，并更新显示和同步
void UDeliveryInventoryComponent::RemoveItem(ADeliveryHandheldItem* Item)
{
	if (!Item || !GetOwner() || !GetOwner()->HasAuthority())
		return;
	bool bRemoved = false;
	for (TObjectPtr<ADeliveryHandheldItem>& Slot : Slots)
	{
		if (Slot == Item)
		{
			Slot = nullptr;
			bRemoved = true;
		}
	}
	if (bRemoved)
	{
		ApplyPresentation();
		OnInventoryChanged.Broadcast();
		GetOwner()->ForceNetUpdate();
	}
}

// 仅移除指定手持物的背包关系，由交付流程负责销毁物体
bool UDeliveryInventoryComponent::ConsumeHeldItem(ADeliveryHandheldItem* ExpectedItem)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !ExpectedItem || GetHeldItem() != ExpectedItem)
	{
		return false;
	}
	Slots[0] = nullptr;
	ExpectedItem->SetOwner(nullptr);
	OnInventoryChanged.Broadcast();
	GetOwner()->ForceNetUpdate();
	return true;
}

void UDeliveryInventoryComponent::UseHeldItem()
{
	if (ADeliveryHandheldItem* Item = GetHeldItem())
		Item->UseFromInventory(GetCharacter());
}

void UDeliveryInventoryComponent::ClientInventoryFull_Implementation()
{
	FullFeedbackStartedAt = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	OnInventoryChanged.Broadcast();
}

// 客户端收到槽位变化后更新物体表现，再通知界面
void UDeliveryInventoryComponent::OnRep_Slots()
{
	Slots.SetNum(SlotCount);
	ApplyPresentation();
	OnInventoryChanged.Broadcast();
}

// 根据槽位重新设置每件物品的手持或收纳显示
void UDeliveryInventoryComponent::ApplyPresentation()
{
	ADeliveryCharacter* Character = GetCharacter();
	if (!Character)
		return;
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		if (ADeliveryHandheldItem* Item = Slots[Index])
		{
			Item->SetInventoryPresentation(Index == 0, Index != 0, Character->GetHeldItemAnchor());
		}
	}
}
