#include "Speech/DeliverySpeechPlayerContext.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"

bool FDeliverySpeechPlayerContext::Capture(APlayerController* Player)
{
    Reset();
    if (!IsValid(Player) || Player->IsActorBeingDestroyed() || !Player->GetWorld() ||
        !IsValid(Player->PlayerState) || Player->PlayerState->IsInactive()) return false;
    APawn* Pawn = Player->GetPawn();
    if (!IsValid(Pawn)) return false;
    Owner = Player;
    PlayerState = Player->PlayerState;
    World = Player->GetWorld();
    if (const auto* Bike = Cast<ADeliveryMotorbike>(Pawn)) OriginalPawn = Bike->GetDriver();
    else OriginalPawn = Pawn;
    if (Check(Player) == EDeliverySpeechPlayerContextState::Valid) return true;
    Reset();
    return false;
}

EDeliverySpeechPlayerContextState FDeliverySpeechPlayerContext::Check(const APlayerController* Player) const
{
    using EState = EDeliverySpeechPlayerContextState;
    if (!IsValid(Player) || Player->IsActorBeingDestroyed() || Owner.Get() != Player ||
        !World.IsValid() || World->bIsTearingDown || Player->GetWorld() != World.Get() || !PlayerState.IsValid() ||
        PlayerState->IsActorBeingDestroyed() || PlayerState->IsInactive() ||
        PlayerState->GetWorld() != World.Get() || !OriginalPawn.IsValid() ||
        OriginalPawn->IsActorBeingDestroyed() || OriginalPawn->GetWorld() != World.Get()) return EState::Invalid;
    if (Player->PlayerState && Player->PlayerState != PlayerState.Get()) return EState::Invalid;
    if (!Player->PlayerState) return EState::Pending;

    // 上车后原人物的 Controller 和 PlayerState 可被清空，非空的异主值仍立即拒绝
    if (OriginalPawn->GetController() && OriginalPawn->GetController() != Player) return EState::Invalid;
    if (OriginalPawn->GetPlayerState() && OriginalPawn->GetPlayerState() != PlayerState.Get()) return EState::Invalid;
    const APawn* Controlled = Player->GetPawn();
    if (!Controlled) return EState::Pending;
    if (!IsValid(Controlled) || Controlled->IsActorBeingDestroyed() || Controlled->GetWorld() != World.Get())
        return EState::Invalid;
    if (Controlled->GetController() && Controlled->GetController() != Player) return EState::Invalid;
    if (Controlled->GetPlayerState() && Controlled->GetPlayerState() != PlayerState.Get()) return EState::Invalid;

    if (Controlled == OriginalPawn.Get())
    {
        if (!Controlled->GetController()) return EState::Pending;
        if (const auto* Character = GetCharacter(); Character && Character->GetCurrentVehicle()) return EState::Pending;
        return EState::Valid;
    }

    const auto* Character = GetCharacter();
    const auto* Bike = Cast<ADeliveryMotorbike>(Controlled);
    if (!Character || !Bike) return EState::Invalid;
    if (Bike->GetDriver() && Bike->GetDriver() != Character) return EState::Invalid;
    if (Character->GetAttachParentActor() && Character->GetAttachParentActor() != Bike) return EState::Invalid;
    if (Character->GetTaskPlayerState() && Character->GetTaskPlayerState() != PlayerState.Get()) return EState::Invalid;
    const auto* CurrentVehicle = Character->GetCurrentVehicle();
    if (CurrentVehicle && CurrentVehicle != Bike) return EState::Invalid;
    if (!Controlled->GetController() || !Bike->GetDriver() || !CurrentVehicle) return EState::Pending;
    return EState::Valid;
}

void FDeliverySpeechPlayerContext::Reset()
{
    Owner.Reset();
    PlayerState.Reset();
    World.Reset();
    OriginalPawn.Reset();
}

bool FDeliverySpeechPlayerContext::IsCaptured() const
{
    return Owner.IsValid() && PlayerState.IsValid() && World.IsValid() && OriginalPawn.IsValid();
}

ADeliveryCharacter* FDeliverySpeechPlayerContext::GetCharacter() const
{
    return Cast<ADeliveryCharacter>(OriginalPawn.Get());
}
