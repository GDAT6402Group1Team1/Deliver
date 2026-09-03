// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "DeliverAttributeSet.generated.h"

#define DELIVER_ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

UCLASS()
class DELIVERY_API UDeliverAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:

	UDeliverAttributeSet();

	// 登记需要复制的属性
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	
	// 当属性将要被更改成新值时调用
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;

	/** 主属性 **/
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_Health, Category="Vital")
	FGameplayAttributeData Health;									// HP
	DELIVER_ATTRIBUTE_ACCESSORS(UDeliverAttributeSet, Health)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_MaxHealth, Category="Vital")
	FGameplayAttributeData MaxHealth;								// MaxHP							
	DELIVER_ATTRIBUTE_ACCESSORS(UDeliverAttributeSet, MaxHealth)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_HealthRegenRate, Category="Vital")
	FGameplayAttributeData HealthRegenRate;							// （晕倒期间） 生命回复
	DELIVER_ATTRIBUTE_ACCESSORS(UDeliverAttributeSet, HealthRegenRate)

protected:

	UFUNCTION()
	void OnRep_Health(const FGameplayAttributeData& OldHealth);

	UFUNCTION()
	void OnRep_MaxHealth(const FGameplayAttributeData& OldMaxHealth);

	UFUNCTION()
	void OnRep_HealthRegenRate(const FGameplayAttributeData& OldHealthRegenRate);
};
