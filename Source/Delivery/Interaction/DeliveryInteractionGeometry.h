#pragma once
#include "CoreMinimal.h"

namespace DeliveryInteractionGeometry
{
// 摄像机准星到目标可见部分的横向距离。返回负数表示目标在镜头后方。
inline float AimMissDistance(const FVector& RayStart, const FVector& RayDirection, const FVector& Target)
{
	const FVector ToTarget = Target - RayStart;
	const float AlongRay = FVector::DotProduct(ToTarget, RayDirection);
	return AlongRay > 0.0f ? (ToTarget - RayDirection * AlongRay).Size() : -1.0f;
}

inline bool IsWithinReachFacing(const FVector& ToTarget, const FRotator& ViewRotation)
{
	return ToTarget.SizeSquared2D() <= FMath::Square(20.0f)
		|| FVector::DotProduct(ViewRotation.Vector().GetSafeNormal2D(), ToTarget.GetSafeNormal2D()) >= 0.35f;
}
}
