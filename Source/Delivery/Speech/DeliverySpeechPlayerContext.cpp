#include "Speech/DeliverySpeechPlayerContext.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"

/// @brief 记下 Player 现在的 PlayerState、世界和人物（骑车时记驾驶员）；记下后 Check 一次，不是 Valid 就清掉并返回 false
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

/// @brief 先查玩家、世界、PlayerState 和人物都还在；再按现在控制的是人物还是摩托车，分别查控制关系是否对得上
/// 某个字段在同步过程中暂时为空时返回 Pending，有值但对不上时返回 Invalid
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

    // 上车后人物的 Controller 和 PlayerState 会被清空，空值可以接受；有值但属于别人时返回 Invalid
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

/// @brief 清空记录
void FDeliverySpeechPlayerContext::Reset()
{
    Owner.Reset();
    PlayerState.Reset();
    World.Reset();
    OriginalPawn.Reset();
}

/// @brief 已经记下而且都还有效
bool FDeliverySpeechPlayerContext::IsCaptured() const
{
    return Owner.IsValid() && PlayerState.IsValid() && World.IsValid() && OriginalPawn.IsValid();
}

/// @brief 记下的人物
ADeliveryCharacter* FDeliverySpeechPlayerContext::GetCharacter() const
{
    return Cast<ADeliveryCharacter>(OriginalPawn.Get());
}
