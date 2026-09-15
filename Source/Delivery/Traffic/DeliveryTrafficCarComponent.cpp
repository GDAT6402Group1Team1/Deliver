// Copyright Epic Games, Inc. All Rights Reserved.

#include "Traffic/DeliveryTrafficCarComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

UDeliveryTrafficCarComponent::UDeliveryTrafficCarComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UDeliveryTrafficCarComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bIsCurrentlyOnRoute)
	{
		RouteLostElapsedTime += DeltaTime;
		if (RouteLostElapsedTime >= RouteLostTimeout)
		{
			RouteLostElapsedTime = 0.0f;
			OnRouteLost.Broadcast();
		}
	}

	bool bBlocked = IsCarAhead();
	if (bBlocked)
	{
		BlockedElapsedTime += DeltaTime;
		if (BlockedElapsedTime >= MaxBlockedTime)
		{
			// 汇入口两辆车互为"前车"时会一直卡住，卡够久就放弃避让强行通过，打破死锁。
			bBlocked = false;
		}
	}
	else
	{
		BlockedElapsedTime = 0.0f;
	}

	const float Target = bBlocked ? 0.0f : 1.0f;
	SpeedMultiplier = FMath::FInterpConstantTo(SpeedMultiplier, Target, DeltaTime, SpeedMultiplierChangeRate);
}

void UDeliveryTrafficCarComponent::UpdateRouteFollowState(bool bIsFollowingRoute)
{
	bIsCurrentlyOnRoute = bIsFollowingRoute;
	if (bIsFollowingRoute)
	{
		RouteLostElapsedTime = 0.0f;
	}
}

void UDeliveryTrafficCarComponent::NotifyResetToStart()
{
	bIsCurrentlyOnRoute = true;
	RouteLostElapsedTime = 0.0f;
}

bool UDeliveryTrafficCarComponent::IsCarAhead() const
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return false;
	}

	const FVector Forward = Owner->GetActorForwardVector();
	const FVector Start = Owner->GetActorLocation() + Forward * TraceStartForwardOffset;
	const FVector End = Start + Forward * ForwardTraceDistance;

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(DeliveryTrafficCarForwardTrace), false, Owner);
	QueryParams.AddIgnoredActor(Owner);

	TArray<FHitResult> Hits;
	GetWorld()->SweepMultiByChannel(Hits, Start, End, FQuat::Identity, TraceChannel.GetValue(), FCollisionShape::MakeSphere(ForwardTraceRadius), QueryParams);

	if (bDrawDebugTrace)
	{
		DrawDebugCapsule(GetWorld(), (Start + End) * 0.5f, (End - Start).Size() * 0.5f, ForwardTraceRadius, FRotationMatrix::MakeFromZ(Forward).ToQuat(), FColor::Yellow, false, -1.0f, 0, 1.5f);
	}

	for (const FHitResult& Hit : Hits)
	{
		const AActor* HitActor = Hit.GetActor();
		if (HitActor && HitActor != Owner && HitActor->FindComponentByClass<UDeliveryTrafficCarComponent>())
		{
			return true;
		}
	}

	return false;
}
