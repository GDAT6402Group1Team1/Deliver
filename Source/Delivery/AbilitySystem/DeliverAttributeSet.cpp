

#include "AbilitySystem/DeliverAttributeSet.h"
#include "Net/UnrealNetwork.h"
#include "GameplayEffectExtension.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/Trunk/DeliveryTrunkPassengerComponent.h"

// ── 生命周期 ──

/// @brief 初始血量 100、上限 100、回血 6 每秒
UDeliverAttributeSet::UDeliverAttributeSet()
{
	InitHealth(100.f);
	InitMaxHealth(100.f);
	InitHealthRegenRate(6.f);
}

/// @brief 登记需要同步给客户端的生命值属性
void UDeliverAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION_NOTIFY(UDeliverAttributeSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDeliverAttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDeliverAttributeSet, HealthRegenRate, COND_None, REPNOTIFY_Always);
}

/// @brief 属性改成新值之前调用，把 Health 限制在 0 到 MaxHealth 之间
void UDeliverAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	// 钳制生命值在最大生命值内
	if (Attribute == GetHealthAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxHealth());
	}
}

// ── 同步通知 ──

// ── 同步通知 ──
// 客户端收到属性同步时调用，GAMEPLAYATTRIBUTE_REPNOTIFY 让 GAS 广播属性变化

/// @brief 客户端收到 Health 同步
void UDeliverAttributeSet::OnRep_Health(const FGameplayAttributeData& OldHealth)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDeliverAttributeSet, Health, OldHealth);
}

/// @brief 客户端收到 MaxHealth 同步
void UDeliverAttributeSet::OnRep_MaxHealth(const FGameplayAttributeData& OldMaxHealth)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDeliverAttributeSet, MaxHealth, OldMaxHealth);
}

/// @brief 客户端收到 HealthRegenRate 同步
void UDeliverAttributeSet::OnRep_HealthRegenRate(const FGameplayAttributeData& OldHealthRegenRate)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDeliverAttributeSet, HealthRegenRate, OldHealthRegenRate);
}

// ── 生命周期 ──
bool UDeliverAttributeSet::PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data)
{
    const auto* Character = Cast<ADeliveryCharacter>(Data.Target.GetAvatarActor());
    if (Data.EvaluatedData.Attribute == GetHealthAttribute() && Data.EvaluatedData.Magnitude < 0.f &&
        Character && Character->TrunkPassenger && Character->TrunkPassenger->IsDamageProtected()) return false;
    return Super::PreGameplayEffectExecute(Data);
}
