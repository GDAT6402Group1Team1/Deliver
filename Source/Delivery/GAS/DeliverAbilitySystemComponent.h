// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffectTypes.h"
#include "DeliverAbilitySystemComponent.generated.h"

class UGameplayEffect;
class UGameplayAbility;

/**
 * 玩家专用 ASC。负责 GAS 复制策略，以及 Owner / Avatar 绑定。
 * PlayerState 只持有本组件并在 Possess 时通知它。
 */
UCLASS()
class DELIVERY_API UDeliverAbilitySystemComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()

public:

	UDeliverAbilitySystemComponent();

	// InitActorInfo封装
	void InitializeAbilityActor(AActor* Owner, AActor* Avatar);

	/** 左右拳共用冷却，由 GA 读写。 */
	float LastMeleeAttackTime = -1000.f;

	/** 要比整段出拳（收拳到收回）稍长，否则下一拳会在上一拳还没收完时被拒掉。 */
	static constexpr float MeleeAttackCooldown = 0.75f;

protected:

	TSubclassOf<UGameplayEffect> HealthRegenEffect;

	TSubclassOf<UGameplayAbility> PunchLeftAbilityClass;
	TSubclassOf<UGameplayAbility> PunchRightAbilityClass;

	FActiveGameplayEffectHandle RegenHandle;
	bool bRegenWatchBound = false;
	bool bAbilitiesGranted = false;

	// 血量变化回调（服务器上）
	void OnHealthChanged(const FOnAttributeChangeData& Data);
};
