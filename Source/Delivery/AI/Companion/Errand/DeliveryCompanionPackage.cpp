#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "Inventory/Items/DeliveryPackageItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Vehicle/Companion/DeliveryCompanionMotorbike.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"

// ── 快递包裹 ──
// 二狗接过、拿着、交出快递包裹；包裹归属仍走原来的携带接口

/// @brief 二狗手上的任务包裹，转给组件的 GetCarriedTaskPackage
ADeliveryPackageItem* ADeliveryCompanionCharacter::GetCarriedTaskPackage() const
{
    return Companion->GetCarriedTaskPackage();
}
/// @brief 二狗代表的玩家就是主人，任务和奖励按主人结算
APlayerState* ADeliveryCompanionCharacter::GetTaskPlayerState() const
{
    return Companion->GetOwnerPlayerState();
}
/// @brief 二狗站着、没在骑车、主人在线时可以拿任务包裹
bool ADeliveryCompanionCharacter::CanHandleTaskPackage() const
{
    const auto* Player = GetTaskPlayerState();
    return !IsKnockedDown() && !Companion->IsUsingErrandVehicle() && IsValid(Player) && Player->GetWorld() == GetWorld() && !Player->IsInactive();
}
/// @brief 转给组件的 CanReceiveTaskPackage
bool ADeliveryCompanionCharacter::CanReceiveTaskPackage(ADeliveryPackageItem* Package) const
{
    return Companion->CanReceiveTaskPackage(Package);
}
/// @brief 转给组件的 TryReceiveTaskPackage
bool ADeliveryCompanionCharacter::TryReceiveTaskPackage(ADeliveryPackageItem* Package)
{
    return Companion->TryReceiveTaskPackage(Package);
}
/// @brief 转给组件的 ReleaseTaskPackage
bool ADeliveryCompanionCharacter::ReleaseTaskPackage(ADeliveryPackageItem* Package)
{
    return Companion->ReleaseTaskPackage(Package);
}
/// @brief 包裹按普通手持物品的方式挂到二狗身上，转给 ApplyHandheldItemPresentation
void ADeliveryCompanionCharacter::ApplyTaskPackagePresentation(ADeliveryPackageItem* Package)
{
    ApplyHandheldItemPresentation(Package);
}
/// @brief 把二狗手上的物品挂到手上或车上：骑专属车时挂到车的货位，否则挂到手持挂点
void ADeliveryCompanionCharacter::ApplyHandheldItemPresentation(ADeliveryHandheldItem* Item)
{
    if (!IsValid(Companion) || !IsValid(Item) || Item->IsActorBeingDestroyed() || IsActorBeingDestroyed() ||
        Item->GetOwner()!=this || Item->GetWorld()!=GetWorld() || Companion->GetCarriedItem()!=Item ||
        !IsValid(Item->GetRootComponent())) return;
    const auto FindMountedBike=[](const ADeliveryCompanionCharacter* Npc)->ADeliveryCompanionMotorbike*
    {
        const auto Matches=[Npc](const ADeliveryCompanionMotorbike* Bike)
        {
            return IsValid(Bike) && !Bike->IsActorBeingDestroyed() && Bike->GetWorld()==Npc->GetWorld() &&
                Bike->IsMounted() && Bike->GetReservedCompanion()==Npc;
        };
        auto* Attached=Cast<ADeliveryCompanionMotorbike>(Npc->GetAttachParentActor());
        if (Matches(Attached)) return Attached;
        auto* Reserved=Npc->Companion->ErrandVehicle.Get();
        return Matches(Reserved)?Reserved:nullptr;
    };
    const TWeakObjectPtr<ADeliveryCompanionMotorbike> ExpectedBike(FindMountedBike(this));
    const FVector OriginalWorldScale=Item->GetActorScale3D();
    if (OriginalWorldScale.ContainsNaN()) return;
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakThis(this);
    const TWeakObjectPtr<ADeliveryHandheldItem> WeakItem(Item);
    const auto IsCurrent=[WeakThis,WeakItem,ExpectedBike,FindMountedBike]()
    {
        return WeakThis.IsValid() && WeakItem.IsValid() && !WeakThis->IsActorBeingDestroyed() &&
            !WeakItem->IsActorBeingDestroyed() && WeakItem->GetOwner()==WeakThis.Get() &&
            WeakItem->GetWorld()==WeakThis->GetWorld() && IsValid(WeakThis->Companion) &&
            WeakThis->Companion->GetCarriedItem()==WeakItem.Get() && IsValid(WeakItem->GetRootComponent()) &&
            TWeakObjectPtr<ADeliveryCompanionMotorbike>(FindMountedBike(WeakThis.Get())).HasSameIndexAndSerialNumber(ExpectedBike);
    };
    USceneComponent* Anchor=PackageAnchor;
    FVector Offset=FVector::ZeroVector;
    FRotator Rotation=FRotator::ZeroRotator;
    bool bGripUnavailable=false;
    // 两种引用的复制到达顺序不固定，已附着的专属车也能提供当前挂点
    if (ExpectedBike.IsValid())
    {
        Anchor=ExpectedBike->CompanionCargoAnchor;
    }
    else if (DeliveryHandheldTransfer::IsOrdinaryItem(Item))
    {
        auto* NpcMesh=GetMesh();
        const TWeakObjectPtr<USceneComponent> Grip(HandheldAnchor);
        if (IsValid(NpcMesh) && Grip.IsValid() && !Item->NpcHandSocket.IsNone() &&
            NpcMesh->DoesSocketExist(Item->NpcHandSocket) && !Item->NpcHeldOffset.ContainsNaN() &&
            !Item->NpcHeldRotation.ContainsNaN())
        {
            bool bAttached=true;
            if (Grip->GetAttachParent()!=NpcMesh || Grip->GetAttachSocketName()!=Item->NpcHandSocket)
                bAttached=Grip->AttachToComponent(NpcMesh,FAttachmentTransformRules::SnapToTargetNotIncludingScale,Item->NpcHandSocket);
            if (!IsCurrent() || !Grip.IsValid() || HandheldAnchor!=Grip.Get()) return;
            if (bAttached)
            {
                if (Grip->IsUsingAbsoluteLocation() || Grip->IsUsingAbsoluteRotation() || !Grip->IsUsingAbsoluteScale())
                    Grip->SetAbsolute(false,false,true);
                if (!IsCurrent() || !Grip.IsValid()) return;
                if (!Grip->GetRelativeTransform().Equals(FTransform::Identity))
                    Grip->SetRelativeTransform(FTransform::Identity);
                if (!IsCurrent() || !Grip.IsValid()) return;
                Anchor=Grip.Get();
                Offset=Item->NpcHeldOffset;
                Rotation=Item->NpcHeldRotation;
            }
            else bGripUnavailable=true;
        }
        else bGripUnavailable=true;
    }
    if (!IsCurrent() || !IsValid(Anchor)) return;
    // 锚点配置被修正时也保持附着中实物的当前尺寸，不保存或乘第二份缩放状态
    if (!Item->GetActorScale3D().Equals(OriginalWorldScale))
    {
        Item->SetActorScale3D(OriginalWorldScale);
        if (!IsCurrent() || !IsValid(Anchor)) return;
    }
    const auto* Root=Item->GetRootComponent();
    // 同一父节点不足以说明已贴手，切换道具配置后仍核对 socket、偏移与朝向
    if (Root->GetAttachParent()==Anchor && Root->GetAttachSocketName().IsNone() &&
        Root->GetRelativeLocation().Equals(Offset,.01f) && Root->GetRelativeRotation().Equals(Rotation,.01f) &&
        !Item->IsHidden() && !Item->GetActorEnableCollision()) return;
    // 缺配置只在需要重新附着时提示，保留原安全挂点，不在动画每帧输出
    if (bGripUnavailable)
        UE_LOG(LogTemp,Warning,TEXT("CompanionItemGripUnavailable npc=%s item=%s socket=%s; keeping package anchor"),
            *GetName(),*Item->GetName(),*Item->NpcHandSocket.ToString());
    Item->SetAttachedCarryPresentation(Anchor,Offset,Rotation);
    if (IsCurrent() && HasAuthority()) Item->ForceNetUpdate();
}

/// @brief 二狗手上的东西是任务包裹时返回它
ADeliveryPackageItem* UDeliveryCompanionComponent::GetCarriedTaskPackage() const
{
    return Cast<ADeliveryPackageItem>(GetCarriedItem());
}
/// @brief 二狗能拿包裹、手上空着、没有家居安排，且包裹允许被二狗取走
bool UDeliveryCompanionComponent::CanReceiveTaskPackage(ADeliveryPackageItem* Package) const
{
    const auto* Npc = GetCompanion();
    if (!Npc || !Npc->CanHandleTaskPackage() || IsValid(CarriedPackage) || !IsValid(Package) ||
        Package->GetWorld() != GetWorld() || HomeState.CarriedProp || HasActiveHomeArrangement() || PendingHomeArrangementId.IsValid()) return false;
    const auto* Item = Package->GetDeliveryItemComponent();
    // 查询容量时物品仍可属于原持有人，真正接收必须等服务器交接释放
    return Item && Item->CanBeAcquiredBy(Npc);
}
/// @brief 服务器把包裹放进二狗唯一的持物槽，再按任务状态登记取件，不重复接单
bool UDeliveryCompanionComponent::TryReceiveTaskPackage(ADeliveryPackageItem* Package)
{
    if (!GetOwner()->HasAuthority() || !CanReceiveTaskPackage(Package) || Package->GetOwner()) return false;
    if (!StoreCarriedItem(Package)) return false;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryPackageItem> WeakPackage(Package);
    auto* Item = Package->GetDeliveryItemComponent();
    const auto* Manager = UDeliveryTaskManagerComponent::Get(this);
    // 换手时任务已经在进行中：不再接单、不重置计时，也不把任务状态没有前进当成接收失败
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
/// @brief 二狗手上拿的就是 Package 时释放它
bool UDeliveryCompanionComponent::ReleaseTaskPackage(ADeliveryPackageItem* Package)
{
    return IsValid(Package) && GetCarriedTaskPackage()==Package && ReleaseCarriedItem(Package);
}

/// @brief 客户端收到持物变化时刷新包裹的挂载表现
void UDeliveryCompanionComponent::OnRep_CarriedPackage()
{
    if (IsValid(CarriedPackage)) CarriedPackage->RefreshCarrierPresentation();
}
/// @brief 手上的物品被销毁时清空持物槽并同步
void UDeliveryCompanionComponent::OnCarriedPackageDestroyed(AActor* DestroyedActor)
{
    if (!GetOwner()->HasAuthority() || CarriedPackage != DestroyedActor) return;
    CarriedPackage = nullptr;
    GetOwner()->ForceNetUpdate();
}
/// @brief 把手上的物品原地放到地上，任务和奖励不变
void UDeliveryCompanionComponent::DropCarriedItem()
{
    auto* Item=GetCarriedItem();
    if (!Item || !ReleaseCarriedItem(Item)) return;
    // 失去主人或同伴销毁留下同一实物，不改变任务或奖励
    Item->DropFromInventory(Item->GetActorLocation(),FVector::ZeroVector);
    if (IsValid(Item)) Item->ForceNetUpdate();
}

/// @brief 持物槽里仍属于二狗的那件物品，没有返回空
ADeliveryHandheldItem* UDeliveryCompanionComponent::GetCarriedItem() const
{
    return IsValid(CarriedPackage) && !CarriedPackage->IsActorBeingDestroyed() &&
        CarriedPackage->GetOwner()==GetOwner()?CarriedPackage.Get():nullptr;
}

/// @brief 二狗手上空着、没有家居安排，且这件普通物品允许转给二狗
bool UDeliveryCompanionComponent::CanReceiveCarriedItem(ADeliveryHandheldItem* Item) const
{
    const auto* Npc=GetCompanion();
    return Npc && Npc->CanHandleHandheldItem() && !IsValid(CarriedPackage) &&
        DeliveryHandheldTransfer::IsOrdinaryItem(Item) && Item->GetWorld()==GetWorld() &&
        Item->GetItemComponent()->CanTransferTo(Npc) && !HomeState.CarriedProp &&
        !HasActiveHomeArrangement() && !PendingHomeArrangementId.IsValid();
}

/// @brief 服务器检查 CanReceiveCarriedItem 后调用 StoreCarriedItem 放进持物槽
bool UDeliveryCompanionComponent::TryReceiveCarriedItem(ADeliveryHandheldItem* Item)
{
    return GetOwner() && GetOwner()->HasAuthority() && CanReceiveCarriedItem(Item) && StoreCarriedItem(Item);
}

/// @brief 把物品写进持物槽、设所有者为二狗、订阅销毁事件，并刷新挂载表现
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

/// @brief 清空持物槽和所有者，解绑销毁事件；挂载表现由接收方或掉落流程负责
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

/// @brief 二狗手上的普通物品
ADeliveryHandheldItem* ADeliveryCompanionCharacter::GetCarriedHandheldItem() const { return Companion->GetCarriedItem(); }
/// @brief 手持物品按主人结算
APlayerState* ADeliveryCompanionCharacter::GetHandheldOwnerPlayerState() const { return Companion->GetOwnerPlayerState(); }
/// @brief 已结识、主人在线、没在受击反应时可以拿普通物品
bool ADeliveryCompanionCharacter::CanHandleHandheldItem() const
{
    const auto* Player=GetHandheldOwnerPlayerState();
    return CanHandleTaskPackage() && IsValid(Player->GetPlayerController()) &&
        Companion->GetMode()!=EDeliveryCompanionMode::Unrecruited && (!Combat || !Combat->IsReactingToHit());
}
/// @brief Item 就在二狗的持物槽里时返回 true
bool ADeliveryCompanionCharacter::OwnsHandheldItem(const ADeliveryHandheldItem* Item) const
{
    return IsValid(Item) && Companion->GetCarriedItem()==Item && Item->GetOwner()==this;
}
/// @brief 转给组件的 CanReceiveCarriedItem
bool ADeliveryCompanionCharacter::CanReceiveHandheldItem(ADeliveryHandheldItem* Item) const
{
    return Companion->CanReceiveCarriedItem(Item);
}
/// @brief 转给组件的 TryReceiveCarriedItem
bool ADeliveryCompanionCharacter::TryReceiveHandheldItem(ADeliveryHandheldItem* Item)
{
    return Companion->TryReceiveCarriedItem(Item);
}
/// @brief 只释放普通物品，转给组件的 ReleaseCarriedItem
bool ADeliveryCompanionCharacter::ReleaseHandheldItem(ADeliveryHandheldItem* Item)
{
    return DeliveryHandheldTransfer::IsOrdinaryItem(Item) && Companion->ReleaseCarriedItem(Item);
}
