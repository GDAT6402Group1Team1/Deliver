// Copyright Epic Games, Inc. All Rights Reserved.

#include "GA_DeliverPunch.h"

#include "AbilitySystemComponent.h"
#include "Combat/DeliveryRagdollCombatComponent.h"
#include "DeliveryCharacter.h"
#include "GAS/DeliverGameplayTags.h"
#include "GAS/DeliverPlayerState.h"
#include "GAS/DeliverAbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"

UGA_DeliverPunch::UGA_DeliverPunch()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	ActivationBlockedTags.AddTag(TAG_State_Stunned);
}

bool UGA_DeliverPunch::CanActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayTagContainer* SourceTags,
	const FGameplayTagContainer* TargetTags,
	FGameplayTagContainer* OptionalRelevantTags) const
{
	const UDeliverAbilitySystemComponent* ASC = Cast<UDeliverAbilitySystemComponent>(ActorInfo->AbilitySystemComponent.Get());
	const UWorld* World = ActorInfo->AvatarActor->GetWorld();
	if (ASC && World && World->GetTimeSeconds() - ASC->LastMeleeAttackTime < UDeliverAbilitySystemComponent::MeleeAttackCooldown)
	{
		return false;
	}

	return Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags);
}

void UGA_DeliverPunch::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	if (UDeliverAbilitySystemComponent* ASC = Cast<UDeliverAbilitySystemComponent>(ActorInfo->AbilitySystemComponent.Get()))
	{
		ASC->LastMeleeAttackTime = ActorInfo->AvatarActor->GetWorld()->GetTimeSeconds();
	}

	ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(ActorInfo->AvatarActor.Get());
	if (!Character || !Character->StartMeleeAttack(Hand))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	RagdollCombat = Character->GetRagdollCombat();
	RagdollCombat->OnPunchHitWindow.AddDynamic(this, &UGA_DeliverPunch::OnHitWindow);
	RagdollCombat->OnPunchEnded.AddDynamic(this, &UGA_DeliverPunch::OnPunchEnded);
}

void UGA_DeliverPunch::EndAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility,
	bool bWasCancelled)
{
	if (RagdollCombat)
	{
		RagdollCombat->OnPunchHitWindow.RemoveDynamic(this, &UGA_DeliverPunch::OnHitWindow);
		RagdollCombat->OnPunchEnded.RemoveDynamic(this, &UGA_DeliverPunch::OnPunchEnded);
		if (bWasCancelled) RagdollCombat->CancelPunch();
		RagdollCombat = nullptr;
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

void UGA_DeliverPunch::OnHitWindow(EMeleeHand FiredHand)
{
	if (FiredHand != Hand || !CurrentActorInfo->IsNetAuthority())
	{
		return;
	}

	ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(CurrentActorInfo->AvatarActor.Get());
	if (!Character || !RagdollCombat || !Character->GetDamageEffect())
	{
		return;
	}
	const TArray<AActor*> Hits = Character->GatherMeleeHits(Hand);
	const TSubclassOf<UGameplayEffect> DamageClass = Character->GetDamageEffect();
	const FVector PunchDirection = RagdollCombat->GetPunchAimDirection();
	const FVector ImpactPoint = RagdollCombat->GetPunchTraceTransform().GetLocation();

	for (AActor* HitActor : Hits)
	{
		ADeliveryCharacter* TargetCharacter = Cast<ADeliveryCharacter>(HitActor);
		const ADeliverPlayerState* TargetPS = TargetCharacter
			? TargetCharacter->GetPlayerState<ADeliverPlayerState>() : nullptr;
		UAbilitySystemComponent* TargetASC = TargetPS ? TargetPS->GetAbilitySystemComponent() : nullptr;
		UDeliveryActiveRagdollComponent* TargetRagdoll = TargetCharacter
			? TargetCharacter->GetActiveRagdoll() : nullptr;
		if (!TargetASC || !TargetRagdoll || !TargetRagdoll->IsRagdollActive())
		{
			continue;
		}

		FGameplayEffectSpecHandle Spec = MakeOutgoingGameplayEffectSpec(DamageClass);
		if (!Spec.Data.IsValid())
		{
			continue;
		}
		Spec.Data->SetSetByCallerMagnitude(TAG_Effect_Type_Damage, -PunchDamage);
		TargetASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());

		// 击打者只提供方向和力度；真正的物理冲量由服务器施加到受击者自身。
		if (HitReactionImpulse > 0.0f && !PunchDirection.IsNearlyZero())
		{
			const FVector ReactionDirection = (PunchDirection
				+ FVector::UpVector * HitReactionUpwardFraction).GetSafeNormal();
			TargetRagdoll->ApplyMeleeImpact(ReactionDirection * HitReactionImpulse, ImpactPoint);
		}
	}
}

void UGA_DeliverPunch::OnPunchEnded(EMeleeHand FiredHand)
{
	if (FiredHand != Hand)
	{
		return;
	}

	if (ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(CurrentActorInfo->AvatarActor.Get()))
	{
		Character->EndMeleeAttack(Hand);
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

UGA_DeliverPunchLeft::UGA_DeliverPunchLeft()
{
	Hand = EMeleeHand::Left;
	SetAssetTags(FGameplayTagContainer(TAG_Ability_Attack_Punch_Left));
}

UGA_DeliverPunchRight::UGA_DeliverPunchRight()
{
	Hand = EMeleeHand::Right;
	SetAssetTags(FGameplayTagContainer(TAG_Ability_Attack_Punch_Right));
}
