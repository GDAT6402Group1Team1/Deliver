// Copyright Epic Games, Inc. All Rights Reserved.
#include "Events/DeliveryEventComponent.h"

UDeliveryEventComponent::UDeliveryEventComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UDeliveryEventComponent::Publish(EDeliveryEvent Event)
{
    OnEvent.Broadcast(Event);
}
