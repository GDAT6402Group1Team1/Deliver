#pragma once
#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "NativeGameplayTags.h"
#include "GA_NpcStrike.generated.h"

/// 触发 GA_NpcStrike 的事件标签 Event.Npc.Strike
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Npc_Strike);

/**
 * @brief NPC 挥一拳的技能，只在服务器运行
 *
 * NPC 的 RequestStrike 发出 Event.Npc.Strike 事件，事件里带一个 UDeliveryNpcStrikeRequest（目标、动作编号、这一拳的编号）
 * ActivateAbility 调用 NPC（实现 IDeliveryNpcStrikeExecutor）的 StartNpcStrike 开始挥拳，命中由战斗组件的拳头扫掠判断
 * 这一拳结束时 NPC 广播完成通知，OnStrikeFinished 结束技能；技能被取消时调用 CancelNpcStrike 只取消这一拳
 */
UCLASS(Blueprintable)
class DELIVERY_API UGA_NpcStrike : public UGameplayAbility
{
    GENERATED_BODY()
public:
    /// @brief 每个角色一个实例，只在服务器运行，由 Event.Npc.Strike 事件触发
    UGA_NpcStrike();

    /// @brief 检查请求属于这个 NPC、目标有效，订阅完成通知，再调用 StartNpcStrike 开始挥拳；开始成功后调用蓝图事件 OnStrikeStarted
    virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo,const FGameplayEventData* TriggerEventData) override;

    /// @brief 技能结束：取消订阅完成通知；这一拳还没打完就被结束时调用 CancelNpcStrike
    virtual void EndAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo,bool bReplicateEndAbility,bool bWasCancelled) override;
protected:
    /// @brief 蓝图事件：挥拳开始时调用，只用来加音效和特效；身体动画和命中时间由 NPC 自己按服务器时间播放
    UFUNCTION(BlueprintImplementableEvent,Category="Delivery|NPC Strike")
    void OnStrikeStarted(AActor* Target,FGuid ActionId,FGuid PunchId);
private:
    /// @brief NPC 广播这一拳结束时调用；编号和本次技能一致才结束技能
    void OnStrikeFinished(FGuid ActionId,FGuid PunchId,bool bCancelled);

    /// 挥拳的 NPC
    TWeakObjectPtr<AActor> StrikeExecutor;

    /// 完成通知的订阅句柄
    FDelegateHandle FinishedHandle;

    /// 这一拳所属的动作编号
    FGuid ExecutingActionId;

    /// 这一拳的编号
    FGuid ExecutingPunchId;

    /// 这一拳已经正常打完；为 false 时结束技能会取消这一拳
    bool bStrikeResolved=false;
};
