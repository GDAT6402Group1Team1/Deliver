// 管理服务器权威背包；槽位 0 为手持物，槽位 1 至 4 为背包
// Copyright Epic Games, Inc. All Rights Reserved.

#include "Inventory/DeliveryInventoryComponent.h"
#include "CollisionQueryParams.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "AbilitySystemComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Character/DeliveryCharacter.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Net/UnrealNetwork.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/World/DeliveryItemComponent.h"

namespace
{
bool FindPackageDropLocation(const ADeliveryCharacter& Character, const ADeliveryHandheldItem& Package,
                             const FVector& Reference, const FVector& Forward, float ForwardDistance,
                             FVector& OutLocation, FVector& InOutVelocity)
{
	const UWorld* World = Character.GetWorld();
	const UStaticMeshComponent* Mesh = Package.GetItemMesh();
	const UStaticMesh* StaticMesh = Mesh ? Mesh->GetStaticMesh() : nullptr;
	if (!World || !StaticMesh)
		return false;

	const FBoxSphereBounds Bounds = StaticMesh->GetBounds();
	const FVector Scale = Mesh->GetComponentScale();
	const FQuat Rotation = Mesh->GetComponentQuat();
	const FVector CenterOffset = Rotation.RotateVector(Bounds.Origin * Scale);
	const FVector BoxExtent = Bounds.BoxExtent * Scale.GetAbs();
	const FCollisionShape Shape = FCollisionShape::MakeBox(BoxExtent);
	const FName Profile(TEXT("PhysicsActor"));
	FCollisionQueryParams Query(SCENE_QUERY_STAT(InventoryPackageDrop), false, &Character);
	Query.AddIgnoredActor(&Package);

	// 以玩家当前一侧为起点，用恢复物理后的碰撞规则检查整条放置路径
	const FVector Start = Reference + CenterOffset;
	if (World->OverlapBlockingTestByProfile(Start, Rotation, Profile, Shape, Query))
		return false;

	const auto IsClearDropPath = [&](const FVector& Candidate)
	{
		const FVector End = Candidate + CenterOffset;
		return !World->SweepTestByProfile(Start, End, Rotation, Profile, Shape, Query) &&
		       !World->OverlapBlockingTestByProfile(End, Rotation, Profile, Shape, Query);
	};

	const FVector Preferred = Reference + Forward * ForwardDistance;
	if (IsClearDropPath(Preferred))
	{
		OutLocation = Preferred;
		return true;
	}

	// 备选点也要给包裹和玩家各自的碰撞体留出空间
	const FVector Side = FVector::CrossProduct(FVector::UpVector, Forward);
	const double CharacterRadius = Character.GetCapsuleComponent()->GetScaledCapsuleRadius();
	for (const FVector& Direction : { Side, -Side, -Forward })
	{
		const double BoxRadius = FVector::DotProduct(Rotation.UnrotateVector(Direction).GetAbs(), BoxExtent);
		const double CenterDistance = CharacterRadius + BoxRadius + 3.0;
		const FVector Candidate = Reference + Direction * CenterDistance - FVector(CenterOffset.X, CenterOffset.Y, 0.0);
		if (IsClearDropPath(Candidate))
		{
			OutLocation = Candidate;
			InOutVelocity = FVector::ZeroVector;
			return true;
		}
	}
	return false;
}
}

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

// 角色被销毁时放下仍由该角色持有的实物；换控制对象和清理世界不改物品
void UDeliveryInventoryComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ADeliveryCharacter* Character=GetCharacter();
    const TWeakObjectPtr<ADeliveryCharacter> PreviousOwner(Character);
    const TWeakObjectPtr<UWorld> PreviousWorld(GetWorld());
    TArray<TWeakObjectPtr<ADeliveryHandheldItem>> ItemsToDrop;
    TWeakObjectPtr<ADeliveryPackageItem> TaskPackageToDrop;
    FVector Reference=FVector::ZeroVector;
    FVector Forward=FVector::ZeroVector;
    FVector DropOrigin=FVector::ZeroVector;
    if (EndPlayReason==EEndPlayReason::Destroyed && IsValid(Character) &&
        Character->HasAuthority() && Character->IsActorBeingDestroyed())
    {
        // 隐藏背包道具可能仍在旧拾取点，沿现有丢物偏移取当前角色胶囊位置
        Forward=Character->GetActorForwardVector().GetSafeNormal2D();
        Reference=Character->GetActorLocation()+FVector::UpVector*DropUpOffset;
        DropOrigin=Reference+Forward*DropForwardDistance;
        if (auto* Package=GetStoredTaskPackage(); IsValid(Package) &&
            PreviousWorld.IsValid() && !PreviousWorld->bIsTearingDown &&
            Package->GetOwner()==Character && Package->GetWorld()==PreviousWorld.Get())
        {
            const auto* Item=Package->GetDeliveryItemComponent();
            if (IsValid(Item) && IsValid(Item->OwningTask.Get()))
            {
                TaskPackageToDrop=Package;
                for (auto& Slot : Slots)
                    if (Slot == Package) Slot = nullptr;
            }
        }
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
    if (auto* Package=TaskPackageToDrop.Get(); IsValid(Package) && !Package->IsActorBeingDestroyed() &&
        PreviousOwner.IsValid() && PreviousWorld.IsValid() && !PreviousWorld->bIsTearingDown &&
        Package->GetWorld()==PreviousWorld.Get() &&
        Package->GetOwner()==PreviousOwner.Get())
    {
        FVector PackageLocation;
        FVector Velocity=FVector::ZeroVector;
        if (FindPackageDropLocation(*PreviousOwner.Get(),*Package,Reference,Forward,DropForwardDistance,PackageLocation,Velocity))
        {
            Package->DropFromInventory(PackageLocation,Velocity);
            if (TaskPackageToDrop.IsValid() && !TaskPackageToDrop->IsActorBeingDestroyed()) Package->ForceNetUpdate();
        }
        else
        {
            // 丢件由包裹自己的退出规则确认，已签收或临时生成的包裹不会改变订单
            Package->Destroy();
        }
    }
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
	DOREPLIFETIME_CONDITION(UDeliveryInventoryComponent, DroppedFirstOrderPackage, COND_OwnerOnly);
}

ADeliveryCharacter* UDeliveryInventoryComponent::GetCharacter() const
{
	return Cast<ADeliveryCharacter>(GetOwner());
}

const ADeliveryPackageItem* UDeliveryInventoryComponent::GetDroppedFirstOrderPackage() const
{
	const auto* Package = DroppedFirstOrderPackage.Get();
	if (!IsValid(Package) || Package->IsActorBeingDestroyed() || Package->GetWorld() != GetWorld() ||
	    Package->GetOwner() || Package->GetAttachParentActor() || Package->IsHidden()) return nullptr;
	const auto* Grabbable = Package->FindComponentByClass<UDeliveryGrabbableComponent>();
	return Grabbable && Grabbable->HasAnyCarrier() ? nullptr : Package;
}

void UDeliveryInventoryComponent::ForgetDroppedFirstOrderPackage(const ADeliveryPackageItem* Package)
{
	if (DroppedFirstOrderPackage != Package) return;
	DroppedFirstOrderPackage = nullptr;
	if (auto* Owner = GetOwner(); IsValid(Owner) && !Owner->IsActorBeingDestroyed()) Owner->ForceNetUpdate();
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

ADeliveryPackageItem* UDeliveryInventoryComponent::GetStoredTaskPackage() const
{
	for (const auto& Slot : Slots)
	{
		auto* Package = Cast<ADeliveryPackageItem>(Slot.Get());
		if (IsValid(Package) && !Package->IsActorBeingDestroyed() && Package->GetOwner() == GetOwner())
			return Package;
	}
	return nullptr;
}

bool UDeliveryInventoryComponent::IsTaskPackageInBackpack(const ADeliveryPackageItem* Package) const
{
	if (!IsValid(Package) || Package->GetOwner() != GetOwner()) return false;
	for (int32 Index = FirstBackpackSlot; Index < SlotCount; ++Index)
		if (GetItemInSlot(Index) == Package) return true;
	return false;
}

bool UDeliveryInventoryComponent::CanStoreTaskPackageInBackpack() const
{
	return CanMutateInventory() && HasBackpackSpace() && !GetStoredTaskPackage();
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
	return TryPickupInSlot(Item, false);
}

bool UDeliveryInventoryComponent::TryStoreTaskPackageInBackpack(ADeliveryPackageItem* Package)
{
	const auto* Character = GetCharacter();
	return Character && !Character->IsRidingVehicle() && TryPickupInSlot(Package, true);
}

bool UDeliveryInventoryComponent::TryPickupInSlot(ADeliveryHandheldItem* Item, bool bStorePackageInBackpack)
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
	const auto* TaskPackage = Cast<ADeliveryPackageItem>(Item);
	if ((TaskPackage && GetStoredTaskPackage()) || (bStorePackageInBackpack && !TaskPackage))
		return false;
	if (ItemData->ItemType == EDeliveryInventoryItemType::DeliveryItem && DeliveryTaskItem &&
	    !DeliveryTaskItem->CanBeAcquiredBy(Character))
	{
		return false;
	}

	// 默认领取快递到手持，背包交接指定一个真实空槽
	const int32 EmptyBackpack = FindEmptyBackpackSlot();
	const bool bBackpackItem = ItemData->CanEnterBackpack();
	if ((bBackpackItem || bStorePackageInBackpack) && EmptyBackpack == INDEX_NONE)
	{
		ClientInventoryFull();
		return false;
	}

	// 领取快递前先处理旧手持物：可入背包则收纳，否则丢弃
	if (bStorePackageInBackpack)
	{
		StoreItem(EmptyBackpack, Item);
	}
	else if (ItemData->ItemType == EDeliveryInventoryItemType::DeliveryItem)
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
				const TWeakObjectPtr<UDeliveryInventoryComponent> WeakThis(this);
				const TWeakObjectPtr<ADeliveryCharacter> WeakCharacter(Character);
				const TWeakObjectPtr<ADeliveryHandheldItem> WeakItem(Item);
				if (!TryDropHeldItem(true) || !WeakThis.IsValid() || !WeakCharacter.IsValid() ||
				    !WeakItem.IsValid() || !IsValid(ItemData) || Character->IsActorBeingDestroyed() ||
				    Item->GetOwner() || GetHeldItem() || !ItemData->CanTransferTo(Character) ||
				    (DeliveryTaskItem && (!IsValid(DeliveryTaskItem) || !DeliveryTaskItem->CanBeAcquiredBy(Character))))
					return false;
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

// 槽位 0 执行丢弃，其余槽位与手持物交换
void UDeliveryInventoryComponent::PerformSlotAction(int32 SlotIndex)
{
	const auto* Character = GetCharacter();
	if (!Character || SlotIndex < 0 || SlotIndex >= SlotCount)
		return;
	if (SlotIndex == 0)
	{
		TryDropHeldItem(true);
		return;
	}

	ADeliveryHandheldItem* Hand = GetHeldItem();
	ADeliveryHandheldItem* Target = GetItemInSlot(SlotIndex);
	// 上车前发出的旧槽位请求也不能在驾驶中拿出任务包裹
	if (Character->IsRidingVehicle() && (Cast<ADeliveryPackageItem>(Hand) || Cast<ADeliveryPackageItem>(Target)))
		return;
	Slots[0] = Target;
	Slots[SlotIndex] = Hand;
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

// 晕倒时仍沿原规则强制丢出手持物
void UDeliveryInventoryComponent::DropHeldItem()
{
	TryDropHeldItem(false);
}

bool UDeliveryInventoryComponent::TryDropHeldItem(bool bCheckPackagePlacement)
{
	ADeliveryCharacter* Character = GetCharacter();
	ADeliveryHandheldItem* Item = GetHeldItem();
	if (!Character || !Character->HasAuthority() || !Item)
		return false;
	const FVector Forward = Character->GetActorForwardVector().GetSafeNormal2D();
	const FVector Reference = Character->GetActorLocation() + FVector::UpVector * DropUpOffset;
	FVector Origin = Reference + Forward * DropForwardDistance;
	FVector Velocity = Forward * DropForwardSpeed;
	const UDeliveryInventoryItemComponent* ItemData = Item->GetItemComponent();
	if (bCheckPackagePlacement && ItemData && ItemData->ItemType == EDeliveryInventoryItemType::DeliveryItem &&
	    !FindPackageDropLocation(*Character, *Item, Reference, Forward, DropForwardDistance, Origin, Velocity))
		return false;

	ADeliveryPackageItem* Package = Cast<ADeliveryPackageItem>(Item);
	const auto* TaskItem = Package ? Package->GetDeliveryItemComponent() : nullptr;
	const TWeakObjectPtr<ADeliveryPackageItem> FirstOrderPackage =
	    bCheckPackagePlacement && Item->GetOwner() == Character && TaskItem && TaskItem->OwningTask &&
	    TaskItem->OwningTask->TaskId == TEXT("Task_001") ? Package : nullptr;
	if (FirstOrderPackage.IsValid()) FirstOrderPackage->RecoveryInventory = this;

	// 先清手持槽，再恢复包裹碰撞
	Slots[0] = nullptr;
	const TWeakObjectPtr<UDeliveryInventoryComponent> WeakThis(this);
	const TWeakObjectPtr<ADeliveryCharacter> WeakCharacter(Character);
	Item->DropFromInventory(Origin, Velocity);
	if (!WeakThis.IsValid() || !WeakCharacter.IsValid() || Character->IsActorBeingDestroyed())
		return true;
	// 放置触发的同步回调可能已销毁或转移实物，只记录仍在地面的原件
	if (auto* Dropped = FirstOrderPackage.Get(); IsValid(Dropped) && Dropped->RecoveryInventory.Get() == this)
	{
		if (!Dropped->IsActorBeingDestroyed() && !Dropped->GetOwner() && !Dropped->GetAttachParentActor() &&
		    Dropped->GetActorEnableCollision() && !Dropped->IsHidden())
			DroppedFirstOrderPackage = Dropped;
		else
			Dropped->ClearRecoveryInventory();
	}
	ApplyPresentation();
	if (WeakThis.IsValid() && WeakCharacter.IsValid() && !Character->IsActorBeingDestroyed())
		OnInventoryChanged.Broadcast();
	if (WeakThis.IsValid() && WeakCharacter.IsValid() && !Character->IsActorBeingDestroyed())
		Character->ForceNetUpdate();
	return true;
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
	return ConsumeItemInSlot(0, ExpectedItem);
}

bool UDeliveryInventoryComponent::ConsumeTaskPackage(ADeliveryPackageItem* Package)
{
	if (!IsValid(Package) || Package->GetOwner() != GetOwner()) return false;
	for (int32 Index = 0; Index < SlotCount; ++Index)
		if (GetItemInSlot(Index) == Package) return ConsumeItemInSlot(Index, Package);
	return false;
}

bool UDeliveryInventoryComponent::ConsumeItemInSlot(int32 SlotIndex, ADeliveryHandheldItem* ExpectedItem)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !ExpectedItem || GetItemInSlot(SlotIndex) != ExpectedItem)
	{
		return false;
	}
	Slots[SlotIndex] = nullptr;
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
