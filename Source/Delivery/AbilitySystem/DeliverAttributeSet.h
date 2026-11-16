

#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "DeliverAttributeSet.generated.h"

#define DELIVER_ATTRIBUTE_ACCESSORS(ClassName, PropertyName)                                                 \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName)                                               \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName)                                                             \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName)                                                             \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

/**
 * @brief 玩家的属性：血量 Health、血量上限 MaxHealth、回血速度 HealthRegenRate，都同步给所有客户端
 *
 * 出拳由 GA_DeliverPunch 应用伤害效果扣 Health；Health 变化时技能组件判断晕倒和起身
 * 玩家躺在后备箱里受保护时，PreGameplayEffectExecute 拒绝扣血
 */
UCLASS()

class DELIVERY_API UDeliverAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

  public:
	/// @brief 初始血量 100、上限 100、回血 6 每秒
	UDeliverAttributeSet();

	/// @brief 登记三项属性的同步
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/// @brief 效果改属性之前调用；在后备箱里受保护时拒绝扣血，回血照常
	virtual bool PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data) override;

	/// @brief 属性改成新值之前调用，把 Health 限制在 0 到 MaxHealth 之间
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;

	// ── 属性 ──

	/// 当前血量
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Health, Category = "Vital")
	FGameplayAttributeData Health; // HP
	DELIVER_ATTRIBUTE_ACCESSORS(UDeliverAttributeSet, Health)

	/// 血量上限
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxHealth, Category = "Vital")
	FGameplayAttributeData MaxHealth; // 最大生命值
	DELIVER_ATTRIBUTE_ACCESSORS(UDeliverAttributeSet, MaxHealth)

	/// 晕倒期间每秒回多少血，回血效果读它
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_HealthRegenRate, Category = "Vital")
	FGameplayAttributeData HealthRegenRate; // （晕倒期间） 生命回复
	DELIVER_ATTRIBUTE_ACCESSORS(UDeliverAttributeSet, HealthRegenRate)

  protected:
	// ── 同步通知 ──
	// 客户端收到属性同步时调用，交给 GAS 广播属性变化
	UFUNCTION()
	void OnRep_Health(const FGameplayAttributeData& OldHealth);

	UFUNCTION()
	void OnRep_MaxHealth(const FGameplayAttributeData& OldMaxHealth);

	UFUNCTION()
	void OnRep_HealthRegenRate(const FGameplayAttributeData& OldHealthRegenRate);
};
