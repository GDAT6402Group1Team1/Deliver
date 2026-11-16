#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AbilitySystem/Abilities/GA_NpcStrike.h"
#include "AbilitySystemComponent.h"

// ── 战斗 ──
// NPC 出拳通过 GAS（技能系统）执行：Agent 或行为树调用 RequestStrike 或 RequestFightStrike，它们都调用 RequestStrikeAbility
// RequestStrikeAbility 发出 TAG_Event_Npc_Strike 事件，激活 GA_NpcStrike；GA_NpcStrike 再调用 StartNpcStrike，由战斗组件 Combat 挥拳
// GA_NpcStrike 用 GetNpcStrikeFinishedDelegate 等这一拳结束；这一拳还没结束 GA_NpcStrike 就结束或被取消时，它调用 CancelNpcStrike
// GetNpcStrikeResult 供查询某一拳打中没有

/// @brief IAbilitySystemInterface 要求的函数，返回这位 NPC 的 AbilitySystem
UAbilitySystemComponent* ADeliveryNpcCharacter::GetAbilitySystemComponent() const
{
    return AbilitySystem;
}

/// @brief 打一拳（StrikeOnce 动作）：这一拳的编号就用 ActionId，调用 RequestStrikeAbility
bool ADeliveryNpcCharacter::RequestStrike(AActor* Target, FGuid ActionId)
{
    return RequestStrikeAbility(Target, ActionId, ActionId, false);
}

/// @brief 交战（Fight 动作）中的一拳，行为树的 TickFight 每打一拳调用一次，PunchId 每拳不同
bool ADeliveryNpcCharacter::RequestFightStrike(AActor* Target, FGuid ActionId, FGuid PunchId)
{
    return RequestStrikeAbility(Target, ActionId, PunchId, true);
}

/// @brief 生成一个 UDeliveryNpcStrikeRequest，用 TAG_Event_Npc_Strike 事件激活 GA_NpcStrike
/// ExecutingAction 不是 ActionId（这个动作已经不在执行）时返回 false
/// @return HandleGameplayEvent 返回后，Combat 的 GetCurrentPunchId 等于 PunchId 且这一拳已有开始时间，说明这一拳真的开始了，返回 true
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

/// @brief GA_NpcStrike 激活后调用：再查一遍这个动作还在执行，才交给 Combat 挥拳
/// 要求 PublicState 的 ActionId、Target 和请求一致，Status 是 Running，动作类型和 bFightStrike 对得上
/// 动作来自游戏规则（Origin 为 GameplayRule，例如二狗护主）时，还要 CanExecuteGameplayRule 通过，主人取消后就不打
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

/// @brief 查询 PunchId 这一拳打中没有，转给 Combat->GetStrikeResult
bool ADeliveryNpcCharacter::GetNpcStrikeResult(FGuid PunchId, bool& bOutHit) const
{
    return Combat->GetStrikeResult(PunchId, bOutHit);
}

/// @brief GA_NpcStrike 在这一拳还没结束时就结束或被取消，调用这里，转给 Combat->CancelStrike 停止挥拳
void ADeliveryNpcCharacter::CancelNpcStrike(FGuid ActionId, FGuid PunchId)
{
    Combat->CancelStrike(ActionId, PunchId);
}

/// @brief 返回 Combat->OnStrikeFinished，GA_NpcStrike 订阅它等这一拳结束
FDeliveryNpcStrikeFinished& ADeliveryNpcCharacter::GetNpcStrikeFinishedDelegate()
{
    return Combat->OnStrikeFinished;
}

/// @brief 动作停止时调用：ActionId 是当前的 ExecutingAction，就调用 CancelAbilityHandle 取消 GA_NpcStrike
/// GA_NpcStrike 的 EndAbility 发现这一拳还没结束，会调用 CancelNpcStrike 停止挥拳
void ADeliveryNpcCharacter::CancelStrikeAbility(FGuid ActionId)
{
    if (!HasAuthority() || ActionId != ExecutingAction || !ActionId.IsValid())
        return;
    if (AbilitySystem && StrikeAbilityHandle.IsValid())
        AbilitySystem->CancelAbilityHandle(StrikeAbilityHandle);
}

// ── 受击 ──
// 玩家或别的 NPC 的拳打中这位 NPC 时，调用 ReceiveMeleeHitFrom，转给 Combat->ReceiveMeleeImpact 处理后仰、倒地和还手

/// @brief 倒地时还能不能被打，读 Combat->bReceiveHitsWhileDown
bool ADeliveryNpcCharacter::CanReceiveHitWhileDown() const
{
    return Combat && Combat->bReceiveHitsWhileDown;
}

/// @brief 被拳打中时调用，转给 Combat->ReceiveMeleeImpact；同一个 PunchId 在那里只算一次
void ADeliveryNpcCharacter::ReceiveMeleeHitFrom(const FVector& Impulse, const FVector& Point, AActor* Source, FGuid PunchId)
{
    if (Combat)
        Combat->ReceiveMeleeImpact(Impulse, Point, Source, PunchId);
}
