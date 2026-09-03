// Fill out your copyright notice in the Description page of Project Settings.

#include "DeliverAbilitySystemComponent.h"

UDeliverAbilitySystemComponent::UDeliverAbilitySystemComponent()
{
	// ASC 跟随 PlayerState 复制
	SetIsReplicatedByDefault(true);

	// Mixed：数据由服务器权威计算并下发；本机玩家收到完整 GE 和 Attribute，其他客户端只收到 Tag 和 Cue。
	SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
}

void UDeliverAbilitySystemComponent::InitializeAbilityActor(AActor* Owner, AActor* Avatar)
{
	InitAbilityActorInfo(Owner, Avatar);
}
