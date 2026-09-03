// Fill out your copyright notice in the Description page of Project Settings.

#include "DeliverPlayerState.h"
#include "DeliverAbilitySystemComponent.h"

ADeliverPlayerState::ADeliverPlayerState()
{
	// 创建 ASC
	AbilitySystemComponent = CreateDefaultSubobject<UDeliverAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
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
