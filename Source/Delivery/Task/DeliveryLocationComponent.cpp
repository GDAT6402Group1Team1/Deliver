// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryLocationComponent.h"

#include "DeliveryLocationRegistry.h"

UDeliveryLocationComponent::UDeliveryLocationComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

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
