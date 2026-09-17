// Fill out your copyright notice in the Description page of Project Settings.

#include "DeliverAbilitySystemComponent.h"
#include "DeliverAttributeSet.h"
#include "DeliverGameplayTags.h"
#include "../DeliveryCharacter.h"
#include "../Ragdoll/DeliveryActiveRagdollComponent.h"
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

void UDeliverAbilitySystemComponent::OnHealthChanged(const FOnAttributeChangeData& Data)
{
	const float MaxHealth = GetNumericAttribute(UDeliverAttributeSet::GetMaxHealthAttribute());

	// 晕倒判定放在回血 GE 之前：HP 归零就倒，回血过阈值才起身。
	// 没有受击保护时间，所以躺着继续挨打会把血打回阈值以下，人就一直起不来。
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
		if (Data.NewValue >= RecoverHealth)
		{
			SetStunned(false);
		}
	}

	if (!HealthRegenEffect || RegenHandle.IsValid())
	{
		return;
	}

	if (Data.NewValue < MaxHealth /*如果新血量仍低于满血*/)
	{
		// 创建 回复GE 上下文
		const FGameplayEffectSpecHandle Spec = MakeOutgoingSpec(HealthRegenEffect, 1.f, MakeEffectContext());
		
		// 应用 回复GE
		RegenHandle = ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
	}
}

void UDeliverAbilitySystemComponent::SetStunned(bool bNewStunned)
{
	if (bStunned == bNewStunned || !GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	bStunned = bNewStunned;

	// Loose tag 只存在于服务器，不复制。服务器是技能激活的权威方，
	// UGA_DeliverPunch 的 ActivationBlockedTags 里挂着 State.Stunned，这里置位即生效。
	// 客户端那侧由 GA 自己查布娃娃复制过来的控制模式，见 UGA_DeliverPunch::CanActivateAbility。
	SetLooseGameplayTagCount(TAG_State_Stunned, bNewStunned ? 1 : 0);

	if (bNewStunned)
	{
		// 已经在挥的那一拳要打断，否则人瘫了拳还在往前送。
		CancelAbilities();
	}

	if (!AbilityActorInfo.IsValid())
	{
		return;
	}
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(AbilityActorInfo->AvatarActor.Get()))
	{
		if (UDeliveryActiveRagdollComponent* Ragdoll = Character->GetActiveRagdoll())
		{
			// Limp 会关掉全部 Physics Control，人整个瘫下去；恢复时重新接管直立和步态。
			Ragdoll->SetLimp(bNewStunned);
		}
	}
}
