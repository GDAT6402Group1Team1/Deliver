// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "Combat/Types/DeliveryCombatTypes.h"
#include "GA_DeliverPunch.generated.h"

class ADeliveryCharacter;
class UDeliveryRagdollCombatComponent;

/**
 * @brief 玩家出拳的技能，左右拳各一个子类；在本机预测运行，伤害只在服务器结算
 *
 * 输入组件按 Ability.Attack.Punch.Left/Right 标签激活；ActivateAbility 调用角色的 StartMeleeAttack 开始挥拳，
 * 订阅布娃娃战斗组件的 OnPunchHitWindow 和 OnPunchEnded
 * 拳头接触时间段里每帧调用 OnHitWindow：服务器扫出打到谁，对玩家应用伤害效果并推胸口，对路人和 NPC 调用 ReceiveMeleeHitFrom
 * 收拳时 OnPunchEnded 结束技能
 */
UCLASS()

class DELIVERY_API UGA_DeliverPunch : public UGameplayAbility
{
	GENERATED_BODY()

  public:
	/// @brief 每个角色一个实例、本机预测运行；晕倒时不能激活
	UGA_DeliverPunch();

	// ── 技能流程 ──

	/// @brief 身体不在 Active 模式（晕倒、骑车等）、正在收拳或冷却没过时返回 false
	virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
	                                const FGameplayAbilityActorInfo* ActorInfo,
	                                const FGameplayTagContainer* SourceTags,
	                                const FGameplayTagContainer* TargetTags,
	                                FGameplayTagContainer* OptionalRelevantTags) const override;

	/// @brief 提交技能、记下出拳时间，调用角色的 StartMeleeAttack 挥拳，再订阅命中和收拳通知
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	                             const FGameplayAbilityActorInfo* ActorInfo,
	                             const FGameplayAbilityActivationInfo ActivationInfo,
	                             const FGameplayEventData* TriggerEventData) override;

	/// @brief 取消订阅战斗组件；被取消时调用 CancelPunch 停下这一拳
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
	                        const FGameplayAbilityActorInfo* ActorInfo,
	                        const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility,
	                        bool bWasCancelled) override;

  protected:
	// ── 配置 ──

	/// 这个技能挥哪只手，子类构造时设置
	EMeleeHand Hand = EMeleeHand::Left;

	/**
	 * 每拳伤害；配合 0.75 秒攻击间隔和 6/秒回血，单人连打第 5 拳把 HP 打到 0：
	 * 76 → 56.5 → 37 → 17.5 → 0；改这个数会直接改变几拳晕倒
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Punch")
	float PunchDamage = 24.f;

	/** 命中后由服务器额外施加给受击者胸部的冲量；不推髋和腿 */
	UPROPERTY(EditDefaultsOnly, Category = "Punch|Hit Reaction", meta = (ClampMin = "0.0"))
	float HitReactionImpulse = 1500.f;

	/// 推人的方向往上抬多少（0 到 1），加在出拳方向上
	UPROPERTY(EditDefaultsOnly, Category = "Punch|Hit Reaction", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float HitReactionUpwardFraction = 0.08f;

	/**
	 * 打路人时冲量的额外倍率
	 * 路人的刚体质量和玩家布娃娃不是一个量级（玩家那套做过髋部质量修正），
	 * 共用一个数值的话要么推不动、要么一拳飞上天，所以单独给一个
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Punch|Hit Reaction", meta = (ClampMin = "0.0"))
	float PedestrianImpulseScale = 1.6f;

	// ── 这一拳的状态 ──

	/// 这一拳已经打到的角色；拳头可能连续几帧碰着同一个人，只算一次
	TSet<TWeakObjectPtr<AActor>> HitActors;

	/// 这一拳已经播过命中音效
	bool bPlayedHitSound = false;

	/// 这一拳的编号，ActivateAbility 时生成，报告命中时带上
	FGuid PunchId;

	// ── 战斗组件回调 ──

	/// @brief 拳头接触时间段里每帧调用：服务器扫出打到的角色，每人只结算一次
	UFUNCTION()
	void OnHitWindow(EMeleeHand FiredHand);

	/// @brief 收拳时调用：调用角色的 EndMeleeAttack 并结束技能；左右手分开，只认自己那只手
	UFUNCTION()
	void OnPunchEnded(EMeleeHand FiredHand);

	/// 角色的布娃娃战斗组件，订阅它的命中和收拳通知
	UPROPERTY()
	TObjectPtr<UDeliveryRagdollCombatComponent> RagdollCombat;
};

/// @brief 左拳技能，带标签 Ability.Attack.Punch.Left
UCLASS()

class DELIVERY_API UGA_DeliverPunchLeft : public UGA_DeliverPunch
{
	GENERATED_BODY()

  public:
	UGA_DeliverPunchLeft();
};

/// @brief 右拳技能，带标签 Ability.Attack.Punch.Right
UCLASS()

class DELIVERY_API UGA_DeliverPunchRight : public UGA_DeliverPunch
{
	GENERATED_BODY()

  public:
	UGA_DeliverPunchRight();
};
