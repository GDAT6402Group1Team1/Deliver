#pragma once
#include "CoreMinimal.h"

namespace DeliveryInteractionGeometry
{
inline bool IsWithinReachFacing(const FVector& ToTarget, const FRotator& ViewRotation)
{
	return ToTarget.SizeSquared2D() <= FMath::Square(20.0f)
		|| FVector::DotProduct(ViewRotation.Vector().GetSafeNormal2D(), ToTarget.GetSafeNormal2D()) >= 0.35f;
}
}
