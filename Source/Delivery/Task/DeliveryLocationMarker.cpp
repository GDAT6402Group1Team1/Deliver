// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryLocationMarker.h"

#include "Components/BillboardComponent.h"
#include "DeliveryLocationComponent.h"

ADeliveryLocationMarker::ADeliveryLocationMarker()
{
	PrimaryActorTick.bCanEverTick = false;

	LocationComponent = CreateDefaultSubobject<UDeliveryLocationComponent>(TEXT("Location"));
	SetRootComponent(LocationComponent);

#if WITH_EDITORONLY_DATA
	Billboard = CreateDefaultSubobject<UBillboardComponent>(TEXT("Billboard"));
	Billboard->SetupAttachment(LocationComponent);
	Billboard->bIsEditorOnly = true;
#endif
}

void ADeliveryLocationMarker::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// 把 Actor 上填的 ID 转发给组件。放在 OnConstruction 而不是 BeginPlay：
	// 编辑器里改完 ID 就能立刻在组件详情里看到，不用进 PIE 才发现填错了
	if (LocationComponent)
	{
		LocationComponent->LocationId = LocationId;
	}
}
