#include "AbilitySystem/Abilities/GA_NpcStrike.h"
#include "Combat/Types/DeliveryNpcStrikeExecutor.h"
#include "GameFramework/Actor.h"

UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Npc_Strike,"Event.Npc.Strike");

// ── 生命周期 ──

/// @brief 每个角色一个实例，只在服务器运行，由 Event.Npc.Strike 事件触发
UGA_NpcStrike::UGA_NpcStrike()
{
    InstancingPolicy=EGameplayAbilityInstancingPolicy::InstancedPerActor;
    NetExecutionPolicy=EGameplayAbilityNetExecutionPolicy::ServerOnly;
    NetSecurityPolicy=EGameplayAbilityNetSecurityPolicy::ServerOnly;
    FAbilityTriggerData Trigger;
    Trigger.TriggerTag=TAG_Event_Npc_Strike;
    Trigger.TriggerSource=EGameplayAbilityTriggerSource::GameplayEvent;
    AbilityTriggers.Add(Trigger);
}

/// @brief 检查请求属于这个 NPC、目标有效，订阅完成通知，再调用 StartNpcStrike 开始挥拳；开始成功后调用蓝图事件 OnStrikeStarted
void UGA_NpcStrike::ActivateAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
    FGameplayAbilityActivationInfo ActivationInfo,const FGameplayEventData* TriggerEventData)
{
    AActor* Avatar=ActorInfo?ActorInfo->AvatarActor.Get():nullptr;
    const auto* Request=TriggerEventData?Cast<UDeliveryNpcStrikeRequest>(TriggerEventData->OptionalObject.Get()):nullptr;
    auto* Executor=Cast<IDeliveryNpcStrikeExecutor>(Avatar);
    if (!IsValid(Avatar) || !Avatar->HasAuthority() || !Executor || !Request || Request->GetOuter()!=Avatar ||
        TriggerEventData->Instigator.Get()!=Avatar || TriggerEventData->Target.Get()!=Request->Target.Get() ||
        !IsValid(Request->Target) || Request->Target->GetWorld()!=Avatar->GetWorld() ||
        !Request->ActionId.IsValid() || !Request->PunchId.IsValid() || !CommitAbility(Handle,ActorInfo,ActivationInfo))
    {
        EndAbility(Handle,ActorInfo,ActivationInfo,false,true);
        return;
    }
    StrikeExecutor=Avatar;
    ExecutingActionId=Request->ActionId;
    ExecutingPunchId=Request->PunchId;
    bStrikeResolved=false;
    FinishedHandle=Executor->GetNpcStrikeFinishedDelegate().AddUObject(this,&ThisClass::OnStrikeFinished);
    if (!Executor->StartNpcStrike(*Request))
    {
        EndAbility(Handle,ActorInfo,ActivationInfo,false,true);
        return;
    }
    if (IsActive()) OnStrikeStarted(Request->Target,ExecutingActionId,ExecutingPunchId);
}

/// @brief NPC 广播这一拳结束时调用；编号和本次技能一致才结束技能
void UGA_NpcStrike::OnStrikeFinished(FGuid ActionId,FGuid PunchId,bool bCancelled)
{
    if (!IsActive() || ActionId!=ExecutingActionId || PunchId!=ExecutingPunchId) return;
    bStrikeResolved=!bCancelled;
    EndAbility(CurrentSpecHandle,CurrentActorInfo,CurrentActivationInfo,false,bCancelled);
}

/// @brief 技能结束：取消订阅完成通知；这一拳还没打完就被结束时调用 CancelNpcStrike
void UGA_NpcStrike::EndAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
    FGameplayAbilityActivationInfo ActivationInfo,bool bReplicateEndAbility,bool bWasCancelled)
{
    auto* Executor=Cast<IDeliveryNpcStrikeExecutor>(StrikeExecutor.Get());
    const FDelegateHandle EndingHandle=FinishedHandle;
    const FGuid EndingAction=ExecutingActionId;
    const FGuid EndingPunch=ExecutingPunchId;
    const bool bCancelStrike=!bStrikeResolved;
    // 取消执行者可能同步结束其他动作，先解除本次技能对编号和回调的持有
    FinishedHandle.Reset(); StrikeExecutor.Reset();
    ExecutingActionId.Invalidate(); ExecutingPunchId.Invalidate(); bStrikeResolved=false;
    if (Executor)
    {
        Executor->GetNpcStrikeFinishedDelegate().Remove(EndingHandle);
        // 正常完成保留命中结果和收拳时间，行为树随后读取同一份结果
        if (bCancelStrike && EndingAction.IsValid() && EndingPunch.IsValid()) Executor->CancelNpcStrike(EndingAction,EndingPunch);
    }
    Super::EndAbility(Handle,ActorInfo,ActivationInfo,bReplicateEndAbility,bWasCancelled);
}
