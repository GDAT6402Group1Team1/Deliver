#include "Carry/DeliveryPackageCarrierInterface.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Task/World/DeliveryItemComponent.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "Delivery.h"

namespace DeliveryPackageTransfer
{
bool TryTransfer(AActor* From, AActor* To, FName& Reason)
{
    Reason = NAME_None;
    if (!IsInGameThread()) { Reason = TEXT("WrongThread"); return false; }
    if (!IsValid(From) || !IsValid(To) || From == To)
    { Reason = TEXT("InvalidCarrier"); return false; }
    if (!From->HasAuthority() || !To->HasAuthority())
    { Reason = TEXT("NotAuthority"); return false; }
    if (!From->GetWorld() || From->GetWorld() != To->GetWorld())
    { Reason = TEXT("WorldMismatch"); return false; }
    auto* Source = Cast<IDeliveryPackageCarrierInterface>(From);
    auto* Receiver = Cast<IDeliveryPackageCarrierInterface>(To);
    if (!Source || !Receiver) { Reason = TEXT("UnsupportedCarrier"); return false; }
    APlayerState* Player = Source->GetTaskPlayerState();
    if (!IsValid(Player) || Player->IsInactive() || Player->GetWorld() != From->GetWorld() ||
        Receiver->GetTaskPlayerState() != Player)
    { Reason = TEXT("OwnerMismatch"); return false; }
    if (!Source->CanHandleTaskPackage() || !Receiver->CanHandleTaskPackage())
    { Reason = TEXT("CarrierUnavailable"); return false; }
    auto* Package = Source->GetCarriedTaskPackage();
    auto* Item = IsValid(Package) ? Package->GetDeliveryItemComponent() : nullptr;
    if (!Item || Package->GetWorld() != From->GetWorld() || Package->GetOwner() != From)
    { Reason = TEXT("PackageOwnershipMismatch"); return false; }
    if (Item->IsTransferInProgress()) { Reason = TEXT("TransferBusy"); return false; }
    if (!Receiver->CanReceiveTaskPackage(Package))
    { Reason = TEXT("ReceiverUnavailable"); return false; }
    const auto ReachabilityFailure = [&]() -> FName
    {
        const FVector FromLocation = From->GetActorLocation();
        const FVector ToLocation = To->GetActorLocation();
        if (FromLocation.ContainsNaN() || ToLocation.ContainsNaN() ||
            FVector::DistSquared(FromLocation, ToLocation) > FMath::Square(240.f)) return TEXT("OutOfRange");
        FCollisionQueryParams Query(SCENE_QUERY_STAT(PackageTransfer), false, From);
        Query.AddIgnoredActor(To);
        Query.AddIgnoredActor(Package);
        FCollisionObjectQueryParams Objects;
        for (const auto Channel : {ECC_WorldStatic, ECC_WorldDynamic, ECC_Pawn, ECC_Vehicle, ECC_PhysicsBody})
            Objects.AddObjectTypesToQuery(Channel);
        FHitResult Hit;
        return From->GetWorld()->LineTraceSingleByObjectType(Hit, FromLocation, ToLocation, Objects, Query)
            ? FName(TEXT("Obstructed")) : NAME_None;
    };
    Reason = ReachabilityFailure();
    if (!Reason.IsNone()) return false;
    if (!Item->BeginTransfer(To)) { Reason = TEXT("TransferBusy"); return false; }
    ON_SCOPE_EXIT { if (IsValid(Item)) Item->EndTransfer(); };

    const FVector OriginalPackageLocation = Package->GetActorLocation();
    const auto HasPackage = [Package](AActor* Actor)
    {
        const auto* Carrier = IsValid(Actor) ? Cast<IDeliveryPackageCarrierInterface>(Actor) : nullptr;
        return IsValid(Package) && Carrier && Carrier->GetCarriedTaskPackage() == Package && Package->GetOwner() == Actor;
    };
    const auto IdentityStillValid = [&]()
    {
        return IsValid(From) && IsValid(To) && IsValid(Player) && !Player->IsInactive() &&
            Source->GetTaskPlayerState() == Player && Receiver->GetTaskPlayerState() == Player &&
            Source->CanHandleTaskPackage() && Receiver->CanHandleTaskPackage();
    };
    const auto RollBack = [&](FName Failure)
    {
        Reason = Failure;
        if (!IsValid(Package)) { Reason = TEXT("PackageDestroyedDuringTransfer"); return false; }
        if (HasPackage(From)) return false;
        // 接收接口可能已经入手才报告失败，先摘下该实物，再尝试回到原持有者
        if (HasPackage(To)) Receiver->ReleaseTaskPackage(Package);
        if (IsValid(Package) && !Package->GetOwner() && IsValid(Item) && IsValid(From))
        {
            // 两步之间没有广播或外部调用，预约不会在同步回调中暴露给第三方
            Item->EndTransfer();
            if (Item->BeginTransfer(From) && Source->CanReceiveTaskPackage(Package))
                Source->TryReceiveTaskPackage(Package);
            if (HasPackage(From)) return false;
        }
        // 无法回滚时保留原实物，解除无人持有的手掌表现，并明确上报异常
        if (IsValid(Package) && !Package->GetOwner())
            Package->DropFromInventory(OriginalPackageLocation, FVector::ZeroVector);
        Reason = IsValid(Package) ? TEXT("RollbackFailedPackagePreserved") : TEXT("PackageDestroyedDuringTransfer");
        UE_LOG(LogDelivery, Warning, TEXT("[PackageTransfer] %s source=%s receiver=%s package=%s"),
            *Reason.ToString(), *GetNameSafe(From), *GetNameSafe(To), *GetNameSafe(Package));
        return false;
    };

    if (!Source->ReleaseTaskPackage(Package)) return RollBack(TEXT("SourceReleaseFailed"));
    if (!IsValid(Package) || !IdentityStillValid()) return RollBack(TEXT("CarrierChangedDuringTransfer"));
    // 释放和接收都会广播事件，回调移动了双方或挡住通路时必须重新校验
    if (const FName Failure = ReachabilityFailure(); !Failure.IsNone()) return RollBack(Failure);
    if (Source->GetCarriedTaskPackage() == Package) return RollBack(TEXT("SourceReleaseFailed"));
    // 同步通知中只有预约的目标可能已经接收，实际归属一致即可确认结果
    if (HasPackage(To)) return true;
    if (Package->GetOwner() || !Receiver->CanReceiveTaskPackage(Package) || !Receiver->TryReceiveTaskPackage(Package))
        return RollBack(TEXT("ReceiverRejectedRolledBack"));
    if (!IdentityStillValid() || !HasPackage(To) || Source->GetCarriedTaskPackage() == Package)
        return RollBack(TEXT("ReceiverStateChangedRolledBack"));
    if (const FName Failure = ReachabilityFailure(); !Failure.IsNone()) return RollBack(Failure);
    return true;
}
}
