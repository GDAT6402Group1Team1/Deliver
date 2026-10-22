// Copyright Epic Games, Inc. All Rights Reserved.

#include "Task/DeliveryBreakableDoor.h"

#include "Components/StaticMeshComponent.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryBreakableDoor::ADeliveryBreakableDoor()
{
	bReplicates = true;
	SetReplicateMovement(false);
	PrimaryActorTick.bCanEverTick = false;

	IntactDoor = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("IntactDoor"));
	SetRootComponent(IntactDoor);
	IntactDoor->SetCollisionProfileName(TEXT("BlockAll"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> DoorAsset(TEXT("/Game/model/door.door"));
	if (DoorAsset.Succeeded()) IntactDoor->SetStaticMesh(DoorAsset.Object);

	Fragments = CreateDefaultSubobject<UGeometryCollectionComponent>(TEXT("Fragments"));
	Fragments->SetupAttachment(IntactDoor);
	static ConstructorHelpers::FObjectFinder<UGeometryCollection> FracturedAsset(TEXT("/Game/crack/crackdoor.crackdoor"));
	if (FracturedAsset.Succeeded()) Fragments->SetRestCollection(FracturedAsset.Object);
	Fragments->SetVisibility(false);
	Fragments->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Fragments->SetSimulatePhysics(false);

}

void ADeliveryBreakableDoor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ADeliveryBreakableDoor, bBroken);
}

bool ADeliveryBreakableDoor::BreakFromAxe()
{
	if (!HasAuthority() || bBroken || !IntactDoor || !IntactDoor->GetStaticMesh()) return false;
	bBroken = true;
	OnRep_Broken();
	ForceNetUpdate();
	return true;
}

void ADeliveryBreakableDoor::OnRep_Broken()
{
	if (!bBroken) return;
	IntactDoor->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	IntactDoor->SetVisibility(false, true);
	if (Fragments && Fragments->GetRestCollection())
	{
		Fragments->SetVisibility(true, true);
		Fragments->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Fragments->SetSimulatePhysics(true);
		Fragments->CrumbleActiveClusters();
		Fragments->AddRadialImpulse(GetActorLocation(), 170.f, 350.f, ERadialImpulseFalloff::RIF_Linear, true);
	}
}
