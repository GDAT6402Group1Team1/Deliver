// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "DeliverAbilitySystemComponent.generated.h"

/**
 * 玩家专用 ASC。负责 GAS 复制策略，以及 Owner / Avatar 绑定。
 * PlayerState 只持有本组件并在 Possess 时通知它。
 */
UCLASS()
class DELIVERY_API UDeliverAbilitySystemComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()

public:

	UDeliverAbilitySystemComponent();

	// InitActorInfo封装
	void InitializeAbilityActor(AActor* Owner, AActor* Avatar);
};
