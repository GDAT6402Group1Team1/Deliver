// Fill out your copyright notice in the Description page of Project Settings.

#include "DeliverPlayerState.h"
#include "DeliverAbilitySystemComponent.h"
#include "DeliverAttributeSet.h"
#include "Task/DeliveryTaskTrackerComponent.h"

ADeliverPlayerState::ADeliverPlayerState()
{
	// 创建 ASC
	AbilitySystemComponent = CreateDefaultSubobject<UDeliverAbilitySystemComponent>(TEXT("AbilitySystemComponent"));

	// 创建 属性表
	AttributeSet = CreateDefaultSubobject<UDeliverAttributeSet>(TEXT("AttributeSet"));

	// 创建 任务追踪组件
	TaskTracker = CreateDefaultSubobject<UDeliveryTaskTrackerComponent>(TEXT("TaskTracker"));
}

void ADeliverPlayerState::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// 服务器 Possess 和拥有客户端 OnRep_Pawn 会广播 OnPawnSet
	OnPawnSet.AddDynamic(this, &ADeliverPlayerState::HandlePawnSet);
}

// OnPawnSet，角色被控制时回调
void ADeliverPlayerState::HandlePawnSet(APlayerState* /*Player*/, APawn* NewPawn, APawn* /*OldPawn*/)
{
	if (AbilitySystemComponent)
	{
		// InitActorInfo
		AbilitySystemComponent->InitializeAbilityActor(this /* PlayerState */, NewPawn /* Pawn */);
	}
}

UAbilitySystemComponent* ADeliverPlayerState::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}
