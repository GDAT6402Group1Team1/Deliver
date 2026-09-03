// Fill out your copyright notice in the Description page of Project Settings.

#include "DeliverAttributeSet.h"
#include "Net/UnrealNetwork.h"

UDeliverAttributeSet::UDeliverAttributeSet()
{
	InitHealth(100.f);
	InitMaxHealth(100.f);
	InitHealthRegenRate(6.f);
}

void UDeliverAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION_NOTIFY(UDeliverAttributeSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDeliverAttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDeliverAttributeSet, HealthRegenRate, COND_None, REPNOTIFY_Always);
}

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
