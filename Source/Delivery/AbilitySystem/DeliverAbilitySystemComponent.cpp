

#include "AbilitySystem/DeliverAbilitySystemComponent.h"
#include "AbilitySystem/DeliverAttributeSet.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "Character/DeliveryCharacter.h"
#include "../Ragdoll/DeliveryActiveRagdollComponent.h"
#include "../Grab/DeliveryGrabComponent.h"
#include "../Grab/DeliveryGrabbableComponent.h"
#include "../Inventory/DeliveryInventoryComponent.h"
#include "Abilities/GameplayAbility.h"
#include "Abilities/GA_DeliverPunch.h"
#include "GameplayEffect.h"
#include "Vehicle/Trunk/DeliveryTrunkPassengerComponent.h"

// ── 生命周期 ──

/// @brief 同步给所有客户端，复制方式为 Mixed：本机玩家收到完整的效果和属性，其他客户端只收到标签和特效
UDeliverAbilitySystemComponent::UDeliverAbilitySystemComponent()
{
	// ASC 跟随 PlayerState 复制
	SetIsReplicatedByDefault(true);

	// Mixed：数据由服务器权威计算并下发；本机玩家收到完整 GE 和 Attribute，其他客户端只收到 Tag 和 Cue
	SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
}

/// @brief 设置技能的拥有者（PlayerState）和执行者（角色），从角色读取配置；服务器上第一次调用时授予左右拳技能并订阅血量变化
void UDeliverAbilitySystemComponent::InitializeAbilityActor(AActor* Owner, AActor* Avatar)
{
	/** 初始化技能拥有者和实际执行技能的角色 */
	InitAbilityActorInfo(Owner, Avatar);

	/** InitAbilityActorInfo之后 */
	// 获取回血 GE
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Avatar))
	{
		HealthRegenEffect = Character->GetHealthRegenEffect();
		PunchLeftAbilityClass = Character->GetPunchLeftAbilityClass();
		PunchRightAbilityClass = Character->GetPunchRightAbilityClass();
		StunRecoverHealthPercent = Character->GetStunRecoverHealthPercent();
	}

	if (GetOwner()->HasAuthority() && Avatar && !bAbilitiesGranted)
	{
		bAbilitiesGranted = true;
		if (PunchLeftAbilityClass)
		{
			GiveAbility(FGameplayAbilitySpec(PunchLeftAbilityClass, 1, INDEX_NONE, Avatar));
		}
		if (PunchRightAbilityClass)
		{
			GiveAbility(FGameplayAbilitySpec(PunchRightAbilityClass, 1, INDEX_NONE, Avatar));
		}
	}

	// 绑定生命值变化事件
	if (!GetOwner()->HasAuthority() || !Avatar || bRegenWatchBound)
	{
		return;
	}

	bRegenWatchBound = true;
	GetGameplayAttributeValueChangeDelegate(UDeliverAttributeSet::GetHealthAttribute())
	    .AddUObject(this, &UDeliverAbilitySystemComponent::OnHealthChanged);
}

// ── 晕倒 ──

/// @brief 服务器：血量变化时调用；没晕倒时血量到 0 就调用 SetStunned(true)，晕倒时血量回到起身线就调用 SetStunned(false)
/// 在后备箱里时血量到 40% 调用 RequestExit 请求出箱；血量低于上限、而且还没有回血效果时应用 HealthRegenEffect
void UDeliverAbilitySystemComponent::OnHealthChanged(const FOnAttributeChangeData& Data)
{
	const float MaxHealth = GetNumericAttribute(UDeliverAttributeSet::GetMaxHealthAttribute());

	// 生命值归零时晕倒，恢复到指定比例后才能起身
	if (!bStunned)
	{
		if (Data.NewValue <= 0.f)
		{
			SetStunned(true);
		}
	}
	else
	{
		const float RecoverHealth = MaxHealth * FMath::Clamp(StunRecoverHealthPercent, 1.f, 100.f) * 0.01f;
		const auto* PassengerCharacter = Cast<ADeliveryCharacter>(GetAvatarActor());
        auto* Passenger = PassengerCharacter ? PassengerCharacter->TrunkPassenger.Get() : nullptr;
        if (Passenger && Passenger->IsPassenger())
        {
            if (Data.NewValue >= MaxHealth * .4f) Passenger->RequestExit(true);
        }
        else if (Data.NewValue >= RecoverHealth)
		{
			SetStunned(false);
		}
	}

	// 回血效果已经存在时不再添加，避免重复叠加
	if (!HealthRegenEffect || RegenHandle.IsValid())
	{
		return;
	}

	if (Data.NewValue < MaxHealth /* 如果新血量仍低于满血 */)
	{
		// 按配置创建回血效果
		const FGameplayEffectSpecHandle Spec = MakeOutgoingSpec(HealthRegenEffect, 1.f, MakeEffectContext());

		// 将回血效果应用到自身，并记录效果句柄
		RegenHandle = ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
	}
}

/// @brief 服务器：进入或退出晕倒；晕倒时挂 State.Stunned、打断技能、松手、丢下手持物、身体瘫软，起身时让拖着自己的玩家松手、身体站起
void UDeliverAbilitySystemComponent::SetStunned(bool bNewStunned)
{
	if (bStunned == bNewStunned || !GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	bStunned = bNewStunned;

	// 服务器用眩晕标签阻止技能，客户端在技能检查中读取同步的身体模式
	SetLooseGameplayTagCount(TAG_State_Stunned, bNewStunned ? 1 : 0);

	if (bNewStunned)
	{
		// 晕倒时中断当前技能
		CancelAbilities();
	}

	if (!AbilityActorInfo.IsValid())
	{
		return;
	}
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(AbilityActorInfo->AvatarActor.Get()))
	{
		if (bNewStunned)
		{
			if (UDeliveryGrabComponent* Grab = Character->GetGrabComponent())
				Grab->ForceRelease();
			if (UDeliveryInventoryComponent* Inventory = Character->GetInventoryComponent())
				Inventory->DropHeldItem();
		}
		else if (UDeliveryGrabbableComponent* Grabbable = Character->GetGrabbableComponent())
		{
			// 苏醒后解除其他玩家的拖拽，让身体重新站立
			Grabbable->ReleaseAllGrabbers();
		}
		if (UDeliveryActiveRagdollComponent* Ragdoll = Character->GetActiveRagdoll())
		{
			// 晕倒时关闭身体主动支撑，苏醒时开始恢复站立
			Ragdoll->SetLimp(bNewStunned);
		}
	}
}

/// @brief 服务器：玩家从后备箱出来后调用；血量够了才调用 SetStunned(false) 起身，还在箱里时不起身
/// @param bWakeAtFortyPercent true 时血量到 40% 就起身，false 时按 StunRecoverHealthPercent
void UDeliverAbilitySystemComponent::CompleteTrunkRecovery(bool bWakeAtFortyPercent)
{
    if (!GetOwner() || !GetOwner()->HasAuthority()) return;
    const auto* Character = Cast<ADeliveryCharacter>(GetAvatarActor());
    if (Character && Character->TrunkPassenger && Character->TrunkPassenger->IsPassenger()) return;
    const float Threshold = bWakeAtFortyPercent ? .4f : StunRecoverHealthPercent * .01f;
    if (bStunned && GetNumericAttribute(UDeliverAttributeSet::GetHealthAttribute()) >=
        GetNumericAttribute(UDeliverAttributeSet::GetMaxHealthAttribute()) * Threshold) SetStunned(false);
}
