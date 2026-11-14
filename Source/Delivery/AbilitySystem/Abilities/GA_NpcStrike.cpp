#include "AbilitySystem/Abilities/GA_NpcStrike.h"
#include "Combat/DeliveryNpcStrikeExecutor.h"
#include "GameFramework/Actor.h"

UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Npc_Strike,"Event.Npc.Strike");

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

void UGA_NpcStrike::OnStrikeFinished(FGuid ActionId,FGuid PunchId,bool bCancelled)
{
    if (!IsActive() || ActionId!=ExecutingActionId || PunchId!=ExecutingPunchId) return;
    bStrikeResolved=!bCancelled;
    EndAbility(CurrentSpecHandle,CurrentActorInfo,CurrentActivationInfo,false,bCancelled);
}

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
