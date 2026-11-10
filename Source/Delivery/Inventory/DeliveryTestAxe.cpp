// 创建用于验证拾取和斧头动作的测试物品
// Copyright Epic Games, Inc. All Rights Reserved.

#include "Inventory/DeliveryTestAxe.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryTestAxe::ADeliveryTestAxe()
{
	bUsesAxeSwing = true;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		ItemMesh->SetStaticMesh(CubeFinder.Object);
		ItemMesh->SetRelativeScale3D(FVector(0.10f, 0.10f, 0.62f));
		AxeHead = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("AxeHead"));
		AxeHead->SetupAttachment(ItemMesh);
		AxeHead->SetStaticMesh(CubeFinder.Object);
		AxeHead->SetRelativeLocation(FVector(0.0f, 0.0f, 65.0f));
		// 补偿斧柄根组件的不均匀缩放，使斧刃保持预期尺寸
		AxeHead->SetRelativeScale3D(FVector(1.8f, 4.8f, 0.258f));
		AxeHead->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	ItemComponent->DisplayName = NSLOCTEXT("DeliveryItems", "TestAxe", "Axe");
	static ConstructorHelpers::FObjectFinder<UTexture2D> AxeIcon(
	    TEXT("/Game/UI/Inventory/Textures/T_ItemIconAxe.T_ItemIconAxe"));
	if (AxeIcon.Succeeded())
		ItemComponent->Icon = AxeIcon.Object;
	ItemComponent->ItemType = EDeliveryInventoryItemType::BackpackItem;
	ItemComponent->MaxDurability = 100.0f;
	ItemComponent->Durability = 100.0f;
	// 将斧柄长轴转到手掌前方，并调整握持偏移
	// 让拳头握住斧柄下端，而不是斧柄中心
	HeldRelativeTransform = FTransform(FRotator(-90.0f, 0.0f, 0.0f), FVector(-18.0f, 0.0f, 0.0f));
}

// 准备测试斧头的物理和显示配置
void ADeliveryTestAxe::BeginPlay()
{
	Super::BeginPlay();
	// 运行时统一测试斧头的外形，避免使用蓝图中残留的旧组件配置
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
