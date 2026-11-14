#include "Combat/DeliveryCombatEvents.h"
#include "Character/DeliveryCharacter.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"
#include "UObject/StrongObjectPtr.h"

UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Combat_ConfirmedHit,"Event.Combat.ConfirmedHit");

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

void DeliveryCombatEvents::ReportHit(AActor* Attacker,AActor* Target,FGuid PunchId,const FVector& Point)
{
    if (!IsInGameThread() || !IsValid(Attacker) || Attacker->IsActorBeingDestroyed() || !Attacker->HasAuthority() ||
        !IsValid(Target) || Target->IsActorBeingDestroyed() || !Target->HasAuthority() || Target==Attacker ||
        !Attacker->GetWorld() || Attacker->GetWorld()->GetNetMode()==NM_Client ||
        Target->GetWorld()!=Attacker->GetWorld() || !PunchId.IsValid() || Point.ContainsNaN()) return;
    // 事件仅在同步回调期间保活，接收者自行保存所需事实，不保活任一参战角色
    const TStrongObjectPtr<UDeliveryCombatHitEvent> Event(NewObject<UDeliveryCombatHitEvent>());
    Event->PunchId=PunchId; Event->Attacker=Attacker; Event->Target=Target; Event->ImpactPoint=Point;
    Event->AttackerPlayerState=FindPlayerState(Attacker);
    Event->TargetPlayerState=FindPlayerState(Target);
    FGameplayEventData Data;
    Data.EventTag=TAG_Event_Combat_ConfirmedHit;
    Data.Instigator=Attacker; Data.Target=Target; Data.OptionalObject=Event.Get();
    // 先捕获双方身份，第一次同步通知中的换 Pawn 不能改变第二次通知的归属
    TArray<TWeakObjectPtr<APlayerState>> Players;
    Players.AddUnique(Event->AttackerPlayerState);
    Players.AddUnique(Event->TargetPlayerState);
    for (const auto& Player:Players)
        if (auto* State=Player.Get(); IsValid(State) && !State->IsInactive())
            if (auto* AbilityOwner=Cast<IAbilitySystemInterface>(State))
                if (auto* ASC=AbilityOwner->GetAbilitySystemComponent(); IsValid(ASC) && ASC->GetWorld()==State->GetWorld())
                    ASC->HandleGameplayEvent(Data.EventTag,&Data);
}
