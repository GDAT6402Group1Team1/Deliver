#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Vehicle/DeliveryRiderPose.h"
#include "DeliveryRiderAnimInstance.generated.h"

// 主线程把车上的输入/目标点复制进这个快照；动画求值线程不读取 Actor 或场景。
struct FDeliveryRiderFrame
{
	FDeliveryRiderMotion Motion;
	FDeliveryRiderSettings Settings;
	FTransform Contacts[4];
	FVector Forward = FVector::ForwardVector, Right = FVector::RightVector;
	bool bActive = false;
};

/** ABP_MotorbikeRider 的原生父类。原生动画代理输出坐姿＋惯性＋四肢 IK。 */
UCLASS(Transient, Blueprintable)
class DELIVERY_API UDeliveryRiderAnimInstance : public UAnimInstance
{
	GENERATED_BODY()
protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;
};
