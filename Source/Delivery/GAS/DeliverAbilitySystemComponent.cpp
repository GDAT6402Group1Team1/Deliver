// Fill out your copyright notice in the Description page of Project Settings.

#include "DeliverAbilitySystemComponent.h"
#include "DeliverAttributeSet.h"
#include "../DeliveryCharacter.h"
#include "Abilities/GameplayAbility.h"
#include "Abilities/GA_DeliverPunch.h"
#include "GameplayEffect.h"

UDeliverAbilitySystemComponent::UDeliverAbilitySystemComponent()
{
	// ASC 跟随 PlayerState 复制
	SetIsReplicatedByDefault(true);

	// Mixed：数据由服务器权威计算并下发；本机玩家收到完整 GE 和 Attribute，其他客户端只收到 Tag 和 Cue。
	SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
}

void UDeliverAbilitySystemComponent::InitializeAbilityActor(AActor* Owner, AActor* Avatar)
{
	/** InitAbilityActorInfo **/
	InitAbilityActorInfo(Owner, Avatar);
	
	/** InitAbilityActorInfo之后 **/
	// 获取回血 GE
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Avatar))
	{
		HealthRegenEffect = Character->GetHealthRegenEffect();
		PunchLeftAbilityClass = Character->GetPunchLeftAbilityClass();
		PunchRightAbilityClass = Character->GetPunchRightAbilityClass();
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

void UDeliverAbilitySystemComponent::OnHealthChanged(const FOnAttributeChangeData& Data)
{
	if (!HealthRegenEffect || RegenHandle.IsValid())
	{
		return;
	}

	const float MaxHealth = GetNumericAttribute(UDeliverAttributeSet::GetMaxHealthAttribute());
	if (Data.NewValue < MaxHealth /*如果新血量仍低于满血*/)
	{
		// 创建 回复GE 上下文
		const FGameplayEffectSpecHandle Spec = MakeOutgoingSpec(HealthRegenEffect, 1.f, MakeEffectContext());
		
		// 应用 回复GE
		RegenHandle = ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
	}
}
