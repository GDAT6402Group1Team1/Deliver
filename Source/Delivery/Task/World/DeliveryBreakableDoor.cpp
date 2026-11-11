// Copyright Epic Games, Inc. All Rights Reserved.

#include "Task/World/DeliveryBreakableDoor.h"

#include "Components/StaticMeshComponent.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"
#include "Camera/DeliveryDoorHitCameraShake.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundAttenuation.h"

// 准备完整门体和碎片，初始只启用完整门体的显示与碰撞
ADeliveryBreakableDoor::ADeliveryBreakableDoor()
{
	bReplicates = true;
	SetReplicateMovement(false);
	PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;

	IntactDoor = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("IntactDoor"));
	SetRootComponent(IntactDoor);
	IntactDoor->SetCollisionProfileName(TEXT("BlockAll"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> DoorAsset(TEXT("/Game/model/door.door"));
	if (DoorAsset.Succeeded())
		IntactDoor->SetStaticMesh(DoorAsset.Object);

	Fragments = CreateDefaultSubobject<UGeometryCollectionComponent>(TEXT("Fragments"));
	Fragments->SetupAttachment(IntactDoor);
	static ConstructorHelpers::FObjectFinder<UGeometryCollection> FracturedAsset(
	    TEXT("/Game/crack/crackdoor.crackdoor"));
	if (FracturedAsset.Succeeded())
		Fragments->SetRestCollection(FracturedAsset.Object);
	Fragments->SetVisibility(false);
	Fragments->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Fragments->SetSimulatePhysics(false);
    ImpactVisual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ImpactVisual"));
    ImpactVisual->SetupAttachment(IntactDoor);
    ImpactVisual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    ImpactVisual->SetVisibility(false);
    static ConstructorHelpers::FObjectFinder<USoundBase> HitAsset(TEXT("/Game/Sound/SFX/SFX_Door_AxeHit"));
    static ConstructorHelpers::FObjectFinder<USoundBase> BreakAsset(TEXT("/Game/Sound/SFX/SFX_Door_Break"));
    if (HitAsset.Succeeded()) HitSound = HitAsset.Object;
    if (BreakAsset.Succeeded()) BreakSound = BreakAsset.Object;
}

void ADeliveryBreakableDoor::BeginPlay()
{
    Super::BeginPlay();
    ImpactVisual->SetStaticMesh(IntactDoor->GetStaticMesh());
    for (int32 i = 0; i < IntactDoor->GetNumMaterials(); ++i)
        ImpactVisual->SetMaterial(i, IntactDoor->GetMaterial(i));
    IntactDoor->SetVisibility(false, false);
    ImpactVisual->SetVisibility(!bBroken);
}

bool ADeliveryBreakableDoor::ReceiveAxeHit(APawn* Attacker, const FVector& HitPoint)
{
    if (!HasAuthority() || bBroken || !IsValid(Attacker) || Attacker->GetWorld() != GetWorld()) return false;
    HitCount = FMath::Min(HitCount + 1, FMath::Max(1, HitsToBreak));
    const bool bFinal = HitCount >= HitsToBreak;
    // 内置控制器 RPC 只发给本次挥斧者，旁观者不被强制晃镜头
    if (auto* PC = Cast<APlayerController>(Attacker->GetController()))
        PC->ClientStartCameraShake(UDeliveryDoorHitCameraShake::StaticClass(), bFinal ? 1.5f : 1.f);
    MulticastImpact(HitPoint, bFinal);
    if (bFinal) BreakFromAxe();
    ForceNetUpdate();
    return true;
}

void ADeliveryBreakableDoor::MulticastImpact_Implementation(FVector_NetQuantize HitPoint, bool bFinal)
{
    if (GetNetMode() == NM_DedicatedServer) return;
    ImpactElapsed = 0.f;
    SetActorTickEnabled(true);
    if (USoundBase* Sound = bFinal ? BreakSound.Get() : HitSound.Get())
    {
        auto* Attenuation = NewObject<USoundAttenuation>(this);
        Attenuation->Attenuation.bAttenuate = true;
        Attenuation->Attenuation.AttenuationShapeExtents = FVector(150.f);
        Attenuation->Attenuation.FalloffDistance = 1400.f;
        UGameplayStatics::PlaySoundAtLocation(this, Sound, HitPoint, 1.f, 1.f, 0.f, Attenuation);
    }
}

void ADeliveryBreakableDoor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    ImpactElapsed += DeltaSeconds;
    const float Envelope = FMath::Square(FMath::Clamp(1.f - ImpactElapsed / .32f, 0.f, 1.f));
    ImpactVisual->SetRelativeRotation(FRotator(0.f, FMath::Sin(ImpactElapsed * 65.f) * 3.5f * Envelope, 0.f));
    if (bBroken || ImpactElapsed >= .32f)
    {
        ImpactVisual->SetRelativeRotation(FRotator::ZeroRotator);
        SetActorTickEnabled(false);
    }
}

void ADeliveryBreakableDoor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ADeliveryBreakableDoor, bBroken);
    DOREPLIFETIME(ADeliveryBreakableDoor, HitCount);
}

// 服务器记录破坏状态，同一扇门只触发一次
bool ADeliveryBreakableDoor::BreakFromAxe()
{
	if (!HasAuthority() || bBroken || !IntactDoor || !IntactDoor->GetStaticMesh())
		return false;
	bBroken = true;
	OnRep_Broken();
	ForceNetUpdate();
	return true;
}

// 隐藏完整门体，启用碎片物理并施加散开冲量
void ADeliveryBreakableDoor::OnRep_Broken()
{
	if (!bBroken)
		return;
	IntactDoor->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	IntactDoor->SetVisibility(false, true);
    ImpactVisual->SetVisibility(false);
	if (Fragments && Fragments->GetRestCollection())
	{
		Fragments->SetVisibility(true, true);
		Fragments->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Fragments->SetSimulatePhysics(true);
		Fragments->CrumbleActiveClusters();
		Fragments->AddRadialImpulse(GetActorLocation(), 170.f, 350.f, ERadialImpulseFalloff::RIF_Linear,
		                            true);
	}
	OnDoorBroken.Broadcast();
}
