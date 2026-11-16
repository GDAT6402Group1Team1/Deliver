

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffectTypes.h"
#include "DeliverAbilitySystemComponent.generated.h"

class UGameplayEffect;
class UGameplayAbility;

/**
 * @brief 玩家的技能组件（ASC），挂在 PlayerState 上随它复制；角色死亡、上下车之后血量和技能都还在
 *
 * 角色 Possess 后调用 InitializeAbilityActor：把 PlayerState 设为拥有者、角色设为执行者，服务器授予左右拳技能，订阅血量变化
 * 血量归零时 OnHealthChanged 调用 SetStunned(true) 晕倒：挂上 State.Stunned、打断技能、松手、丢下手持物、身体瘫软
 * 晕倒期间按回血效果涨血，涨到 StunRecoverHealthPercent 后调用 SetStunned(false) 起身
 */
UCLASS()

class DELIVERY_API UDeliverAbilitySystemComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()

  public:
	// ── 生命周期 ──

	/// @brief 同步给所有客户端，复制方式为 Mixed：本机玩家收到完整的效果和属性，其他客户端只收到标签和特效
	UDeliverAbilitySystemComponent();

	/// @brief 设置技能的拥有者（PlayerState）和执行者（角色），从角色读取回血效果、左右拳技能和起身比例
	/// 服务器上第一次调用时授予左右拳技能，并订阅血量变化调用 OnHealthChanged
	void InitializeAbilityActor(AActor* Owner, AActor* Avatar);

	// ── 晕倒 ──

	/// @brief 服务器：玩家从后备箱出来后调用；血量够了才调用 SetStunned(false) 起身，还在箱里时不起身
	/// @param bWakeAtFortyPercent true 时血量到 40% 就起身，false 时按 StunRecoverHealthPercent
	void CompleteTrunkRecovery(bool bWakeAtFortyPercent);

	// ── 出拳冷却 ──

	/// 上一次出拳的世界时间，GA_DeliverPunch 的 ActivateAbility 写入、CanActivateAbility 读取，左右拳共用
	float LastMeleeAttackTime = -1000.f;

	/// 两拳之间至少隔多少秒；要比整段出拳（出拳到收回）稍长，否则下一拳会在上一拳还没收完时被拒掉
	static constexpr float MeleeAttackCooldown = 0.75f;

	/// @brief 是否晕倒；只在服务器上准确，客户端读复制过来的 State.Stunned 标签
	UFUNCTION(BlueprintPure, Category = "Ability|Stun")

	bool IsStunned() const
	{
		return bStunned;
	}

  protected:
	// ── 配置 ──
	// InitializeAbilityActor 从角色 ADeliveryCharacter 读进来

	/// 回血效果，血量低于上限时 OnHealthChanged 应用一次
	TSubclassOf<UGameplayEffect> HealthRegenEffect;

	/// 左拳技能
	TSubclassOf<UGameplayAbility> PunchLeftAbilityClass;

	/// 右拳技能
	TSubclassOf<UGameplayAbility> PunchRightAbilityClass;

	/// 晕倒后血量回到上限的百分之多少才起身
	float StunRecoverHealthPercent = 30.f;

	// ── 服务器状态 ──

	/// 是否晕倒
	bool bStunned = false;

	/// 已经应用的回血效果；有效时不再重复应用
	FActiveGameplayEffectHandle RegenHandle;

	/// 是否已经订阅血量变化
	bool bRegenWatchBound = false;

	/// 是否已经授予左右拳技能
	bool bAbilitiesGranted = false;

	// ── 晕倒 ──

	/// @brief 服务器：血量变化时调用；没晕倒时血量到 0 就晕倒，晕倒时血量回到起身线就起身（在后备箱里时到 40% 请求出箱）
	/// 血量低于上限、而且还没有回血效果时应用 HealthRegenEffect
	void OnHealthChanged(const FOnAttributeChangeData& Data);

	/// @brief 服务器：进入或退出晕倒
	/// 晕倒时挂上 State.Stunned、打断技能、松开抓着的东西、丢下手持物、身体瘫软；起身时摘掉标签、让拖着自己的玩家松手、身体开始站起
	void SetStunned(bool bNewStunned);
};
