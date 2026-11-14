#pragma once
#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "NativeGameplayTags.h"
#include "GA_CompanionItemTransfer.generated.h"
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Companion_ItemTransfer);

// 只执行已接近后的服务器交接，不控制导航、镜头或长途安排
UCLASS(Blueprintable)
class DELIVERY_API UGA_CompanionItemTransfer : public UGameplayAbility
{
    GENERATED_BODY()
public:
    UGA_CompanionItemTransfer();
    virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo,const FGameplayEventData* TriggerEventData) override;
    virtual void EndAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo,bool bReplicateEndAbility,bool bWasCancelled) override;
private:
    TWeakObjectPtr<UActorComponent> Executor;
    FGuid ExecutingUse;
    bool bResolved=false;
};
