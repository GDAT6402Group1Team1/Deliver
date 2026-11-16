#include "AbilitySystem/Abilities/GA_CompanionItemTransfer.h"
#include "Carry/DeliveryHandheldUseExecutor.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"

UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Companion_ItemTransfer,"Event.Companion.ItemTransfer");

// ── 生命周期 ──

/// @brief 每个角色一个实例，只在服务器运行，由 Event.Companion.ItemTransfer 事件触发
UGA_CompanionItemTransfer::UGA_CompanionItemTransfer()
{
    InstancingPolicy=EGameplayAbilityInstancingPolicy::InstancedPerActor;
    NetExecutionPolicy=EGameplayAbilityNetExecutionPolicy::ServerOnly;
    NetSecurityPolicy=EGameplayAbilityNetSecurityPolicy::ServerOnly;
    FAbilityTriggerData Trigger;
    Trigger.TriggerTag=TAG_Event_Companion_ItemTransfer;
    Trigger.TriggerSource=EGameplayAbilityTriggerSource::GameplayEvent;
    AbilityTriggers.Add(Trigger);
}

/// @brief 检查请求属于这个角色、执行组件存在，再调用 ExecuteHandheldUse 交接，最后调用 FinishHandheldUse 报告结果并结束技能
void UGA_CompanionItemTransfer::ActivateAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
    FGameplayAbilityActivationInfo ActivationInfo,const FGameplayEventData* TriggerEventData)
{
    AActor* Avatar=ActorInfo?ActorInfo->AvatarActor.Get():nullptr;
    const auto* Request=TriggerEventData?Cast<UDeliveryHandheldUseRequest>(TriggerEventData->OptionalObject.Get()):nullptr;
    auto* Component=IsValid(Avatar)?Avatar->FindComponentByInterface(UDeliveryHandheldUseExecutor::StaticClass()):nullptr;
    auto* UseExecutor=Cast<IDeliveryHandheldUseExecutor>(Component);
    if (!IsValid(Avatar) || !Avatar->HasAuthority() || !UseExecutor || !Request || Request->GetOuter()!=Avatar ||
        TriggerEventData->Instigator.Get()!=Avatar || !Request->UseId.IsValid() || Component->GetOwner()!=Avatar ||
        Component->GetWorld()!=Avatar->GetWorld())
    { EndAbility(Handle,ActorInfo,ActivationInfo,false,true); return; }
    Executor=Component; ExecutingUse=Request->UseId; bResolved=false;
    const FGuid StartingUse=ExecutingUse;
    const TWeakObjectPtr<UGA_CompanionItemTransfer> WeakThis(this);
    if (!CommitAbility(Handle,ActorInfo,ActivationInfo))
    { EndAbility(Handle,ActorInfo,ActivationInfo,false,true); return; }
    if (!WeakThis.IsValid() || !IsActive() || ExecutingUse!=StartingUse || !Executor.IsValid()) return;
    FName Reason;
    const bool bSucceeded=UseExecutor->ExecuteHandheldUse(*Request,Reason);
    if (!WeakThis.IsValid() || !IsActive() || ExecutingUse!=StartingUse) return;
    bResolved=true;
    if (auto* Current=Cast<IDeliveryHandheldUseExecutor>(Executor.Get())) Current->FinishHandheldUse(StartingUse,bSucceeded,Reason);
    if (WeakThis.IsValid() && IsActive() && ExecutingUse==StartingUse)
        EndAbility(Handle,ActorInfo,ActivationInfo,false,!bSucceeded);
}

/// @brief 技能结束；交接还没执行就被结束时，调用 FinishHandheldUse 报告 ItemAbilityCancelled
void UGA_CompanionItemTransfer::EndAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
    FGameplayAbilityActivationInfo ActivationInfo,bool bReplicateEndAbility,bool bWasCancelled)
{
    const FGuid OldUse=ExecutingUse;
    auto* OldExecutor=Cast<IDeliveryHandheldUseExecutor>(Executor.Get());
    const bool bCancelled=!bResolved;
    Executor.Reset(); ExecutingUse.Invalidate(); bResolved=false;
    if (OldExecutor && bCancelled && OldUse.IsValid()) OldExecutor->FinishHandheldUse(OldUse,false,TEXT("ItemAbilityCancelled"));
    Super::EndAbility(Handle,ActorInfo,ActivationInfo,bReplicateEndAbility,bWasCancelled);
}
