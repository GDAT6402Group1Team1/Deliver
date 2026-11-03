#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "DeliveryHandPose.generated.h"

struct FReferenceSkeleton;

namespace DeliveryHandPose
{
struct FCurledBone
{
	int32 BoneIndex = INDEX_NONE;
	/** 相对父骨骼的附加旋转，直接左乘到参考姿势的局部旋转上 */
	FQuat Delta = FQuat::Identity;
};

// 握拳姿势计算
/**
 * 把参考姿势的手指卷成拳头；铰链轴取自手指自己在参考姿势里的自然弯曲方向
 * （第一节到第二节的叉乘），所以不需要知道这套骨架把哪个轴当作弯曲轴，
 * 也不会把手指掰向手背；手指完全伸直、判断不出方向时，那根手指不动
 */
void BuildFist(const FReferenceSkeleton& RefSkeleton, float FingerAngleDegrees, float ThumbAngleDegrees,
               TArray<FCurledBone>& OutBones);
}

USTRUCT()

struct FDeliveryHandPoseProxy : public FAnimInstanceProxy
{
	GENERATED_BODY()

	FDeliveryHandPoseProxy() = default;

	explicit FDeliveryHandPoseProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}

	virtual void Initialize(UAnimInstance* InAnimInstance) override;
	virtual bool Evaluate(FPoseContext& Output) override;

	void Build(const UAnimInstance* Instance);

  private:
	TArray<DeliveryHandPose::FCurledBone> CurledBones;
};

/**
 * 输出握拳姿势，让没有物理刚体的手指弯曲
 * 手臂等有刚体的骨骼仍由物理模拟控制
 */
UCLASS()

class DELIVERY_API UDeliveryHandPoseAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

  public:
	/** 每节手指弯曲多少度；三节累加起来才是握拳的总量 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hands",
	          meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float FingerCurlAngle = 52.0f;

	/** 拇指弯曲多少度；拇指压在手指外侧，弯太多会陷进拳头里 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hands",
	          meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float ThumbCurlAngle = 26.0f;

	// 姿势参数更新
	/** 改过上面两个角度之后调用，重新算一遍手指的卷曲量 */
	void RebuildFist();

  protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
};
