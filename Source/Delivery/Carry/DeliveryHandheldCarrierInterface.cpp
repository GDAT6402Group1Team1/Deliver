#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"

namespace
{
    bool IsLiveActor(const AActor* Actor,const UWorld* World)
    {
        return IsValid(Actor) && !Actor->IsActorBeingDestroyed() && Actor->HasAuthority() && Actor->GetWorld()==World;
    }

    bool HasClearReach(AActor* From,AActor* To,ADeliveryHandheldItem* Item,const FVector& Goal,float Radius)
    {
        if (!IsValid(From) || !IsValid(To) || !IsValid(Item) || !From->GetWorld() ||
            Goal.ContainsNaN() || From->GetActorLocation().ContainsNaN() || !FMath::IsFinite(Radius) || Radius<=0 ||
            FVector::DistSquared(From->GetActorLocation(),Goal)>FMath::Square(Radius)) return false;
        FCollisionQueryParams Query(SCENE_QUERY_STAT(HandheldTransfer),false,From);
        Query.AddIgnoredActor(To);
        Query.AddIgnoredActor(Item);
        FCollisionObjectQueryParams Objects;
        for (const auto Channel:{ECC_WorldStatic,ECC_WorldDynamic,ECC_Pawn,ECC_Vehicle,ECC_PhysicsBody})
            Objects.AddObjectTypesToQuery(Channel);
        FHitResult Hit;
        return !From->GetWorld()->LineTraceSingleByObjectType(Hit,From->GetActorLocation(),Goal,Objects,Query);
    }
}

bool DeliveryHandheldTransfer::IsOrdinaryItem(const ADeliveryHandheldItem* Item)
{
    const auto* Data=IsValid(Item)?Item->GetItemComponent():nullptr;
    return IsValid(Item) && !Item->IsActorBeingDestroyed() && Data && Data->ItemType==EDeliveryInventoryItemType::BackpackItem &&
        !Item->IsA<ADeliveryPackageItem>() && !Item->FindComponentByClass<UDeliveryItemComponent>() &&
        !Item->FindComponentByClass<UDeliveryGrabbableComponent>();
}

bool DeliveryHandheldTransfer::IsFetchableItem(const ADeliveryHandheldItem* Item)
{
    return IsOrdinaryItem(Item) && Item->GetItemComponent()->bCompanionCanFetch && !Item->GetItemComponent()->DisplayName.IsEmpty();
}

bool DeliveryHandheldTransfer::TryPickupWorldItem(AActor* Receiver,ADeliveryHandheldItem* Item,FName& Reason,
    const TFunction<bool()>& CanContinue)
{
    Reason=NAME_None;
    UWorld* World=IsValid(Receiver)?Receiver->GetWorld():nullptr;
    auto* Carrier=IsValid(Receiver)?Cast<IDeliveryHandheldCarrierInterface>(Receiver):nullptr;
    auto* Pawn=Cast<APawn>(Receiver);
    auto* Data=IsValid(Item)?Item->GetItemComponent():nullptr;
    auto* Interaction=IsValid(Item)?UDeliveryInteractableComponent::FindOn(Item,EDeliveryInteractionKey::PickupE):nullptr;
    if (!IsInGameThread() || !World || (CanContinue && !CanContinue()) || !IsLiveActor(Receiver,World) || !IsLiveActor(Item,World) || !Pawn || !Carrier ||
        !IsFetchableItem(Item) || Item->GetOwner() || Item->GetAttachParentActor() || Item->IsHidden() ||
        !Item->GetItemMesh() || Item->GetRootComponent()!=Item->GetItemMesh() || !Item->GetItemMesh()->IsSimulatingPhysics() ||
        !Interaction || !Interaction->CanInteract(Pawn) || !Carrier->CanHandleHandheldItem() ||
        !Carrier->CanReceiveHandheldItem(Item))
    { Reason=TEXT("WorldItemUnavailable"); return false; }
    APlayerState* Owner=Carrier->GetHandheldOwnerPlayerState();
    if (!IsValid(Owner) || Owner->IsInactive() || Owner->GetWorld()!=World)
    { Reason=TEXT("OwnerUnavailable"); return false; }
    const FVector Point=Interaction->GetInteractionPoint(Pawn->GetActorLocation());
    const float PickupReach=Interaction->InteractRadius+Pawn->GetSimpleCollisionRadius();
    // CanInteract 使用物体表面距离，额外射线仍覆盖角色中心到真实交互点
    if (!HasClearReach(Pawn,Item,Item,Point,PickupReach))
    { Reason=TEXT("OutOfReachOrObstructed"); return false; }
    const FGuid TransferId=Data->BeginTransfer(Receiver);
    if (!TransferId.IsValid()) { Reason=TEXT("TransferBusy"); return false; }
    const TWeakObjectPtr<UDeliveryInventoryItemComponent> WeakData(Data);
    const TWeakObjectPtr<AActor> WeakReceiver(Receiver);
    const TWeakObjectPtr<ADeliveryHandheldItem> WeakItem(Item);
    const TWeakObjectPtr<APlayerState> WeakOwner(Owner);
    ON_SCOPE_EXIT { if (WeakData.IsValid()) WeakData->EndTransfer(TransferId); };
    const FVector OriginalLocation=Item->GetActorLocation();
    const bool bReceived=Carrier->TryReceiveHandheldItem(Item);
    Receiver=WeakReceiver.Get(); Item=WeakItem.Get(); Carrier=Cast<IDeliveryHandheldCarrierInterface>(Receiver);
    if (bReceived && (!CanContinue || CanContinue()) && IsLiveActor(Receiver,World) && IsLiveActor(Item,World) && Carrier && WeakOwner.IsValid() &&
        !WeakOwner->IsInactive() && Carrier->GetHandheldOwnerPlayerState()==WeakOwner.Get() &&
        Carrier->CanHandleHandheldItem() && Carrier->OwnsHandheldItem(Item) && Item->GetOwner()==Receiver &&
        HasClearReach(Receiver,Item,Item,Point,PickupReach)) return true;
    // 接收回调可能改变主人或销毁对象，只清理本次仍占有的实物
    if (IsLiveActor(Receiver,World) && IsLiveActor(Item,World) && Carrier && Carrier->OwnsHandheldItem(Item))
        Carrier->ReleaseHandheldItem(Item);
    if (WeakItem.IsValid() && !WeakItem->GetOwner()) WeakItem->DropFromInventory(OriginalLocation,FVector::ZeroVector);
    Reason=WeakItem.IsValid()?TEXT("WorldPickupRejectedItemPreserved"):TEXT("ItemDestroyedDuringPickup");
    return false;
}

bool DeliveryHandheldTransfer::TryTransfer(AActor* From,AActor* To,ADeliveryHandheldItem* Item,FName& Reason,
    const TFunction<bool()>& CanContinue)
{
    Reason=NAME_None;
    UWorld* World=IsValid(From)?From->GetWorld():nullptr;
    auto* Source=IsValid(From)?Cast<IDeliveryHandheldCarrierInterface>(From):nullptr;
    auto* Receiver=IsValid(To)?Cast<IDeliveryHandheldCarrierInterface>(To):nullptr;
    if (!IsInGameThread() || !World || From==To || !IsLiveActor(From,World) || !IsLiveActor(To,World) ||
        !IsLiveActor(Item,World) || !Source || !Receiver || !IsOrdinaryItem(Item) ||
        !Source->OwnsHandheldItem(Item) || Item->GetOwner()!=From)
    { Reason=TEXT("InvalidItemOrCarrier"); return false; }
    const TWeakObjectPtr<AActor> WeakFrom(From),WeakTo(To);
    const TWeakObjectPtr<ADeliveryHandheldItem> WeakItem(Item);
    const TWeakObjectPtr<APlayerState> WeakOwner(Source->GetHandheldOwnerPlayerState());
    const auto IdentityValid=[&]()
    {
        auto* A=Cast<IDeliveryHandheldCarrierInterface>(WeakFrom.Get());
        auto* B=Cast<IDeliveryHandheldCarrierInterface>(WeakTo.Get());
        return (!CanContinue || CanContinue()) && IsLiveActor(WeakFrom.Get(),World) && IsLiveActor(WeakTo.Get(),World) && IsLiveActor(WeakItem.Get(),World) &&
            WeakOwner.IsValid() && !WeakOwner->IsInactive() && WeakOwner->GetWorld()==World && A && B &&
            A->GetHandheldOwnerPlayerState()==WeakOwner.Get() && B->GetHandheldOwnerPlayerState()==WeakOwner.Get() &&
            A->CanHandleHandheldItem() && B->CanHandleHandheldItem();
    };
    const auto InReach=[&]()
    {
        return WeakTo.IsValid() && HasClearReach(WeakFrom.Get(),WeakTo.Get(),WeakItem.Get(),WeakTo->GetActorLocation(),240.f);
    };
    if (!IdentityValid()) { Reason=TEXT("OwnerOrCarrierUnavailable"); return false; }
    if (!Receiver->CanReceiveHandheldItem(Item)) { Reason=TEXT("ReceiverInventoryFullOrBusy"); return false; }
    if (!InReach()) { Reason=TEXT("OutOfReachOrObstructed"); return false; }
    auto* Data=Item->GetItemComponent();
    const FGuid TransferId=Data->BeginTransfer(To);
    if (!TransferId.IsValid()) { Reason=TEXT("TransferBusy"); return false; }
    const TWeakObjectPtr<UDeliveryInventoryItemComponent> WeakData(Data);
    ON_SCOPE_EXIT { if (WeakData.IsValid()) WeakData->EndTransfer(TransferId); };
    const FVector OriginalLocation=Item->GetActorLocation();
    const auto HasItem=[&](AActor* Actor)
    {
        const auto* Carrier=IsValid(Actor)?Cast<IDeliveryHandheldCarrierInterface>(Actor):nullptr;
        return WeakItem.IsValid() && Carrier && Carrier->OwnsHandheldItem(WeakItem.Get()) && WeakItem->GetOwner()==Actor;
    };
    const auto Rollback=[&](FName Failure)
    {
        Reason=Failure;
        if (!WeakItem.IsValid()) { Reason=TEXT("ItemDestroyedDuringTransfer"); return false; }
        if (HasItem(WeakFrom.Get())) return false;
        if (HasItem(WeakTo.Get()))
            Cast<IDeliveryHandheldCarrierInterface>(WeakTo.Get())->ReleaseHandheldItem(WeakItem.Get());
        if (WeakItem.IsValid() && !WeakItem->GetOwner() && IsLiveActor(WeakFrom.Get(),World) && WeakData.IsValid() &&
            WeakData->RetargetTransfer(TransferId,WeakFrom.Get()))
        {
            auto* OldCarrier=Cast<IDeliveryHandheldCarrierInterface>(WeakFrom.Get());
            if (WeakOwner.IsValid() && !WeakOwner->IsInactive() && OldCarrier && OldCarrier->GetHandheldOwnerPlayerState()==WeakOwner.Get() &&
                OldCarrier->CanReceiveHandheldItem(WeakItem.Get())) OldCarrier->TryReceiveHandheldItem(WeakItem.Get());
            if (HasItem(WeakFrom.Get())) return false;
        }
        if (WeakItem.IsValid() && !WeakItem->GetOwner()) WeakItem->DropFromInventory(OriginalLocation,FVector::ZeroVector);
        Reason=WeakItem.IsValid()?TEXT("RollbackFailedItemPreserved"):TEXT("ItemDestroyedDuringTransfer");
        return false;
    };
    if (!Source->ReleaseHandheldItem(Item)) return Rollback(TEXT("SourceReleaseFailed"));
    if (!IdentityValid() || !InReach() || HasItem(WeakFrom.Get())) return Rollback(TEXT("StateChangedDuringTransfer"));
    // 回调仅可完成本次预约的接收，再次核验后才能确认同一实物已交还
    if (HasItem(WeakTo.Get())) return true;
    Receiver=Cast<IDeliveryHandheldCarrierInterface>(WeakTo.Get());
    if (WeakItem->GetOwner() || !Receiver->CanReceiveHandheldItem(WeakItem.Get()) || !Receiver->TryReceiveHandheldItem(WeakItem.Get()))
        return Rollback(TEXT("ReceiverRejected"));
    if (!IdentityValid() || !InReach() || !HasItem(WeakTo.Get()) || HasItem(WeakFrom.Get()))
        return Rollback(TEXT("ReceiverChangedDuringTransfer"));
    return true;
}
