// Copyright Epic Games, Inc. All Rights Reserved.

#include "Task/DeliveryTestTarget.h"
#include "Components/StaticMeshComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Task/DeliveryTargetComponent.h"
#include "Task/DeliveryTaskDefinition.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryTestTarget::ADeliveryTestTarget()
{
	TargetMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TargetMesh"));
	RootComponent = TargetMesh;
	TargetMesh->SetCollisionProfileName(TEXT("BlockAll"));
	TargetMesh->SetSimulatePhysics(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		TargetMesh->SetStaticMesh(CubeFinder.Object);
		TargetMesh->SetRelativeScale3D(FVector(0.6f, 0.6f, 1.2f));
	}

	InteractableComponent = CreateDefaultSubobject<UDeliveryInteractableComponent>(TEXT("Interactable"));
	InteractableComponent->PromptText = NSLOCTEXT("DeliveryItems", "HoldDeliverPackage", "[按住 E] 交付快递");
	InteractableComponent->InteractionKey = EDeliveryInteractionKey::PickupE;
	InteractableComponent->HoldDuration = 0.5f;
	InteractableComponent->InteractRadius = 100.0f;
	InteractableComponent->PromptOffset = FVector(0.0f, 0.0f, 130.0f);

	DeliveryTargetComponent = CreateDefaultSubobject<UDeliveryTargetComponent>(TEXT("DeliveryTarget"));
	DeliveryTargetComponent->DeliveryRadius = 100.0f;
	static ConstructorHelpers::FObjectFinder<UDeliveryTaskDefinition> TaskFinder(
		TEXT("/Game/Task/Definitions/DA_Task_001.DA_Task_001"));
	if (TaskFinder.Succeeded()) DeliveryTargetComponent->ExpectedTask = TaskFinder.Object;
}
