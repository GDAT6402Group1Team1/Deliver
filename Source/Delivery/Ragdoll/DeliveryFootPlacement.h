#pragma once

#include "CoreMinimal.h"

namespace DeliveryFootPlacement
{
inline FVector SideAxis(const FVector& GroundNormal, float BodyYaw)
{
	return FVector::CrossProduct(GroundNormal, FRotator(0.0f, BodyYaw, 0.0f).Vector()).GetSafeNormal();
}

inline float SeparationAcceleration(float Side, float OutwardSpeed, float MinimumSide)
{
	return Side >= MinimumSide ? 0.0f
		: FMath::Clamp((MinimumSide - Side) * 120.0f - OutwardSpeed * 20.0f, 0.0f, 1200.0f);
}
}
