#pragma once
#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "NativeGameplayTags.h"
#include "GA_NpcStrike.generated.h"

UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Npc_Strike);

// 服务器接收单拳请求，通过执行接口等待现有接触判定，取消时只清理本次拳头
UCLASS(Blueprintable)
class DELIVERY_API UGA_NpcStrike : public UGameplayAbility
{
    GENERATED_BODY()
public:
    UGA_NpcStrike();
    virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo,const FGameplayEventData* TriggerEventData) override;
    virtual void EndAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo,bool bReplicateEndAbility,bool bWasCancelled) override;
protected:
    // 仅供蓝图补充音效特效，身体动画和命中窗口仍由执行者按服务器时间显示
    UFUNCTION(BlueprintImplementableEvent,Category="Delivery|NPC Strike")
    void OnStrikeStarted(AActor* Target,FGuid ActionId,FGuid PunchId);
private:
    void OnStrikeFinished(FGuid ActionId,FGuid PunchId,bool bCancelled);
    TWeakObjectPtr<AActor> StrikeExecutor;
    FDelegateHandle FinishedHandle;
    FGuid ExecutingActionId;
    FGuid ExecutingPunchId;
    bool bStrikeResolved=false;
};
