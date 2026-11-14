#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Vehicle/DeliveryCompanionMotorbike.h"
#include "Components/SceneComponent.h"

ADeliveryPackageItem* ADeliveryCompanionCharacter::GetCarriedTaskPackage() const
{
    return Companion->GetCarriedTaskPackage();
}
APlayerState* ADeliveryCompanionCharacter::GetTaskPlayerState() const
{
    return Companion->GetOwnerPlayerState();
}
bool ADeliveryCompanionCharacter::CanHandleTaskPackage() const
{
    const auto* Player = GetTaskPlayerState();
    return !IsKnockedDown() && !Companion->IsUsingErrandVehicle() && IsValid(Player) && Player->GetWorld() == GetWorld() && !Player->IsInactive();
}
bool ADeliveryCompanionCharacter::CanReceiveTaskPackage(ADeliveryPackageItem* Package) const
{
    return Companion->CanReceiveTaskPackage(Package);
}
bool ADeliveryCompanionCharacter::TryReceiveTaskPackage(ADeliveryPackageItem* Package)
{
    return Companion->TryReceiveTaskPackage(Package);
}
bool ADeliveryCompanionCharacter::ReleaseTaskPackage(ADeliveryPackageItem* Package)
{
    return Companion->ReleaseTaskPackage(Package);
}
void ADeliveryCompanionCharacter::ApplyTaskPackagePresentation(ADeliveryPackageItem* Package)
{
    ApplyHandheldItemPresentation(Package);
}
void ADeliveryCompanionCharacter::ApplyHandheldItemPresentation(ADeliveryHandheldItem* Package)
{
    if (!IsValid(Package) || Package->IsActorBeingDestroyed() || Package->GetOwner()!=this ||
        Package->GetWorld()!=GetWorld() || Companion->GetCarriedItem()!=Package) return;
    USceneComponent* Anchor=PackageAnchor;
    // 两种引用的复制到达顺序不固定，已附着的专属车也能提供当前挂点
    const auto* Bike=Cast<ADeliveryCompanionMotorbike>(GetAttachParentActor());
    if (!Bike && Companion) Bike=Companion->ErrandVehicle;
    if (IsValid(Bike) && Bike->GetWorld()==GetWorld() && Bike->IsMounted() && Bike->GetReservedCompanion()==this)
        Anchor=Bike->CompanionCargoAnchor;
    if (!IsValid(Anchor) || (Package->GetRootComponent()->GetAttachParent()==Anchor &&
        !Package->IsHidden() && !Package->GetActorEnableCollision())) return;
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakThis(this);
    const TWeakObjectPtr<ADeliveryHandheldItem> WeakPackage(Package);
    Package->SetAttachedCarryPresentation(Anchor);
    if (WeakThis.IsValid() && WeakPackage.IsValid() && !WeakThis->IsActorBeingDestroyed() &&
        WeakPackage->GetOwner()==WeakThis.Get() && HasAuthority()) WeakPackage->ForceNetUpdate();
}

ADeliveryPackageItem* UDeliveryCompanionComponent::GetCarriedTaskPackage() const
{
    return Cast<ADeliveryPackageItem>(GetCarriedItem());
}
bool UDeliveryCompanionComponent::CanReceiveTaskPackage(ADeliveryPackageItem* Package) const
{
    const auto* Npc = GetCompanion();
    if (!Npc || !Npc->CanHandleTaskPackage() || IsValid(CarriedPackage) || !IsValid(Package) ||
        Package->GetWorld() != GetWorld() || HomeState.CarriedProp || HasActiveHomeArrangement() || PendingHomeArrangementId.IsValid()) return false;
    const auto* Item = Package->GetDeliveryItemComponent();
    // 查询容量时物品仍可属于原持有人，真正接收必须等服务器交接释放
    return Item && Item->CanBeAcquiredBy(Npc);
}
bool UDeliveryCompanionComponent::TryReceiveTaskPackage(ADeliveryPackageItem* Package)
{
    if (!GetOwner()->HasAuthority() || !CanReceiveTaskPackage(Package) || Package->GetOwner()) return false;
    if (!StoreCarriedItem(Package)) return false;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryPackageItem> WeakPackage(Package);
    auto* Item = Package->GetDeliveryItemComponent();
    const auto* Manager = UDeliveryTaskManagerComponent::Get(this);
    // 换手时任务已经进行中，不重复接单、重置计时或把“未推进任务”当作接收失败
    if (!Manager || (Manager->GetTaskStatus(Item->OwningTask) != EDeliveryTaskStatus::InProgress &&
        !Item->NotifyAcquired(OwnerPlayerState)))
    {
        if (WeakThis.IsValid() && WeakPackage.IsValid()) ReleaseTaskPackage(Package);
        return false;
    }
    // 任务通知可能同步触发销毁或别的玩法，只报告当前仍实际持有的结果
    if (!WeakThis.IsValid() || !WeakPackage.IsValid() || !IsValid(GetOwner()) || GetOwner()->IsActorBeingDestroyed()) return false;
    GetOwner()->ForceNetUpdate();
    if (IsValid(Package)) Package->ForceNetUpdate();
    return GetCarriedTaskPackage() == Package;
}
bool UDeliveryCompanionComponent::ReleaseTaskPackage(ADeliveryPackageItem* Package)
{
    return IsValid(Package) && GetCarriedTaskPackage()==Package && ReleaseCarriedItem(Package);
}

void UDeliveryCompanionComponent::OnRep_CarriedPackage()
{
    if (IsValid(CarriedPackage)) CarriedPackage->RefreshCarrierPresentation();
}
void UDeliveryCompanionComponent::OnCarriedPackageDestroyed(AActor* DestroyedActor)
{
    if (!GetOwner()->HasAuthority() || CarriedPackage != DestroyedActor) return;
    CarriedPackage = nullptr;
    GetOwner()->ForceNetUpdate();
}
void UDeliveryCompanionComponent::DropCarriedItem()
{
    auto* Item=GetCarriedItem();
    if (!Item || !ReleaseCarriedItem(Item)) return;
    // 失去主人或同伴销毁留下同一实物，不改变任务或奖励
    Item->DropFromInventory(Item->GetActorLocation(),FVector::ZeroVector);
    if (IsValid(Item)) Item->ForceNetUpdate();
}

ADeliveryHandheldItem* UDeliveryCompanionComponent::GetCarriedItem() const
{
    return IsValid(CarriedPackage) && !CarriedPackage->IsActorBeingDestroyed() &&
        CarriedPackage->GetOwner()==GetOwner()?CarriedPackage.Get():nullptr;
}

bool UDeliveryCompanionComponent::CanReceiveCarriedItem(ADeliveryHandheldItem* Item) const
{
    const auto* Npc=GetCompanion();
    return Npc && Npc->CanHandleHandheldItem() && !IsValid(CarriedPackage) &&
        DeliveryHandheldTransfer::IsOrdinaryItem(Item) && Item->GetWorld()==GetWorld() &&
        Item->GetItemComponent()->CanTransferTo(Npc) && !HomeState.CarriedProp &&
        !HasActiveHomeArrangement() && !PendingHomeArrangementId.IsValid();
}

bool UDeliveryCompanionComponent::TryReceiveCarriedItem(ADeliveryHandheldItem* Item)
{
    return GetOwner() && GetOwner()->HasAuthority() && CanReceiveCarriedItem(Item) && StoreCarriedItem(Item);
}

bool UDeliveryCompanionComponent::StoreCarriedItem(ADeliveryHandheldItem* Item)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !IsValid(Item) || Item->IsActorBeingDestroyed() ||
        IsValid(CarriedPackage) || Item->GetOwner() || Item->GetWorld()!=GetWorld()) return false;
    CarriedPackage=Item;
    Item->OnDestroyed.AddUniqueDynamic(this,&ThisClass::OnCarriedPackageDestroyed);
    Item->SetOwner(GetOwner());
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryHandheldItem> WeakItem(Item);
    OnRep_CarriedPackage();
    if (!WeakThis.IsValid() || !WeakItem.IsValid() || !IsValid(GetOwner()) || GetOwner()->IsActorBeingDestroyed()) return false;
    GetOwner()->ForceNetUpdate();
    Item->ForceNetUpdate();
    return GetCarriedItem()==Item;
}

bool UDeliveryCompanionComponent::ReleaseCarriedItem(ADeliveryHandheldItem* Item)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !IsValid(Item) || GetCarriedItem()!=Item) return false;
    Item->OnDestroyed.RemoveDynamic(this,&ThisClass::OnCarriedPackageDestroyed);
    CarriedPackage=nullptr;
    Item->SetOwner(nullptr);
    // 不在释放接口改变表现，接收者、交付或掉落出口负责同一实物
    GetOwner()->ForceNetUpdate();
    Item->ForceNetUpdate();
    return true;
}

ADeliveryHandheldItem* ADeliveryCompanionCharacter::GetCarriedHandheldItem() const { return Companion->GetCarriedItem(); }
APlayerState* ADeliveryCompanionCharacter::GetHandheldOwnerPlayerState() const { return Companion->GetOwnerPlayerState(); }
bool ADeliveryCompanionCharacter::CanHandleHandheldItem() const
{
    const auto* Player=GetHandheldOwnerPlayerState();
    return CanHandleTaskPackage() && IsValid(Player->GetPlayerController()) &&
        Companion->GetMode()!=EDeliveryCompanionMode::Unrecruited && (!Combat || !Combat->IsReactingToHit());
}
bool ADeliveryCompanionCharacter::OwnsHandheldItem(const ADeliveryHandheldItem* Item) const
{
    return IsValid(Item) && Companion->GetCarriedItem()==Item && Item->GetOwner()==this;
}
bool ADeliveryCompanionCharacter::CanReceiveHandheldItem(ADeliveryHandheldItem* Item) const
{
    return Companion->CanReceiveCarriedItem(Item);
}
bool ADeliveryCompanionCharacter::TryReceiveHandheldItem(ADeliveryHandheldItem* Item)
{
    return Companion->TryReceiveCarriedItem(Item);
}
bool ADeliveryCompanionCharacter::ReleaseHandheldItem(ADeliveryHandheldItem* Item)
{
    return DeliveryHandheldTransfer::IsOrdinaryItem(Item) && Companion->ReleaseCarriedItem(Item);
}
