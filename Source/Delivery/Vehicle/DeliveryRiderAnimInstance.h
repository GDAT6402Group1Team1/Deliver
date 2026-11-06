#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Vehicle/DeliveryRiderPose.h"
#include "DeliveryRiderAnimInstance.generated.h"

// 主线程把车上的输入/目标点复制进这个快照；动画求值线程不读取 Actor 或场景
struct FDeliveryRiderFrame
{
	// 动画线程读取的运动与接触数据
	FDeliveryRiderMotion Motion;
	FDeliveryRiderSettings Settings;
	// 左手、右手、左脚、右脚目标，位置和旋转都相对 RiderMesh
	FTransform Contacts[4];
	FVector Forward = FVector::ForwardVector, Right = FVector::RightVector;
	// 有驾驶员和有效骑手模型才计算动作，编辑器独立预览时通常为 false
	bool bActive = false;
};

/**
 * 骑手动画实例，通过 C++ 代理读取车辆运动并输出骨骼姿态
 * 身体先摆动，再调整手臂和腿贴近车把与脚踏，不依赖 AnimGraph 节点播放动作
 */
UCLASS(Transient, Blueprintable)

class DELIVERY_API UDeliveryRiderAnimInstance : public UAnimInstance
{
	GENERATED_BODY()
  protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;
};
