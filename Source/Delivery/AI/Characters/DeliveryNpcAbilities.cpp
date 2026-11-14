#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AbilitySystem/Abilities/GA_NpcStrike.h"
#include "AbilitySystemComponent.h"

UAbilitySystemComponent* ADeliveryNpcCharacter::GetAbilitySystemComponent() const { return AbilitySystem; }
bool ADeliveryNpcCharacter::RequestStrike(AActor* Target,FGuid ActionId)
{ return RequestStrikeAbility(Target,ActionId,ActionId,false); }
bool ADeliveryNpcCharacter::RequestFightStrike(AActor* Target,FGuid ActionId,FGuid PunchId)
{ return RequestStrikeAbility(Target,ActionId,PunchId,true); }
bool ADeliveryNpcCharacter::RequestStrikeAbility(AActor* Target,FGuid ActionId,FGuid PunchId,bool bFight)
{
    if (!HasAuthority() || !AbilitySystem || !StrikeAbilityHandle.IsValid() || ExecutingAction!=ActionId) return false;
    auto* Request=NewObject<UDeliveryNpcStrikeRequest>(this);
    Request->Target=Target; Request->ActionId=ActionId; Request->PunchId=PunchId; Request->bFightStrike=bFight;
    FGameplayEventData Event;
    Event.EventTag=TAG_Event_Npc_Strike; Event.Instigator=this; Event.Target=Target; Event.OptionalObject=Request;
    AbilitySystem->HandleGameplayEvent(Event.EventTag,&Event);
    return Combat->GetCurrentPunchId()==PunchId && Combat->GetStrikeStartTime(ActionId)>=0;
}
bool ADeliveryNpcCharacter::StartNpcStrike(const UDeliveryNpcStrikeRequest& Request)
{
    const auto State=Agent->GetPublicState();
    if (!HasAuthority() || Request.GetOuter()!=this || ExecutingAction!=Request.ActionId ||
        State.ActionId!=Request.ActionId || State.Status!=EDeliveryNpcActionStatus::Running || State.Target!=Request.Target ||
        (Request.bFightStrike?State.Action!=EDeliveryNpcAction::Fight:State.Action!=EDeliveryNpcAction::StrikeOnce)) return false;
    if (State.Origin==EDeliveryNpcActionOrigin::GameplayRule && !CanExecuteGameplayRule(State.Action,State.Target,State.RuleRevision)) return false;
    return Request.bFightStrike?Combat->StartFightStrike(Request.Target,Request.ActionId,Request.PunchId):Combat->StartStrike(Request.Target,Request.PunchId);
}
bool ADeliveryNpcCharacter::GetNpcStrikeResult(FGuid PunchId,bool& bOutHit) const { return Combat->GetStrikeResult(PunchId,bOutHit); }
void ADeliveryNpcCharacter::CancelNpcStrike(FGuid ActionId,FGuid PunchId) { Combat->CancelStrike(ActionId,PunchId); }
FDeliveryNpcStrikeFinished& ADeliveryNpcCharacter::GetNpcStrikeFinishedDelegate() { return Combat->OnStrikeFinished; }
void ADeliveryNpcCharacter::CancelStrikeAbility(FGuid ActionId)
{
    if (!HasAuthority() || ActionId!=ExecutingAction || !ActionId.IsValid()) return;
    if (AbilitySystem && StrikeAbilityHandle.IsValid()) AbilitySystem->CancelAbilityHandle(StrikeAbilityHandle);
}
bool ADeliveryNpcCharacter::CanReceiveHitWhileDown() const { return Combat && Combat->bReceiveHitsWhileDown; }
void ADeliveryNpcCharacter::ReceiveMeleeHitFrom(const FVector& Impulse,const FVector& Point,AActor* Source,FGuid PunchId)
{
    if (Combat) Combat->ReceiveMeleeImpact(Impulse,Point,Source,PunchId);
}
