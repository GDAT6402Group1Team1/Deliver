// Copyright Epic Games, Inc. All Rights Reserved.

#include "Inventory/DeliveryPackageItem.h"
#include "Components/StaticMeshComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Task/DeliveryItemComponent.h"
#include "Task/DeliveryTaskDefinition.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryPackageItem::ADeliveryPackageItem()
{
	DeliveryItemComponent = CreateDefaultSubobject<UDeliveryItemComponent>(TEXT("DeliveryTaskItem"));
	ItemComponent->ItemType = EDeliveryInventoryItemType::DeliveryItem;
	ItemComponent->DisplayName = NSLOCTEXT("DeliveryItems", "DeliveryPackage", "快递");
	ItemComponent->bUsesDurability = false;
	ItemComponent->MaxDurability = 0.0f;
	ItemComponent->Durability = 0.0f;
	InteractableComponent->PromptText = NSLOCTEXT("DeliveryItems", "HoldPickupPackage", "[按住 E] 拾取快递");
	InteractableComponent->InteractionKey = EDeliveryInteractionKey::PickupE;
	InteractableComponent->HoldDuration = 0.5f;
	InteractableComponent->InteractRadius = 100.0f;
	HeldRelativeTransform = FTransform(FRotator::ZeroRotator, FVector(18.0f, 0.0f, 0.0f));
}

ADeliveryTestPackage::ADeliveryTestPackage()
{
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		ItemMesh->SetStaticMesh(CubeFinder.Object);
		ItemMesh->SetRelativeScale3D(FVector(0.32f, 0.26f, 0.20f));
	}
	static ConstructorHelpers::FObjectFinder<UDeliveryTaskDefinition> TaskFinder(
		TEXT("/Game/Task/Definitions/DA_Task_001.DA_Task_001"));
	if (TaskFinder.Succeeded()) DeliveryItemComponent->OwningTask = TaskFinder.Object;
}

void ADeliveryTestPackage::BeginPlay()
{
	Super::BeginPlay();
	if (UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
	{
		ItemMesh->SetStaticMesh(Cube);
		ItemMesh->SetRelativeScale3D(FVector(0.32f, 0.26f, 0.20f));
	}
	ItemMesh->SetMassOverrideInKg(NAME_None, 1.5f, true);
}
