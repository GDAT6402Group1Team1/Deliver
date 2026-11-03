#pragma once

#include "CoreMinimal.h"

class UAnimSequence;

/** 双臂方向数据，使用 X 向前、Y 向右、Z 向上的坐标系；不包含源骨架长度 */
struct FDeliveryAnimationArmPose
{
	// 采样结果，左右臂分别保存
	/** 上臂方向，下标 0 为左臂，1 为右臂 */
	FVector Upper[2];
	/** 前臂方向，下标 0 为左臂，1 为右臂 */
	FVector Lower[2];
	/** 姿态混合权重，由调用方设置 */
	float Weight = 0;
};

namespace DeliveryAnimationArmPose
{
// 动画采样与混合
/**
 * 采样动画中的双臂方向，动画必须包含双臂、髋部和头部骨骼
 * 采样时间以秒为单位，并限制在动画长度内
 * 输出保留调用方设置的混合权重，采样失败时不能使用方向数据
 */
bool Sample(const UAnimSequence* Animation, float Time, FDeliveryAnimationArmPose& Out);
/**
 * 计算动作片段的淡入、淡出权重
 * 所有时间参数均以秒为单位；片段范围外返回 0
 */
float BlendWeight(float Time, float Duration, float BlendIn, float BlendOut);
}
