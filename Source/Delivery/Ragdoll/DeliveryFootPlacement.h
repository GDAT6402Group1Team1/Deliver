#pragma once

#include "CoreMinimal.h"

// 这里都是不读场景、不施加物理力的计算：输入方向、位置和步伐进度，输出目标值。
// 地面射线、真实刚体位置、电机设置都留在 DeliveryActiveRagdollGait.cpp。
namespace DeliveryFootPlacement
{
inline FVector SideAxis(const FVector& GroundNormal, float BodyYaw)
{
	// 坡面法线 × 身体前方 = 坡面上的身体右侧；左右脚都以这条轴判断所属侧。
	return FVector::CrossProduct(GroundNormal, FRotator(0.0f, BodyYaw, 0.0f).Vector()).GetSafeNormal();
}

inline float SeparationAcceleration(float Side, float OutwardSpeed, float MinimumSide)
{
	// Side 不够宽时给脚向外的加速度；如果脚已经向外运动，就相应减小推力。
	// 上限避免修正过猛，把腿弹开。
	return Side >= MinimumSide ? 0.0f
		: FMath::Clamp((MinimumSide - Side) * 120.0f - OutwardSpeed * 20.0f, 0.0f, 1200.0f);
}

// 只算脚在摆动中的目标位置。Alpha=0 是起点，Alpha=1 是落点；
// 中间沿地面法线抬脚，两端速度变缓。
inline FVector SwingPosition(const FVector& Start, const FVector& Target,
	const FVector& GroundNormal, float Alpha, float StepHeight, float& OutSmoothAlpha)
{
	// 五次缓动让抬脚和落脚两端的水平速度平缓；正弦项只在中途抬高脚。
	OutSmoothAlpha = Alpha * Alpha * Alpha * (Alpha * (Alpha * 6.0f - 15.0f) + 10.0f);
	FVector Position = FMath::Lerp(Start, Target, OutSmoothAlpha);
	Position += GroundNormal * (FMath::Square(FMath::Sin(PI * Alpha)) * StepHeight);
	return Position;
}

// 防止脚在移动目标或坡面上跨过身体中线。只向它本来所属的一侧修正。
inline FVector KeepOnSide(FVector Position, const FVector& Center,
	const FVector& SideAxis, float SideSign, float MinimumSide)
{
	const float SignedSide = FVector::DotProduct(Position - Center, SideAxis) * SideSign;
	if (SignedSide < MinimumSide)
	{
		Position += SideAxis * SideSign * (MinimumSide - SignedSide);
	}
	return Position;
}

// 先在坡面切平面上算落点，随后由调用方做地面射线，把它落到真实表面。
inline FVector LandingOnPlane(const FVector& PelvisTarget, const FVector& Forward,
	const FVector& Right, const FVector& Hips, float SideSign, float ForwardDistance,
	float VelocityLead, float Stance, float MinimumSide)
{
	const FVector Destination = PelvisTarget + Forward * (ForwardDistance + VelocityLead)
		+ Right * (SideSign * Stance);
	return KeepOnSide(Destination, Hips, Right, SideSign, MinimumSide);
}
}
