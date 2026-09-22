// Copyright Epic Games, Inc. All Rights Reserved.

#include "Interaction/DeliveryInteractionProbeComponent.h"
#include "GameFramework/Pawn.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Interaction/DeliveryPromptSubsystem.h"

UDeliveryInteractionProbeComponent::UDeliveryInteractionProbeComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// 走近/走远的判定不需要每帧重算，10Hz 眼睛看不出差别。
	PrimaryComponentTick.TickInterval = 0.1f;
	SetIsReplicatedByDefault(false);
}

void UDeliveryInteractionProbeComponent::TickComponent(
	float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const APawn* Owner = Cast<APawn>(GetOwner());
	// 不是本机操控的 Pawn 就别探测：包括别人的角色，也包括自己上了车之后被丢下的那具身体。
	if (!Owner || !Owner->IsLocallyControlled())
	{
		Focused = nullptr;
		return;
	}

	UDeliveryInteractableComponent* Best = UDeliveryInteractableComponent::FindBest(Owner);
	Focused = Best;

	if (bShowPrompt && Best)
	{
		if (UDeliveryPromptSubsystem* Prompt = UDeliveryPromptSubsystem::Get(this))
		{
			Prompt->PushPrompt(Best->PromptText, Best->GetPromptLocation());
		}
	}
}

AActor* UDeliveryInteractionProbeComponent::GetFocusedActor() const
{
	const UDeliveryInteractableComponent* Target = Focused.Get();
	return Target ? Target->GetOwner() : nullptr;
}
