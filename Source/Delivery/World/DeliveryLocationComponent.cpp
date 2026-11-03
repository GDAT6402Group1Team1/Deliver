// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/DeliveryLocationComponent.h"

#include "World/DeliveryLocationRegistry.h"

UDeliveryLocationComponent::UDeliveryLocationComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

// 向当前世界的地点注册表登记自身
void UDeliveryLocationComponent::BeginPlay()
{
	Super::BeginPlay();

	if (LocationId.IsNone())
	{
		return;
	}

	if (UDeliveryLocationRegistry* Registry = UDeliveryLocationRegistry::Get(this))
	{
		Registry->Register(LocationId, this);
		RegisteredId = LocationId;
	}
}

// 从地点注册表移除自身，防止留下失效引用
void UDeliveryLocationComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (!RegisteredId.IsNone())
	{
		// 用 RegisteredId 而不是 LocationId：运行中改过 LocationId 的话，
		// 注册表里那条记录还挂在旧值下面，拿新值去摘会摘不掉、留下悬空条目
		if (UDeliveryLocationRegistry* Registry = UDeliveryLocationRegistry::Get(this))
		{
			Registry->Unregister(RegisteredId, this);
		}
		RegisteredId = NAME_None;
	}

	Super::EndPlay(EndPlayReason);
}
