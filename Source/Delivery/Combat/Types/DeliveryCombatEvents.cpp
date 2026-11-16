#include "Combat/Types/DeliveryCombatEvents.h"
#include "Character/DeliveryCharacter.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"
#include "UObject/StrongObjectPtr.h"

UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Combat_ConfirmedHit,"Event.Combat.ConfirmedHit");

/// @brief Actor 是玩家正在控制的角色时返回他的 PlayerState；控制器已经不控制它、PlayerState 失效或不在同一个世界时返回 nullptr
APlayerState* DeliveryCombatEvents::FindPlayerState(const AActor* Actor)
{
    if (!IsValid(Actor) || Actor->IsActorBeingDestroyed()) return nullptr;
    APlayerState* State=nullptr;
    if (const auto* Player=Cast<ADeliveryCharacter>(Actor)) State=Player->GetTaskPlayerState();
    else if (const auto* Pawn=Cast<APawn>(Actor); Pawn && Pawn->IsPlayerControlled()) State=Pawn->GetPlayerState();
    const auto* Controller=IsValid(State)?State->GetPlayerController():nullptr;
    return IsValid(Controller) && Controller->PlayerState==State && !State->IsInactive() &&
        Controller->GetWorld()==Actor->GetWorld() && State->GetWorld()==Actor->GetWorld()?State:nullptr;
}

/// @brief 服务器：建一个 UDeliveryCombatHitEvent，给出拳和被打的玩家的 ASC 各调用一次 HandleGameplayEvent
void DeliveryCombatEvents::ReportHit(AActor* Attacker,AActor* Target,FGuid PunchId,const FVector& Point)
{
    if (!IsInGameThread() || !IsValid(Attacker) || Attacker->IsActorBeingDestroyed() || !Attacker->HasAuthority() ||
        !IsValid(Target) || Target->IsActorBeingDestroyed() || !Target->HasAuthority() || Target==Attacker ||
        !Attacker->GetWorld() || Attacker->GetWorld()->GetNetMode()==NM_Client ||
        Target->GetWorld()!=Attacker->GetWorld() || !PunchId.IsValid() || Point.ContainsNaN()) return;
    // Event 只在这次函数调用期间有效；收到事件的代码要什么自己存下来，Event 只用弱引用指着双方，不让角色多活
    const TStrongObjectPtr<UDeliveryCombatHitEvent> Event(NewObject<UDeliveryCombatHitEvent>());
    Event->PunchId=PunchId; Event->Attacker=Attacker; Event->Target=Target; Event->ImpactPoint=Point;
    Event->AttackerPlayerState=FindPlayerState(Attacker);
    Event->TargetPlayerState=FindPlayerState(Target);
    FGameplayEventData Data;
    Data.EventTag=TAG_Event_Combat_ConfirmedHit;
    Data.Instigator=Attacker; Data.Target=Target; Data.OptionalObject=Event.Get();
    // 先把双方的 PlayerState 存进 Players 再逐个发：第一次通知里有人换了角色，第二次通知发给谁也不变
    TArray<TWeakObjectPtr<APlayerState>> Players;
    Players.AddUnique(Event->AttackerPlayerState);
    Players.AddUnique(Event->TargetPlayerState);
    for (const auto& Player:Players)
        if (auto* State=Player.Get(); IsValid(State) && !State->IsInactive())
            if (auto* AbilityOwner=Cast<IAbilitySystemInterface>(State))
                if (auto* ASC=AbilityOwner->GetAbilitySystemComponent(); IsValid(ASC) && ASC->GetWorld()==State->GetWorld())
                    ASC->HandleGameplayEvent(Data.EventTag,&Data);
}
