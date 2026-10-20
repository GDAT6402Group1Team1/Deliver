// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryPickupPoint.h"

#include "Interaction/DeliveryInteractableComponent.h"
#include "Task/DeliveryPickupPointComponent.h"

ADeliveryPickupPoint::ADeliveryPickupPoint()
{
	Interactable = CreateDefaultSubobject<UDeliveryInteractableComponent>(TEXT("Interactable"));
	// 和快递箱子那套参数保持一致：按住 0.5 秒、E 键、半径 100 cm。
	// 玩家的肌肉记忆是从"按住 E 取快递"来的，换成点一下或者换半径都会让人以为坏了
	Interactable->InteractionKey = EDeliveryInteractionKey::PickupE;
	Interactable->HoldDuration = 0.5f;
	// 半径不在这里设：它归 PickupPoint 组件管（BeginPlay 时统一写入），
	// 这里再写一份会让人以为改这个数有用
	Interactable->PromptText = NSLOCTEXT("DeliveryPickup", "HoldTakePackage", "[按住 E] 取快递");
	// 提示浮窗吊得比箱子高一点：取货点可能是个柜台或者整栋楼，
	// 按箱子那个高度会被模型挡住
	Interactable->PromptOffset = FVector(0.f, 0.f, 150.f);

	PickupPoint = CreateDefaultSubobject<UDeliveryPickupPointComponent>(TEXT("PickupPoint"));
}

void ADeliveryPickupPoint::OnConstruction(const FTransform& Transform)
{
	// 父类会把 LocationId 转发给地点组件
	Super::OnConstruction(Transform);

	// 和 LocationId 同样的理由放在 OnConstruction：编辑器里填完就能在组件详情里
	// 看到，不用进 PIE 才发现填错了
	if (PickupPoint)
	{
		PickupPoint->PackageClass = PackageClass;
		PickupPoint->SpawnOffset = SpawnOffset;
		PickupPoint->InteractRadius = InteractRadius;
	}
}
