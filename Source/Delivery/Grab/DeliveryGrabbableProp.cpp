#include "Grab/DeliveryGrabbableProp.h"

#include "Components/StaticMeshComponent.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryGrabbableProp::ADeliveryGrabbableProp()
{
	bReplicates = true;
	SetReplicateMovement(true);
	PhysicsMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PhysicsMesh"));
	PhysicsMesh->SetMobility(EComponentMobility::Movable);
	PhysicsMesh->SetCollisionProfileName(TEXT("PhysicsActor"));
	PhysicsMesh->SetSimulatePhysics(true);
	PhysicsMesh->SetLinearDamping(2.0f);
	PhysicsMesh->SetAngularDamping(6.0f);
	RootComponent = PhysicsMesh;
	Grabbable = CreateDefaultSubobject<UDeliveryGrabbableComponent>(TEXT("Grabbable"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube"));
	if (Cube.Succeeded()) PhysicsMesh->SetStaticMesh(Cube.Object);
}

void ADeliveryGrabbableProp::BeginPlay()
{
	Super::BeginPlay();
	// 物理质量需要等 BodyInstance 初始化后设置，不能在 CDO 构造时查询材质密度。
	PhysicsMesh->SetMassOverrideInKg(NAME_None, MassKg, true);
}
