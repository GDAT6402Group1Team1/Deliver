

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
#include "Vehicle/DeliveryTrunkPassengerComponent.h"

UDeliverAbilitySystemComponent::UDeliverAbilitySystemComponent()
{
	// ASC 跟随 PlayerState 复制
	SetIsReplicatedByDefault(true);

	// Mixed：数据由服务器权威计算并下发；本机玩家收到完整 GE 和 Attribute，其他客户端只收到 Tag 和 Cue
	SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
}

// 连接技能拥有者与角色，并在服务器授予初始拳击技能
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

// 根据生命值切换晕倒状态，并在需要时启动回血
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

// 统一处理晕倒和苏醒，包括技能中断、手持物和身体控制
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

void UDeliverAbilitySystemComponent::CompleteTrunkRecovery(bool bWakeAtFortyPercent)
{
    if (!GetOwner() || !GetOwner()->HasAuthority()) return;
    const auto* Character = Cast<ADeliveryCharacter>(GetAvatarActor());
    if (Character && Character->TrunkPassenger && Character->TrunkPassenger->IsPassenger()) return;
    const float Threshold = bWakeAtFortyPercent ? .4f : StunRecoverHealthPercent * .01f;
    if (bStunned && GetNumericAttribute(UDeliverAttributeSet::GetHealthAttribute()) >=
        GetNumericAttribute(UDeliverAttributeSet::GetMaxHealthAttribute()) * Threshold) SetStunned(false);
}
