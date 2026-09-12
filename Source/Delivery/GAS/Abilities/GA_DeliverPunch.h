// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "Combat/DeliveryCombatTypes.h"
#include "GA_DeliverPunch.generated.h"

class ADeliveryCharacter;
class UDeliveryRagdollCombatComponent;

/** 物理挥拳 Ability 基类：调 CombatInterface 挥拳，命中窗口扣血。 */
UCLASS()
class DELIVERY_API UGA_DeliverPunch : public UGameplayAbility
{
	GENERATED_BODY()

public:

	UGA_DeliverPunch();

	virtual bool CanActivateAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayTagContainer* SourceTags,
		const FGameplayTagContainer* TargetTags,
		FGameplayTagContainer* OptionalRelevantTags) const override;

	virtual void ActivateAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility,
		bool bWasCancelled) override;

protected:

	EMeleeHand Hand = EMeleeHand::Left;

	UPROPERTY(EditDefaultsOnly, Category="Punch")
	float PunchDamage = 18.f;

	UFUNCTION()
	void OnHitWindow(EMeleeHand FiredHand);

	UFUNCTION()
	void OnPunchEnded(EMeleeHand FiredHand);

	UPROPERTY()
	TObjectPtr<UDeliveryRagdollCombatComponent> RagdollCombat;
};

UCLASS()
class DELIVERY_API UGA_DeliverPunchLeft : public UGA_DeliverPunch
{
	GENERATED_BODY()

public:

	UGA_DeliverPunchLeft();
};

UCLASS()
class DELIVERY_API UGA_DeliverPunchRight : public UGA_DeliverPunch
{
	GENERATED_BODY()

public:

	UGA_DeliverPunchRight();
};
