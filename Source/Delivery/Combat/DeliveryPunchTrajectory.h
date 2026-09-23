#pragma once

#include "CoreMinimal.h"

// 只计算肩到拳头的目标，不读取骨骼，也不驱动物理电机。
// 读这个文件时可以把肩看成原点：Direction * Distance 就是拳头相对肩的位置。
namespace DeliveryPunchTrajectory
{
struct FArmReach
{
	// Direction × Distance = 手相对肩的位置；Pole 是肘关节优先弯曲的一侧。
	FVector Direction;
	float Distance;
	FVector Pole;
};

inline FArmReach MakeReach(const FVector& Offset, const FVector& Pole, float MaxDistance)
{
	// 把任意肩→手偏移拆成方向和距离，并限制为略短于手臂全长。
	return { Offset.GetSafeNormal(), FMath::Min(float(Offset.Size()), MaxDistance), Pole.GetSafeNormal() };
}

// 蓄力时先改变手臂方向，再逐渐改变距离，避免手划出很大的横弧。
inline FArmReach BlendReach(const FArmReach& From, const FArmReach& To, float Alpha, float DistanceAlpha)
{
	FArmReach Out;
	Out.Direction = FMath::Lerp(From.Direction, To.Direction, Alpha).GetSafeNormal();
	if (Out.Direction.IsNearlyZero()) Out.Direction = To.Direction;
	Out.Distance = FMath::Lerp(From.Distance, To.Distance, DistanceAlpha);
	Out.Pole = FMath::Lerp(From.Pole, To.Pole, Alpha).GetSafeNormal();
	if (Out.Pole.IsNearlyZero()) Out.Pole = To.Pole;
	return Out;
}

inline float StrikeProgress(float Extension)
{
	// Extension 从 0 增到 1；这条曲线一开始斜率较大，临近伸直时变平。
	return 1.0f - FMath::Square(1.0f - Extension);
}

inline float RetractProgress(float Extension)
{
	// 收拳时 Extension 从 1 减到 0；两端斜率小，避免突然停住或落回垂手姿势。
	return FMath::SmoothStep(0.0f, 1.0f, Extension);
}

inline FVector WindupOffset(const FVector& Forward, const FVector& Outward,
	float Length, float Back, float Up, float Side)
{
	// 三个分量依次是向后蓄力、抬高手、向身体外侧让开；参数按手臂长度缩放。
	return (-Forward * Back + FVector::UpVector * Up + Outward * Side) * Length;
}

inline FVector StrikeOffset(const FVector& Forward, const FVector& Outward,
	float Length, float Reach, float Inward, float Drop)
{
	// 从肩部向前打出，同时略向内收、向下压；只返回几何目标，不施加物理力。
	return (Forward * Reach - Outward * Inward - FVector::UpVector * Drop) * Length;
}
}
