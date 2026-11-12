

#include "AbilitySystem/DeliverAttributeSet.h"
#include "Net/UnrealNetwork.h"
#include "GameplayEffectExtension.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/DeliveryTrunkPassengerComponent.h"

UDeliverAttributeSet::UDeliverAttributeSet()
{
	InitHealth(100.f);
	InitMaxHealth(100.f);
	InitHealthRegenRate(6.f);
}

// 登记需要同步给客户端的生命值属性
void UDeliverAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION_NOTIFY(UDeliverAttributeSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDeliverAttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDeliverAttributeSet, HealthRegenRate, COND_None, REPNOTIFY_Always);
}

// 限制生命值和最大生命值，避免出现无效范围
void UDeliverAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	// 钳制生命值在最大生命值内
	if (Attribute == GetHealthAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxHealth());
	}
}

void UDeliverAttributeSet::OnRep_Health(const FGameplayAttributeData& OldHealth)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDeliverAttributeSet, Health, OldHealth);
}

void UDeliverAttributeSet::OnRep_MaxHealth(const FGameplayAttributeData& OldMaxHealth)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDeliverAttributeSet, MaxHealth, OldMaxHealth);
}

void UDeliverAttributeSet::OnRep_HealthRegenRate(const FGameplayAttributeData& OldHealthRegenRate)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDeliverAttributeSet, HealthRegenRate, OldHealthRegenRate);
}

// 只屏蔽伤害，回血仍通过同一套 GE；复制的当前血量供本机恢复提示使用
bool UDeliverAttributeSet::PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data)
{
    const auto* Character = Cast<ADeliveryCharacter>(Data.Target.GetAvatarActor());
    if (Data.EvaluatedData.Attribute == GetHealthAttribute() && Data.EvaluatedData.Magnitude < 0.f &&
        Character && Character->TrunkPassenger && Character->TrunkPassenger->IsDamageProtected()) return false;
    return Super::PreGameplayEffectExecute(Data);
}
