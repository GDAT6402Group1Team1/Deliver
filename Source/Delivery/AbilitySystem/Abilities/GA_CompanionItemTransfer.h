#pragma once
#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "NativeGameplayTags.h"
#include "GA_CompanionItemTransfer.generated.h"
/// 触发 GA_CompanionItemTransfer 的事件标签 Event.Companion.ItemTransfer
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Companion_ItemTransfer);

/**
 * @brief 二狗和主人之间交接手持物品的技能，只在服务器运行
 *
 * 二狗走到主人身边后，二狗组件发出 Event.Companion.ItemTransfer 事件，事件里带一个 UDeliveryHandheldUseRequest
 * ActivateAbility 找到二狗身上实现 IDeliveryHandheldUseExecutor 的组件，调用 ExecuteHandheldUse 当场交接，
 * 再调用 FinishHandheldUse 报告结果；走过去、镜头和更长的安排都不归它管
 */
UCLASS(Blueprintable)
class DELIVERY_API UGA_CompanionItemTransfer : public UGameplayAbility
{
    GENERATED_BODY()
public:
    /// @brief 每个角色一个实例，只在服务器运行，由 Event.Companion.ItemTransfer 事件触发
    UGA_CompanionItemTransfer();

    /// @brief 检查请求属于这个角色、执行组件存在，再调用 ExecuteHandheldUse 交接，最后调用 FinishHandheldUse 报告结果并结束技能
    virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo,const FGameplayEventData* TriggerEventData) override;

    /// @brief 技能结束；交接还没执行就被结束时，调用 FinishHandheldUse 报告 ItemAbilityCancelled
    virtual void EndAbility(FGameplayAbilitySpecHandle Handle,const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo,bool bReplicateEndAbility,bool bWasCancelled) override;
private:
    /// 执行交接的组件
    TWeakObjectPtr<UActorComponent> Executor;

    /// 这次交接的编号，请求里的 UseId
    FGuid ExecutingUse;

    /// 交接已经执行过（成功或失败）；为 false 时结束技能算取消
    bool bResolved=false;
};
