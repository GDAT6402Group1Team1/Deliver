#include "DeliveryActiveRagdollComponent.h"
#include "Animation/AnimSequence.h"
#include "DeliveryCharacter.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "GameFramework/GameStateBase.h"
#include "Engine/World.h"

float UDeliveryActiveRagdollComponent::GetAxeSwingTime() const
{
	if (!GetWorld() || AxeSwingStartTime < 0) return -1.f;
	const AGameStateBase* State = GetWorld()->GetGameState();
	const float Now = State ? State->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds();
	return FMath::Max(0.f, Now - AxeSwingStartTime) * FMath::Clamp(AxeSwingPlayRate, .1f, 3.f);
}

float UDeliveryActiveRagdollComponent::GetAxeSwingWeight() const
{
	const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
	if (!AxeSwingAnimation || !SwingAxe.IsValid() || !Character || Character->IsIncapacitated()
		|| !bIsActive || ReplicatedControlMode != EDeliveryRagdollControlMode::Active
		|| !Character->GetInventoryComponent() || Character->GetInventoryComponent()->GetHeldItem() != SwingAxe.Get()
		|| (Character->GetGrabComponent() && Character->GetGrabComponent()->IsGrabbing())) return 0;
	return DeliveryAnimationArmPose::BlendWeight(GetAxeSwingTime(), AxeSwingAnimation->GetPlayLength(),
		AxeSwingBlendIn, AxeSwingBlendOut);
}

bool UDeliveryActiveRagdollComponent::CanStartAxeSwing() const
{
	const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
	return Character && !Character->IsIncapacitated() && CanDrivePunch(-1.f) && AxeSwingAnimation
		&& !bBodyDrivenPunchActive && !bRisingFromLimp
		&& Character->GetInventoryComponent() && Character->GetInventoryComponent()->GetHeldItem()
		&& Character->GetInventoryComponent()->GetHeldItem()->bUsesAxeSwing
		&& (!Character->GetGrabComponent() || !Character->GetGrabComponent()->IsGrabbing())
		&& (AxeSwingStartTime < 0 || GetAxeSwingTime() >= AxeSwingAnimation->GetPlayLength() + .1f);
}

void UDeliveryActiveRagdollComponent::RequestAxeSwing()
{
	if (CanStartAxeSwing()) ServerRequestAxeSwing();
}

void UDeliveryActiveRagdollComponent::ServerRequestAxeSwing_Implementation()
{
	if (!CanStartAxeSwing()) return;
	FDeliveryAnimationArmPose Sample;
	if (!DeliveryAnimationArmPose::Sample(AxeSwingAnimation, 0, Sample)) return;
	ADeliveryCharacter* Character = CastChecked<ADeliveryCharacter>(GetOwner());
	const AGameStateBase* State = GetWorld()->GetGameState();
	MulticastStartAxeSwing(Character->GetInventoryComponent()->GetHeldItem(),
		State ? State->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds());
}

void UDeliveryActiveRagdollComponent::MulticastStartAxeSwing_Implementation(ADeliveryHandheldItem* Axe, float ServerStartTime)
{
	SwingAxe = Axe;
	AxeSwingStartTime = ServerStartTime;
}

bool UDeliveryActiveRagdollComponent::SampleAxeSwing(FDeliveryAnimationArmPose& Out)
{
	Out.Weight = GetAxeSwingWeight();
	return Out.Weight > 0 && DeliveryAnimationArmPose::Sample(AxeSwingAnimation, GetAxeSwingTime(), Out);
}
