#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AbilitySystem/Abilities/GA_NpcStrike.h"
#include "AbilitySystemComponent.h"

// GAS 接口：返回本人物的技能组件
UAbilitySystemComponent* ADeliveryNpcCharacter::GetAbilitySystemComponent() const
{
    return AbilitySystem;
}

// 单次出拳，转交 RequestStrikeAbility
bool ADeliveryNpcCharacter::RequestStrike(AActor* Target, FGuid ActionId)
{
    return RequestStrikeAbility(Target, ActionId, ActionId, false);
}

// 交战中的一拳，行为树 TickFight 调用
bool ADeliveryNpcCharacter::RequestFightStrike(AActor* Target, FGuid ActionId, FGuid PunchId)
{
    return RequestStrikeAbility(Target, ActionId, PunchId, true);
}

// 交战流程第 18 步的开始：构造出拳请求，以 TAG_Event_Npc_Strike 事件激活 GA_NpcStrike
bool ADeliveryNpcCharacter::RequestStrikeAbility(AActor* Target, FGuid ActionId, FGuid PunchId, bool bFight)
{
    if (!HasAuthority() || !AbilitySystem || !StrikeAbilityHandle.IsValid() || ExecutingAction != ActionId)
        return false;
    auto* Request = NewObject<UDeliveryNpcStrikeRequest>(this);
    Request->Target = Target;
    Request->ActionId = ActionId;
    Request->PunchId = PunchId;
    Request->bFightStrike = bFight;
    FGameplayEventData Event;
    Event.EventTag = TAG_Event_Npc_Strike;
    Event.Instigator = this;
    Event.Target = Target;
    Event.OptionalObject = Request;
    AbilitySystem->HandleGameplayEvent(Event.EventTag, &Event);
    return Combat->GetCurrentPunchId() == PunchId && Combat->GetStrikeStartTime(ActionId) >= 0;
}

// GA_NpcStrike 激活后回调：复核动作仍在执行，交给战斗组件开始挥拳
bool ADeliveryNpcCharacter::StartNpcStrike(const UDeliveryNpcStrikeRequest& Request)
{
    const auto State = Agent->GetPublicState();
    if (!HasAuthority() || Request.GetOuter() != this || ExecutingAction != Request.ActionId ||
        State.ActionId != Request.ActionId || State.Status != EDeliveryNpcActionStatus::Running ||
        State.Target != Request.Target ||
        (Request.bFightStrike ? State.Action != EDeliveryNpcAction::Fight : State.Action != EDeliveryNpcAction::StrikeOnce))
        return false;
    if (State.Origin == EDeliveryNpcActionOrigin::GameplayRule &&
        !CanExecuteGameplayRule(State.Action, State.Target, State.RuleRevision))
        return false;
    return Request.bFightStrike ? Combat->StartFightStrike(Request.Target, Request.ActionId, Request.PunchId)
                                : Combat->StartStrike(Request.Target, Request.PunchId);
}

// GA_NpcStrike 查询这一拳是否命中
bool ADeliveryNpcCharacter::GetNpcStrikeResult(FGuid PunchId, bool& bOutHit) const
{
    return Combat->GetStrikeResult(PunchId, bOutHit);
}

// GA_NpcStrike 结束或被取消时停止挥拳
void ADeliveryNpcCharacter::CancelNpcStrike(FGuid ActionId, FGuid PunchId)
{
    Combat->CancelStrike(ActionId, PunchId);
}

// GA_NpcStrike 订阅出拳结束事件
FDeliveryNpcStrikeFinished& ADeliveryNpcCharacter::GetNpcStrikeFinishedDelegate()
{
    return Combat->OnStrikeFinished;
}

// 动作停止时取消正在进行的出拳技能
void ADeliveryNpcCharacter::CancelStrikeAbility(FGuid ActionId)
{
    if (!HasAuthority() || ActionId != ExecutingAction || !ActionId.IsValid())
        return;
    if (AbilitySystem && StrikeAbilityHandle.IsValid())
        AbilitySystem->CancelAbilityHandle(StrikeAbilityHandle);
}

// 倒地时是否还能被打，由战斗组件配置决定
bool ADeliveryNpcCharacter::CanReceiveHitWhileDown() const
{
    return Combat && Combat->bReceiveHitsWhileDown;
}

// 被拳头打中，转交战斗组件 ReceiveMeleeImpact
void ADeliveryNpcCharacter::ReceiveMeleeHitFrom(const FVector& Impulse, const FVector& Point, AActor* Source, FGuid PunchId)
{
    if (Combat)
        Combat->ReceiveMeleeImpact(Impulse, Point, Source, PunchId);
}
