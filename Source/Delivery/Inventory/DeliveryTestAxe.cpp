// Copyright Epic Games, Inc. All Rights Reserved.

#include "Inventory/DeliveryTestAxe.h"
#include "Components/StaticMeshComponent.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryTestAxe::ADeliveryTestAxe()
{
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		ItemMesh->SetStaticMesh(CubeFinder.Object);
		ItemMesh->SetRelativeScale3D(FVector(0.10f, 0.10f, 0.62f));
		AxeHead = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("AxeHead"));
		AxeHead->SetupAttachment(ItemMesh);
		AxeHead->SetStaticMesh(CubeFinder.Object);
		AxeHead->SetRelativeLocation(FVector(0.0f, 0.0f, 65.0f));
		// Compensate for the handle's non-uniform root scale: final world size is about 18x48x16 cm.
		AxeHead->SetRelativeScale3D(FVector(1.8f, 4.8f, 0.258f));
		AxeHead->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	ItemComponent->DisplayName = NSLOCTEXT("DeliveryItems", "TestAxe", "斧头");
	ItemComponent->ItemType = EDeliveryInventoryItemType::BackpackItem;
	ItemComponent->MaxDurability = 100.0f;
	ItemComponent->Durability = 100.0f;
	// The cube handle's long axis is local Z. Pitch it onto the hand's forward axis and
	// offset the pivot so the fist sits near the bottom of the handle, not at its centre.
	HeldRelativeTransform = FTransform(FRotator(-90.0f, 0.0f, 0.0f), FVector(-18.0f, 0.0f, 0.0f));
}

void ADeliveryTestAxe::BeginPlay()
{
	Super::BeginPlay();
	// Force a compact, unmistakable blocky axe at runtime instead of inheriting a stale BP component template.
	if (UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
	{
		ItemMesh->SetStaticMesh(CubeMesh);
		ItemMesh->SetRelativeScale3D(FVector(0.06f, 0.06f, 0.45f));
		if (AxeHead)
		{
			AxeHead->SetStaticMesh(CubeMesh);
			AxeHead->SetVisibility(true, true);
			AxeHead->SetAbsolute(false, false, false);
			AxeHead->SetRelativeLocation(FVector(0.0f, 0.0f, 65.0f));
			AxeHead->SetRelativeScale3D(FVector(3.0f, 8.0f, 0.35f));
		}
		HeldRelativeTransform = FTransform(FRotator(-90.0f, 0.0f, 0.0f), FVector(-18.0f, 0.0f, 0.0f));
	}
	ItemMesh->SetMassOverrideInKg(NAME_None, 2.5f, true);
}
