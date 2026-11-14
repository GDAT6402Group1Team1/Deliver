// 管理服务器权威背包；槽位 0 为手持物，槽位 1 至 4 为背包
// Copyright Epic Games, Inc. All Rights Reserved.

#include "Inventory/DeliveryInventoryComponent.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "Engine/World.h"
#include "AbilitySystemComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Character/DeliveryCharacter.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Net/UnrealNetwork.h"
#include "Task/World/DeliveryItemComponent.h"

UDeliveryInventoryComponent::UDeliveryInventoryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
	Slots.SetNum(SlotCount);
}

// 初始化与物品栏同步

// 初始化槽位，并根据当前物品恢复显示
void UDeliveryInventoryComponent::BeginPlay()
{
	Super::BeginPlay();
	Slots.SetNum(SlotCount);
	ApplyPresentation();
}

// 角色销毁时只释放仍由本库存实际持有的普通道具，任务包裹沿原任务规则处理
void UDeliveryInventoryComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ADeliveryCharacter* Character=GetCharacter();
    const TWeakObjectPtr<ADeliveryCharacter> PreviousOwner(Character);
    const TWeakObjectPtr<UWorld> PreviousWorld(GetWorld());
    TArray<TWeakObjectPtr<ADeliveryHandheldItem>> ItemsToDrop;
    FVector DropOrigin=FVector::ZeroVector;
    if (EndPlayReason==EEndPlayReason::Destroyed && IsValid(Character) &&
        Character->HasAuthority() && Character->IsActorBeingDestroyed())
    {
        // 隐藏背包道具可能仍在旧拾取点，沿现有丢物偏移取当前角色胶囊位置
        const FVector Forward=Character->GetActorForwardVector().GetSafeNormal2D();
        DropOrigin=Character->GetActorLocation()+Forward*DropForwardDistance+FVector::UpVector*DropUpOffset;
        for (auto& Slot:Slots)
        {
            auto* Item=Slot.Get();
            if (!DeliveryHandheldTransfer::IsOrdinaryItem(Item) || Item->GetOwner()!=Character ||
                Item->GetWorld()!=GetWorld()) continue;
            ItemsToDrop.AddUnique(TWeakObjectPtr<ADeliveryHandheldItem>(Item));
            Slot=nullptr;
        }
    }
    // 退出时先解除旧 UI 监听，清槽和掉落都不广播库存变化
    OnInventoryChanged.Clear();
    Super::EndPlay(EndPlayReason);
    // Super 和前一件道具的碰撞回调可能销毁或转移后面的物品，逐件重查真实归属
    for (const auto& WeakItem:ItemsToDrop)
    {
        auto* Item=WeakItem.Get();
        if (!PreviousOwner.IsValid() || !PreviousWorld.IsValid() ||
            !DeliveryHandheldTransfer::IsOrdinaryItem(Item) || Item->GetWorld()!=PreviousWorld.Get() ||
            Item->GetOwner()!=PreviousOwner.Get()) continue;
        Item->DropFromInventory(DropOrigin,FVector::ZeroVector);
        if (WeakItem.IsValid() && !WeakItem->IsActorBeingDestroyed()) WeakItem->ForceNetUpdate();
    }
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

// 物品栏查询

ADeliveryHandheldItem* UDeliveryInventoryComponent::GetItemInSlot(int32 SlotIndex) const
{
	ADeliveryHandheldItem* Item = Slots.IsValidIndex(SlotIndex) ? Slots[SlotIndex].Get() : nullptr;
	// 销毁通知和槽位复制可能先后到达，已销毁的物品不能继续占手或阻止接件
	return IsValid(Item) ? Item : nullptr;
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

// 拾取与快捷栏操作

// 在服务器检查物品归属和任务条件，分配槽位后同步表现与背包通知
bool UDeliveryInventoryComponent::TryPickup(ADeliveryHandheldItem* Item)
{
	ADeliveryCharacter* Character = GetCharacter();
	if (!Character || !Character->HasAuthority() || !IsValid(Item) || !CanMutateInventory())
		return false;
	UDeliveryInventoryItemComponent* ItemData = Item->GetItemComponent();
    const auto* Grabbable=Item->FindComponentByClass<UDeliveryGrabbableComponent>();
	if (!ItemData || Item->GetOwner() || Item->GetWorld()!=GetWorld() ||
        !ItemData->CanTransferTo(Character) || (Grabbable && Grabbable->HasAnyCarrier()))
		return false;
	UDeliveryItemComponent* DeliveryTaskItem = Item->FindComponentByClass<UDeliveryItemComponent>();
	if (ItemData->ItemType == EDeliveryInventoryItemType::DeliveryItem && DeliveryTaskItem &&
	    !DeliveryTaskItem->CanBeAcquiredBy(Character))
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

    const TWeakObjectPtr<UDeliveryInventoryComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryHandheldItem> WeakItem(Item);
    const TWeakObjectPtr<ADeliveryCharacter> WeakCharacter(Character);
	ApplyPresentation();
    if (!WeakThis.IsValid() || !WeakItem.IsValid() || !WeakCharacter.IsValid() || Character->IsActorBeingDestroyed() ||
        GetOwner()!=Character || !ContainsItem(Item) || (DeliveryTaskItem && !IsValid(DeliveryTaskItem))) return false;
	if (DeliveryTaskItem)
	{
		DeliveryTaskItem->NotifyAcquired(Character->GetPlayerState());
	}
    // 任务状态通知可同步销毁库存或改变持有者，先复核再发库存事件
    if (!WeakThis.IsValid() || !WeakItem.IsValid() || !WeakCharacter.IsValid() ||
        Character->IsActorBeingDestroyed() || GetOwner()!=Character || !ContainsItem(Item)) return false;
	OnInventoryChanged.Broadcast();
    if (!WeakThis.IsValid() || !WeakItem.IsValid() || !IsValid(GetOwner()) || GetOwner()->IsActorBeingDestroyed()) return false;
	GetOwner()->ForceNetUpdate();
	return ContainsItem(Item);
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

// 物品存放与丢弃

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

// 本机反馈与物品显示

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
		if (ADeliveryHandheldItem* Item = GetItemInSlot(Index))
		{
			// 旧槽位可能晚于 Owner 复制到达，不能把已交出的包裹重新挂回手中
            if (Item->GetOwner() != Character) continue;
			Item->SetInventoryPresentation(Index == 0, Index != 0, Character->GetHeldItemAnchor());
		}
	}
}

bool UDeliveryInventoryComponent::CanReceiveFetchedItem(ADeliveryHandheldItem* Item) const
{
    const auto* Character=GetCharacter();
    return Character && !Character->IsRidingVehicle() && !Character->IsIncapacitated() && CanMutateInventory() &&
        DeliveryHandheldTransfer::IsOrdinaryItem(Item) && Item->GetWorld()==GetWorld() &&
        Item->GetItemComponent()->CanTransferTo(Character) && FindEmptyBackpackSlot()!=INDEX_NONE;
}

bool UDeliveryInventoryComponent::ContainsItem(const ADeliveryHandheldItem* Item) const
{
    return IsValid(Item) && Item->GetOwner()==GetOwner() && Slots.ContainsByPredicate(
        [Item](const TObjectPtr<ADeliveryHandheldItem>& Entry) { return Entry.Get()==Item; });
}

bool UDeliveryInventoryComponent::ReleaseFetchedItem(ADeliveryHandheldItem* Item)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !DeliveryHandheldTransfer::IsOrdinaryItem(Item) || !ContainsItem(Item)) return false;
    for (auto& Slot:Slots) if (Slot==Item) Slot=nullptr;
    Item->SetOwner(nullptr);
    // 接收方或回滚负责接下来的表现，释放时不能先把物品抛到地面
    const TWeakObjectPtr<UDeliveryInventoryComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryHandheldItem> WeakItem(Item);
    OnInventoryChanged.Broadcast();
    if (!WeakThis.IsValid() || !WeakItem.IsValid() || !IsValid(GetOwner()) || GetOwner()->IsActorBeingDestroyed()) return false;
    GetOwner()->ForceNetUpdate();
    return !ContainsItem(Item) && Item->GetOwner()!=GetOwner();
}

void UDeliveryInventoryComponent::RefreshItemPresentation(ADeliveryHandheldItem* Item)
{
    if (ContainsItem(Item)) ApplyPresentation();
}
