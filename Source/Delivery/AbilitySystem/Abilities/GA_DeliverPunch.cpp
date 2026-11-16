// 连接出拳动作和伤害结算，本技能不直接计算手臂姿态
// ActivateAbility 发起动作并订阅战斗组件，OnHitWindow 查询目标并施加效果
// OnPunchEnded 结束技能，EndAbility 解绑通知；被打断时同时取消出拳
// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/Abilities/GA_DeliverPunch.h"

#include "AbilitySystemComponent.h"
#include "Combat/Types/DeliveryImpactReceiver.h"
#include "Combat/Types/DeliveryCombatEvents.h"
#include "Combat/DeliveryRagdollCombatComponent.h"
#include "Character/DeliveryCharacter.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "Player/DeliverPlayerState.h"
#include "AbilitySystem/DeliverAbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"

UGA_DeliverPunch::UGA_DeliverPunch()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	ActivationBlockedTags.AddTag(TAG_State_Stunned);
}

// 检查技能条件和身体状态，阻止晕倒或无法控制身体时出拳
bool UGA_DeliverPunch::CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
                                          const FGameplayAbilityActorInfo* ActorInfo,
                                          const FGameplayTagContainer* SourceTags,
                                          const FGameplayTagContainer* TargetTags,
                                          FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!ActorInfo || !ActorInfo->AvatarActor.IsValid()) return false;
	// 晕倒判定：服务器靠 State.Stunned（ActivationBlockedTags）拦，
	// 但那个 Tag 不复制，客户端预测时看不到；这里改查布娃娃的控制模式——
	// ReplicatedControlMode 是复制的，两端都能得到一致结论
	const ADeliveryCharacter* StunCheckCharacter = Cast<ADeliveryCharacter>(ActorInfo->AvatarActor.Get());
	const UDeliveryActiveRagdollComponent* StunCheckRagdoll =
	    StunCheckCharacter ? StunCheckCharacter->GetActiveRagdoll() : nullptr;
	// 收拳期间先拒绝攻击，再提交技能消耗
	// 避免尚不能出拳时，点击攻击仍消耗冷却时间
	if (StunCheckRagdoll && (StunCheckRagdoll->GetControlMode() != EDeliveryRagdollControlMode::Active ||
	                         StunCheckRagdoll->IsPunchRecovering()))
	{
		return false;
	}

	const UDeliverAbilitySystemComponent* ASC =
	    Cast<UDeliverAbilitySystemComponent>(ActorInfo->AbilitySystemComponent.Get());
	const UWorld* World = ActorInfo->AvatarActor->GetWorld();
	if (ASC && World &&
	    World->GetTimeSeconds() - ASC->LastMeleeAttackTime <
	        UDeliverAbilitySystemComponent::MeleeAttackCooldown)
	{
		return false;
	}

	return Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags);
}

// 启动技能：通过提交检查后请求角色开始出拳，再订阅命中窗口和结束通知
void UGA_DeliverPunch::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
                                       const FGameplayAbilityActorInfo* ActorInfo,
                                       const FGameplayAbilityActivationInfo ActivationInfo,
                                       const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	if (UDeliverAbilitySystemComponent* ASC =
	        Cast<UDeliverAbilitySystemComponent>(ActorInfo->AbilitySystemComponent.Get()))
	{
		ASC->LastMeleeAttackTime = ActorInfo->AvatarActor->GetWorld()->GetTimeSeconds();
	}

	ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(ActorInfo->AvatarActor.Get());
	if (!Character || !Character->StartMeleeAttack(Hand))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 一次技能实例可以重复出拳，每次成功启动都清空上一拳的命中记录
	HitActors.Reset();
	PunchId=FGuid::NewGuid();
	bPlayedHitSound = false;
	RagdollCombat = Character->GetRagdollCombat();
	RagdollCombat->OnPunchHitWindow.AddDynamic(this, &UGA_DeliverPunch::OnHitWindow);
	RagdollCombat->OnPunchEnded.AddDynamic(this, &UGA_DeliverPunch::OnPunchEnded);
}

// 结束本次攻击
void UGA_DeliverPunch::EndAbility(const FGameplayAbilitySpecHandle Handle,
                                  const FGameplayAbilityActorInfo* ActorInfo,
                                  const FGameplayAbilityActivationInfo ActivationInfo,
                                  bool bReplicateEndAbility, bool bWasCancelled)
{
	PunchId.Invalidate();
	if (RagdollCombat)
	{
		UDeliveryRagdollCombatComponent* Combat = RagdollCombat;
		RagdollCombat = nullptr;
		// 技能结束后，不再处理这次出拳的命中和结束通知
		Combat->OnPunchHitWindow.RemoveDynamic(this, &UGA_DeliverPunch::OnHitWindow);
		Combat->OnPunchEnded.RemoveDynamic(this, &UGA_DeliverPunch::OnPunchEnded);
		// 被打断时，同时停止尚未完成的出拳
		if (bWasCancelled)
			Combat->CancelPunch();
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

// 在服务器结算当前出拳手的命中目标和伤害
void UGA_DeliverPunch::OnHitWindow(EMeleeHand FiredHand)
{
	if (!IsActive() || !CurrentActorInfo || FiredHand != Hand || !CurrentActorInfo->IsNetAuthority() || !PunchId.IsValid())
	{
		return;
	}

	ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(CurrentActorInfo->AvatarActor.Get());
	if (!Character || !RagdollCombat || !Character->GetDamageEffect())
	{
		return;
	}
	// 战斗组件已记录本帧拳路，这里经角色入口调用 GatherHits 做扫掠查询
	const TArray<AActor*> Hits = Character->GatherMeleeHits(Hand);
	const TSubclassOf<UGameplayEffect> DamageClass = Character->GetDamageEffect();
	const FVector PunchDirection = RagdollCombat->GetPunchAimDirection();
	const FVector ImpactPoint = RagdollCombat->GetPunchTraceTransform().GetLocation();
	const FGuid ResolvingPunchId = PunchId;
	const TWeakObjectPtr<UDeliveryRagdollCombatComponent> ResolvingCombat = RagdollCombat;

	// 同一次出拳仅播放一次命中音效
	bool bPlayHitSound = false;
	bool bHitNPC = false;
	const auto RecordHit = [&](AActor* HitActor)
	{
		// 受击和 GAS 事件会同步取消动作，进入回调前先记下本拳的去重与音效状态
		HitActors.Add(HitActor);
		if (!bPlayedHitSound) { bPlayedHitSound = true; bPlayHitSound = true; }
	};

	for (AActor* HitActor : Hits)
	{
		if (!IsActive() || PunchId != ResolvingPunchId || !IsValid(Character) ||
		    Character->IsActorBeingDestroyed() || RagdollCombat != ResolvingCombat.Get()) break;
		// 窗口会连续触发，多帧接触同一角色也只处理一拳
		if (!IsValid(HitActor) || HitActor->IsActorBeingDestroyed() || HitActors.Contains(HitActor))
			continue;
		// 路人这类不走 GAS 的目标：没有血量也没有晕倒，只有被打飞和退场。
		// 判据是"实现了受击接口"而不是"Cast 成 ADeliveryPedestrian"——
		// 后者会让战斗代码反向依赖 NPC 模块，而且同一个判断在撞车那边还要再写一遍
		if (IDeliveryImpactReceiver* Receiver = Cast<IDeliveryImpactReceiver>(HitActor))
		{
			if (Receiver->IsKnockedDown() && !Receiver->CanReceiveHitWhileDown())
			{
				continue;
			}
			RecordHit(HitActor);
			bHitNPC = true;
			if (HitReactionImpulse > 0.0f && !PunchDirection.IsNearlyZero())
			{
				const FVector ReactionDirection =
				    (PunchDirection + FVector::UpVector * HitReactionUpwardFraction).GetSafeNormal();
				// 冲量乘一个倍率：路人的刚体质量和玩家布娃娃不是一个量级，
				// 用同一个数值的话要么推不动要么飞上天，所以单独给
				Receiver->ReceiveMeleeHitFrom(ReactionDirection * HitReactionImpulse * PedestrianImpulseScale,
				                        ImpactPoint, Character, ResolvingPunchId);
			}
			DeliveryCombatEvents::ReportHit(Character,HitActor,ResolvingPunchId,ImpactPoint);
			continue;
		}

		ADeliveryCharacter* TargetCharacter = Cast<ADeliveryCharacter>(HitActor);
		const ADeliverPlayerState* TargetPS =
		    TargetCharacter ? Cast<ADeliverPlayerState>(TargetCharacter->GetTaskPlayerState()) : nullptr;
		UAbilitySystemComponent* TargetASC = TargetPS ? TargetPS->GetAbilitySystemComponent() : nullptr;
		UDeliveryActiveRagdollComponent* TargetRagdoll =
		    TargetCharacter ? TargetCharacter->GetActiveRagdoll() : nullptr;
		if (!TargetASC || !TargetRagdoll || !TargetRagdoll->IsRagdollActive())
		{
			continue;
		}

		// 晕倒期间暂停普通扣血，保留生命恢复至起身阈值的条件
		// 物理冲量仍然生效，使倒地身体保留受击反应
		const bool bTargetStunned = TargetRagdoll->GetControlMode() == EDeliveryRagdollControlMode::Limp;
		if (!bTargetStunned)
		{
			FGameplayEffectSpecHandle Spec = MakeOutgoingGameplayEffectSpec(DamageClass);
			if (!Spec.Data.IsValid())
			{
				continue;
			}
			Spec.Data->SetSetByCallerMagnitude(TAG_Effect_Type_Damage, -PunchDamage);
			RecordHit(HitActor);
			TargetASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
		}
		else RecordHit(HitActor);

		// 击打者只提供方向和力度；真正的物理冲量由服务器施加到受击者自身
		if (IsValid(TargetCharacter) && !TargetCharacter->IsActorBeingDestroyed() && IsValid(TargetRagdoll) &&
		    HitReactionImpulse > 0.0f && !PunchDirection.IsNearlyZero())
		{
			const FVector ReactionDirection =
			    (PunchDirection + FVector::UpVector * HitReactionUpwardFraction).GetSafeNormal();
			TargetRagdoll->ApplyMeleeImpact(ReactionDirection * HitReactionImpulse, ImpactPoint);
		}

		DeliveryCombatEvents::ReportHit(Character,HitActor,ResolvingPunchId,ImpactPoint);
	}

	if (bPlayHitSound && ResolvingCombat.IsValid())
	{
		ResolvingCombat->MulticastPlayPunchHit(ImpactPoint, bHitNPC);
	}
}

// 只结束当前技能对应的那只手，避免左右拳事件互相影响
void UGA_DeliverPunch::OnPunchEnded(EMeleeHand FiredHand)
{
	if (!IsActive() || !CurrentActorInfo || FiredHand != Hand)
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
