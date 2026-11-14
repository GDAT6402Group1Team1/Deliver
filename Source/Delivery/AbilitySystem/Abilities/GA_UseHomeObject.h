#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "NativeGameplayTags.h"
#include "GA_UseHomeObject.generated.h"

UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Home_Use);

// 只在服务器激活，复用执行接口管理家具使用与取消，不直接移动人物或物品
UCLASS(Blueprintable)
class DELIVERY_API UGA_UseHomeObject : public UGameplayAbility
{
    GENERATED_BODY()
public:
    UGA_UseHomeObject();
    virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo,const FGameplayEventData* TriggerEventData) override;
    virtual void EndAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo,bool bReplicateEndAbility,bool bWasCancelled) override;
private:
    void OnHomeUseFinished(FGuid ArrangementId,bool bSucceeded,FName Reason);
    TWeakObjectPtr<UActorComponent> Executor;
    FDelegateHandle FinishedHandle;
    FGuid ExecutingArrangement;
    bool bResolved=false;
};
