// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryTargetComponent.h"

#include "GameFramework/Actor.h"
#include "Task/DeliveryItemComponent.h"
#include "Task/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"

UDeliveryTargetComponent::UDeliveryTargetComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

bool UDeliveryTargetComponent::CanAcceptDelivery(AActor* ItemActor, APlayerState* /*Player*/) const
{
	if (!ExpectedTask || !ItemActor || !GetOwner())
	{
		return false;
	}

	// 必须是这个任务的快递，不能拿别的包裹来交差
	const UDeliveryItemComponent* Item = ItemActor->FindComponentByClass<UDeliveryItemComponent>();
	if (!Item || Item->OwningTask != ExpectedTask)
	{
		return false;
	}

	const UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);
	if (!Manager || Manager->GetTaskStatus(ExpectedTask) != EDeliveryTaskStatus::InProgress)
	{
		return false;
	}

	// 判距离用快递本身的位置：它此刻在谁手上、在不在车里都由交互系统决定
	return FVector::Dist(ItemActor->GetActorLocation(), GetOwner()->GetActorLocation()) <= DeliveryRadius;
}

bool UDeliveryTargetComponent::TryDeliver(AActor* ItemActor, APlayerState* Player)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !CanAcceptDelivery(ItemActor, Player))
	{
		return false;
	}

	UDeliveryTaskManagerComponent* Manager = UDeliveryTaskManagerComponent::Get(this);
	if (!Manager || !Manager->TryCompleteDelivery(ExpectedTask, Player))
	{
		return false;
	}

	ItemActor->Destroy();

	return true;
}
