// Copyright Epic Games, Inc. All Rights Reserved.

#include "Interaction/DeliveryInteractionProbeComponent.h"
#include "DeliveryCharacter.h"
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

	// 倒在地上的人不该还能按 F 上车 / 按 E 捡东西。清掉 Focused 就同时挡住了提示浮窗
	// 和按键——DoInteract/DoPickup 拿的都是这里的结果，不用在每个调用点各写一遍。
	const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Owner);
	if (Character && Character->IsIncapacitated())
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
