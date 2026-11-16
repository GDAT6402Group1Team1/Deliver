#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "AI/Actions/DeliveryVehicleHoldComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "Inventory/Items/DeliveryPackageItem.h"
#include "Task/World/DeliveryItemComponent.h"

void ADeliveryMotorbike::RegisterPlayerUse(APlayerState* Player)
{
    if (HasAuthority() && AllowsPlayerUse() && !IsActorBeingDestroyed() && IsValid(Player) &&
        !Player->IsInactive() && Player->GetWorld() == GetWorld()) RegisteredPlayer = Player;
}

APlayerState* ADeliveryMotorbike::GetRegisteredPlayer() const { return RegisteredPlayer.Get(); }
APlayerState* ADeliveryMotorbike::GetCargoPlayerState() const { return GetCargo() ? CargoPlayer.Get() : nullptr; }
ADeliveryPackageItem* ADeliveryMotorbike::GetCarriedTaskPackage() const { return GetCargo(); }
APlayerState* ADeliveryMotorbike::GetTaskPlayerState() const
{
    return bCompletingCargoHandoff ? CargoTransferPlayer.Get() : GetCargoPlayerState();
}

FVector ADeliveryMotorbike::GetTaskPackageTransferPoint() const
{
    return TrunkAnchor ? TrunkAnchor->GetComponentLocation() : GetActorLocation();
}

bool ADeliveryMotorbike::CanHandleTaskPackage() const
{
    const auto* Player = GetTaskPlayerState();
    return AllowsPlayerUse() && !IsActorBeingDestroyed() && IsValid(Player) && !Player->IsInactive() &&
        Player->GetWorld() == GetWorld() && RegisteredPlayer.Get() == Player && !Driver && !HasTrunkPassenger() &&
        (!IsTrunkBusy() || bCompletingCargoHandoff) && !(ResidentHold && ResidentHold->IsHeld()) &&
        FMath::Abs(CurrentSpeed) <= 10.f && FMath::Abs(VerticalVelocity) <= 10.f && KnockVelocity.SizeSquared() <= 100.f;
}

bool ADeliveryMotorbike::CanReceiveTaskPackage(ADeliveryPackageItem* Package) const
{
    const auto* Item = IsValid(Package) ? Package->GetDeliveryItemComponent() : nullptr;
    // 车辆接口只为本次卸货失败回滚接收，不把交还指令变成往任意车装货
    return bCompletingCargoHandoff && CanHandleTaskPackage() && !HasCargo() && Item &&
        Package->GetWorld() == GetWorld() && !Package->GetOwner() && Item->CanBeAcquiredBy(this);
}

bool ADeliveryMotorbike::TryReceiveTaskPackage(ADeliveryPackageItem* Package)
{
    return CanReceiveTaskPackage(Package) && AttachCargoForPlayer(Package, CargoTransferPlayer.Get());
}

bool ADeliveryMotorbike::ReleaseTaskPackage(ADeliveryPackageItem* Package)
{
    return HasAuthority() && IsValid(Package) && GetCargo() == Package && ReleaseCargo() == Package;
}

void ADeliveryMotorbike::ApplyTaskPackagePresentation(ADeliveryPackageItem* Package)
{
    RefreshCargoPresentation(Package);
}

bool ADeliveryMotorbike::IsCargoHandoffCurrent(FGuid Id) const
{
    if (!Id.IsValid() || CargoHandoffId != Id || !HasAuthority() || IsActorBeingDestroyed() ||
        !CargoHandoffReceiver.IsValid() || CargoHandoffReceiver->IsActorBeingDestroyed() ||
        CargoHandoffReceiver->GetWorld() != GetWorld() || !CargoHandoffPackage.IsValid() ||
        GetCargo() != CargoHandoffPackage.Get() || CargoPlayer != CargoTransferPlayer ||
        RegisteredPlayer != CargoTransferPlayer || !CargoTransferPlayer.IsValid() ||
        CargoTransferPlayer->IsInactive() || Driver || HasTrunkPassenger() ||
        FMath::Abs(CurrentSpeed) > 10.f || FMath::Abs(VerticalVelocity) > 10.f || KnockVelocity.SizeSquared() > 100.f ||
        (ResidentHold && ResidentHold->IsHeld())) return false;
    const auto* Receiver = Cast<IDeliveryPackageCarrierInterface>(CargoHandoffReceiver.Get());
    if (!Receiver || Receiver->GetTaskPlayerState() != CargoTransferPlayer.Get() ||
        !Receiver->CanHandleTaskPackage() || !Receiver->CanReceiveTaskPackage(CargoHandoffPackage.Get())) return false;
    const FVector From = GetTaskPackageTransferPoint();
    const FVector To = Receiver->GetTaskPackageTransferPoint();
    if (From.ContainsNaN() || To.ContainsNaN() || FVector::DistSquared(From, To) > FMath::Square(240.f)) return false;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(CargoHandoff), false, this);
    Query.AddIgnoredActor(CargoHandoffReceiver.Get());
    Query.AddIgnoredActor(CargoHandoffPackage.Get());
    FCollisionObjectQueryParams Objects;
    for (const auto Channel : {ECC_WorldStatic, ECC_WorldDynamic, ECC_Pawn, ECC_Vehicle, ECC_PhysicsBody})
        Objects.AddObjectTypesToQuery(Channel);
    FHitResult Hit;
    if (!GetWorld() || GetWorld()->LineTraceSingleByObjectType(Hit, From, To, Objects, Query)) return false;
    // 回调可取消当前安排，复制函数后调用，随后复核原编号
    const auto Continue = CargoHandoffCanContinue;
    const TWeakObjectPtr<const ADeliveryMotorbike> WeakThis(this);
    return Continue && Continue() && WeakThis.IsValid() && !IsActorBeingDestroyed() && CargoHandoffId == Id;
}

FGuid ADeliveryMotorbike::RequestCargoHandoff(APawn* Receiver, ADeliveryPackageItem* ExpectedPackage,
    APlayerState* Player, TFunction<bool()> CanContinue)
{
    if (!HasAuthority() || bCompletingCargoHandoff || !CanHandleTaskPackage() || !IsValid(Receiver) || Receiver == this ||
        Receiver->GetWorld() != GetWorld() || !IsValid(ExpectedPackage) || GetCargo() != ExpectedPackage ||
        CargoPlayer.Get() != Player || RegisteredPlayer.Get() != Player || !CanContinue) return {};
    const auto* Carrier = Cast<IDeliveryPackageCarrierInterface>(Receiver);
    if (!Carrier || Carrier->GetTaskPlayerState() != Player || !Carrier->CanHandleTaskPackage() ||
        !Carrier->CanReceiveTaskPackage(ExpectedPackage)) return {};
    const auto* Item = ExpectedPackage->GetDeliveryItemComponent();
    if (!Item || Item->IsTransferInProgress()) return {};
    const FGuid Id = FGuid::NewGuid();
    CargoHandoffId = Id;
    CargoTransferPlayer = Player;
    CargoHandoffReceiver = Receiver;
    CargoHandoffPackage = ExpectedPackage;
    CargoHandoffCanContinue = MoveTemp(CanContinue);
    TrunkPendingReceiver.Reset();
    const TWeakObjectPtr<ADeliveryMotorbike> WeakThis(this);
    if (!IsCargoHandoffCurrent(Id))
    {
        if (WeakThis.IsValid()) CancelCargoHandoff(Id);
        return {};
    }
    MulticastTrunkAction(EDeliveryTrunkPhase::Unloading);
    return WeakThis.IsValid() && CargoHandoffId == Id ? Id : FGuid();
}

bool ADeliveryMotorbike::GetCargoHandoffResult(FGuid Id, bool& bComplete, bool& bSucceeded, FName& Reason) const
{
    bComplete = false; bSucceeded = false; Reason = NAME_None;
    if (!Id.IsValid()) return false;
    if (CargoHandoffId == Id) return true;
    if (LastCargoHandoffId != Id) return false;
    bComplete = true; bSucceeded = bLastCargoHandoffSucceeded; Reason = LastCargoHandoffReason;
    return true;
}

void ADeliveryMotorbike::FinishCargoHandoff(FGuid Id, bool bSucceeded, FName Reason)
{
    if (!HasAuthority() || !Id.IsValid() || CargoHandoffId != Id) return;
    CargoHandoffId.Invalidate();
    CargoHandoffReceiver.Reset();
    CargoHandoffPackage.Reset();
    CargoHandoffCanContinue = {};
    // 同步转移中的取消先使编号失效，授权快照保留到 Transfer 的回滚结束
    if (!bCompletingCargoHandoff) CargoTransferPlayer.Reset();
    LastCargoHandoffId = Id;
    LastCargoHandoffReason = Reason;
    bLastCargoHandoffSucceeded = bSucceeded;
    TrunkPendingReceiver.Reset();
    if (IsActorBeingDestroyed() || !GetWorld()) return;
    const TWeakObjectPtr<ADeliveryMotorbike> WeakThis(this);
    MulticastTrunkAction(EDeliveryTrunkPhase::Idle);
    if (WeakThis.IsValid() && !IsActorBeingDestroyed() && !CargoHandoffId.IsValid()) ApplyCargoPresentation();
}

void ADeliveryMotorbike::CancelCargoHandoff(FGuid Id)
{
    FinishCargoHandoff(Id, false, TEXT("CargoHandoffCancelled"));
}

void ADeliveryMotorbike::CompleteCargoHandoff()
{
    const FGuid Id = CargoHandoffId;
    const TWeakObjectPtr<ADeliveryMotorbike> WeakThis(this);
    if (!IsCargoHandoffCurrent(Id))
    {
        if (WeakThis.IsValid()) CancelCargoHandoff(Id);
        return;
    }
    auto* Receiver = CargoHandoffReceiver.Get();
    auto* Expected = CargoHandoffPackage.Get();
    const auto Continue = CargoHandoffCanContinue;
    bCompletingCargoHandoff = true;
    FName Reason;
    const bool bSucceeded = DeliveryPackageTransfer::TryTransfer(this, Receiver, Reason, Expected,
        [WeakThis, Id, Continue]()
        {
            return WeakThis.IsValid() && !WeakThis->IsActorBeingDestroyed() &&
                WeakThis->CargoHandoffId == Id && Continue && Continue() && WeakThis.IsValid() &&
                WeakThis->CargoHandoffId == Id;
        });
    if (!WeakThis.IsValid()) return;
    bCompletingCargoHandoff = false;
    CargoTransferPlayer.Reset();
    FinishCargoHandoff(Id, bSucceeded, bSucceeded ? FName(TEXT("CargoHandedOver")) : Reason);
}
