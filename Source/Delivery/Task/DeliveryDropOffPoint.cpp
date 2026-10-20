// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryDropOffPoint.h"

#include "Interaction/DeliveryInteractableComponent.h"
#include "Task/DeliveryTargetComponent.h"

ADeliveryDropOffPoint::ADeliveryDropOffPoint()
{
	Interactable = CreateDefaultSubobject<UDeliveryInteractableComponent>(TEXT("Interactable"));
	// 和取货点、快递箱子三处保持同一套参数。玩家的肌肉记忆是"按住 E"，
	// 这里换成点一下会让人以为没按上
	Interactable->InteractionKey = EDeliveryInteractionKey::PickupE;
	Interactable->HoldDuration = 0.5f;
	// 半径不在这里设：归 Target 组件管，BeginPlay 时统一写入
	Interactable->PromptText = NSLOCTEXT("DeliveryPickup", "HoldDeliverPackage", "[按住 E] 交付快递");
	Interactable->PromptOffset = FVector(0.f, 0.f, 150.f);

	Target = CreateDefaultSubobject<UDeliveryTargetComponent>(TEXT("Target"));
	// 交付半径和提示半径的对齐放在 Target 组件的 BeginPlay 里统一做（它那边会按
	// 本 Actor 转发进来的 InteractRadius 把两者都写一遍）。这里再设一次会读到
	// 可交互组件的类默认值 300、和 Actor 上填的数不一致，白留一个误导人的数
}

void ADeliveryDropOffPoint::OnConstruction(const FTransform& Transform)
{
	// 父类会把 LocationId 转发给地点组件
	Super::OnConstruction(Transform);

	// 和 LocationId 同样的理由放在 OnConstruction：编辑器里填完就能在组件详情里
	// 看到，不用进 PIE 才发现填错了
	if (Target)
	{
		Target->ExpectedTask = ExpectedTask;
		Target->InteractRadius = InteractRadius;
	}
}
