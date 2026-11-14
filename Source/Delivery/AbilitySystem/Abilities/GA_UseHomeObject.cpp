#include "AbilitySystem/Abilities/GA_UseHomeObject.h"
#include "World/DeliveryHomeUseExecutor.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"

UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Home_Use,"Event.Home.Use");

UGA_UseHomeObject::UGA_UseHomeObject()
{
    InstancingPolicy=EGameplayAbilityInstancingPolicy::InstancedPerActor;
    NetExecutionPolicy=EGameplayAbilityNetExecutionPolicy::ServerOnly;
    NetSecurityPolicy=EGameplayAbilityNetSecurityPolicy::ServerOnly;
    FAbilityTriggerData Trigger;
    Trigger.TriggerTag=TAG_Event_Home_Use;
    Trigger.TriggerSource=EGameplayAbilityTriggerSource::GameplayEvent;
    AbilityTriggers.Add(Trigger);
}

void UGA_UseHomeObject::ActivateAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
    FGameplayAbilityActivationInfo ActivationInfo,const FGameplayEventData* TriggerEventData)
{
    AActor* Avatar=ActorInfo?ActorInfo->AvatarActor.Get():nullptr;
    const auto* Request=TriggerEventData?Cast<UDeliveryHomeUseRequest>(TriggerEventData->OptionalObject.Get()):nullptr;
    auto* Component=IsValid(Avatar)?Avatar->FindComponentByInterface(UDeliveryHomeUseExecutor::StaticClass()):nullptr;
    auto* HomeExecutor=Cast<IDeliveryHomeUseExecutor>(Component);
    if (!IsValid(Avatar) || !Avatar->HasAuthority() || !HomeExecutor || !Request || Request->GetOuter()!=Avatar ||
        TriggerEventData->Instigator.Get()!=Avatar || !Request->ArrangementId.IsValid() ||
        Component->GetOwner()!=Avatar || Component->GetWorld()!=Avatar->GetWorld() ||
        !CommitAbility(Handle,ActorInfo,ActivationInfo))
    {
        EndAbility(Handle,ActorInfo,ActivationInfo,false,true);
        return;
    }
    Executor=Component;
    ExecutingArrangement=Request->ArrangementId;
    bResolved=false;
    FinishedHandle=HomeExecutor->GetHomeUseFinishedDelegate().AddUObject(this,&ThisClass::OnHomeUseFinished);
    const FGuid StartingArrangement=ExecutingArrangement;
    const TWeakObjectPtr<UGA_UseHomeObject> WeakThis(this);
    const bool bStarted=HomeExecutor->StartHomeUse(*Request);
    // 开始回调允许同步结束，旧失败不能结束随后启动的新安排
    if (WeakThis.IsValid() && !bStarted && IsActive() && ExecutingArrangement==StartingArrangement)
        EndAbility(Handle,ActorInfo,ActivationInfo,false,true);
}

void UGA_UseHomeObject::OnHomeUseFinished(FGuid ArrangementId,bool bSucceeded,FName Reason)
{
    if (!IsActive() || ArrangementId!=ExecutingArrangement) return;
    bResolved=true;
    EndAbility(CurrentSpecHandle,CurrentActorInfo,CurrentActivationInfo,false,!bSucceeded);
}

void UGA_UseHomeObject::EndAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
    FGameplayAbilityActivationInfo ActivationInfo,bool bReplicateEndAbility,bool bWasCancelled)
{
    auto* HomeExecutor=Cast<IDeliveryHomeUseExecutor>(Executor.Get());
    const FDelegateHandle EndingHandle=FinishedHandle;
    const FGuid EndingArrangement=ExecutingArrangement;
    const bool bCancel=!bResolved;
    // 先解绑再通知取消，执行器的同步结束不能再次结束本能力或清理后续安排
    Executor.Reset(); FinishedHandle.Reset(); ExecutingArrangement.Invalidate(); bResolved=false;
    if (HomeExecutor)
    {
        HomeExecutor->GetHomeUseFinishedDelegate().Remove(EndingHandle);
        if (bCancel && EndingArrangement.IsValid()) HomeExecutor->CancelHomeUse(EndingArrangement,TEXT("AbilityCancelled"));
    }
    Super::EndAbility(Handle,ActorInfo,ActivationInfo,bReplicateEndAbility,bWasCancelled);
}
