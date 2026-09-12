// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryRagdollCombatComponent.h"

#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "TimerManager.h"

UDeliveryRagdollCombatComponent::UDeliveryRagdollCombatComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UDeliveryRagdollCombatComponent::BeginPlay()
{
	Super::BeginPlay();
	Mesh = GetOwner()->FindComponentByClass<USkeletalMeshComponent>();
	ActiveRagdoll = GetOwner()->FindComponentByClass<UDeliveryActiveRagdollComponent>();
}

bool UDeliveryRagdollCombatComponent::StartPunch(EMeleeHand Hand, FVector AimDir)
{
	UWorld* World = GetWorld();
	if (bIsPunching || !World || !Mesh || !ActiveRagdoll || !ActiveRagdoll->IsRagdollActive())
	{
		return false;
	}

	CachedAimDir = FVector(AimDir.X, AimDir.Y, 0.f).GetSafeNormal();
	if (CachedAimDir.IsNearlyZero() || !ActiveRagdoll->CanDrivePunch(Hand == EMeleeHand::Left ? 1.f : -1.f))
	{
		return false;
	}

	ActiveHand = Hand;
	bIsPunching = true;
	const float HandSide = Hand == EMeleeHand::Left ? 1.0f : -1.0f;
	ActiveRagdoll->BeginBodyDrivenPunch(CachedAimDir, HandSide);

	FTimerManager& TimerManager = World->GetTimerManager();
	const float ReleaseTime = FMath::Max(WindupDelay, 0.05f);
	const float HitTime = FMath::Max(HitWindowDelay, ReleaseTime + 0.16f);
	TimerManager.SetTimer(PunchDriveTimer, this, &UDeliveryRagdollCombatComponent::FirePunchDrive, ReleaseTime, false);
	TimerManager.SetTimer(EndTimer, this, &UDeliveryRagdollCombatComponent::FirePunchEnded, FMath::Max(PunchEndDelay, HitTime + 0.06f), false);
	if (GetOwner()->HasAuthority())
	{
		TimerManager.SetTimer(HitWindowTimer, this, &UDeliveryRagdollCombatComponent::FireHitWindow, HitTime, false);
	}

	return true;
}

void UDeliveryRagdollCombatComponent::CancelPunch()
{
	if (!bIsPunching) return;
	GetWorld()->GetTimerManager().ClearTimer(HitWindowTimer);
	GetWorld()->GetTimerManager().ClearTimer(PunchDriveTimer);
	GetWorld()->GetTimerManager().ClearTimer(EndTimer);
	ActiveRagdoll->EndBodyDrivenPunch();
	bIsPunching = false;
}

void UDeliveryRagdollCombatComponent::FirePunchDrive()
{
	if (!bIsPunching) return;
	ActiveRagdoll->ReleaseBodyDrivenPunch();

	// 姿势电机负责轨迹，冲量负责让拳头一开始就带着速度出去。
	// 大头给拳头，前臂跟上，上臂只给一点，免得整条手臂把身体推走。
	const bool bLeft = ActiveHand == EMeleeHand::Left;
	Mesh->AddImpulse(CachedAimDir * PunchImpulse, bLeft ? LeftHandBone : RightHandBone, true);
	Mesh->AddImpulse(CachedAimDir * PunchImpulse * 0.5f, bLeft ? LeftForearmBone : RightForearmBone, true);
	Mesh->AddImpulse(CachedAimDir * PunchImpulse * 0.1f, bLeft ? LeftArmBone : RightArmBone, true);
}

FTransform UDeliveryRagdollCombatComponent::GetPunchTraceTransform() const
{
	const FName HandBone = ActiveHand == EMeleeHand::Left ? LeftHandBone : RightHandBone;
	const FVector Location = Mesh->GetBoneLocation(HandBone, EBoneSpaces::WorldSpace) + CachedAimDir * ArmReachOffset;
	return FTransform(CachedAimDir.ToOrientationQuat(), Location);
}

void UDeliveryRagdollCombatComponent::FireHitWindow()
{
	OnPunchHitWindow.Broadcast(ActiveHand);
}

void UDeliveryRagdollCombatComponent::FirePunchEnded()
{
	if (!bIsPunching) return;
	ActiveRagdoll->EndBodyDrivenPunch();
	bIsPunching = false;
	OnPunchEnded.Broadcast(ActiveHand);
}
