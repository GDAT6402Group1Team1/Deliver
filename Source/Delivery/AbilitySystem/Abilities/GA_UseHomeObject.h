#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "NativeGameplayTags.h"
#include "GA_UseHomeObject.generated.h"

/// 触发 GA_UseHomeObject 的事件标签 Event.Home.Use
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Home_Use);

/**
 * @brief 二狗使用家具（坐下、躺下等）的技能，只在服务器运行
 *
 * 二狗组件发出 Event.Home.Use 事件，事件里带一个 UDeliveryHomeUseRequest（安排编号等）
 * ActivateAbility 找到实现 IDeliveryHomeUseExecutor 的组件，调用 StartHomeUse 开始；用完时执行组件广播完成通知，OnHomeUseFinished 结束技能
 * 技能被取消时调用 CancelHomeUse；人物和家具怎么移动由执行组件负责
 */
UCLASS(Blueprintable)
class DELIVERY_API UGA_UseHomeObject : public UGameplayAbility
{
    GENERATED_BODY()
public:
    /// @brief 每个角色一个实例，只在服务器运行，由 Event.Home.Use 事件触发
    UGA_UseHomeObject();

    /// @brief 检查请求属于这个角色、执行组件存在，订阅完成通知，再调用 StartHomeUse；没开始成功就结束技能
    virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo,const FGameplayEventData* TriggerEventData) override;

    /// @brief 技能结束：取消订阅完成通知；还没用完就被结束时调用 CancelHomeUse
    virtual void EndAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo,bool bReplicateEndAbility,bool bWasCancelled) override;
private:
    /// @brief 执行组件广播用完时调用；安排编号一致才结束技能
    void OnHomeUseFinished(FGuid ArrangementId,bool bSucceeded,FName Reason);

    /// 执行使用家具的组件
    TWeakObjectPtr<UActorComponent> Executor;

    /// 完成通知的订阅句柄
    FDelegateHandle FinishedHandle;

    /// 这次使用的安排编号
    FGuid ExecutingArrangement;

    /// 已经用完（成功或失败）；为 false 时结束技能会取消使用
    bool bResolved=false;
};
