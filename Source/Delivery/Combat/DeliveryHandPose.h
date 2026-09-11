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
		/** 相对父骨骼的附加旋转，直接左乘到参考姿势的局部旋转上。 */
		FQuat Delta = FQuat::Identity;
	};

	/**
	 * 把参考姿势的手指卷成拳头。铰链轴取自手指自己在参考姿势里的自然弯曲方向
	 * （第一节到第二节的叉乘），所以不需要知道这套骨架把哪个轴当作弯曲轴，
	 * 也不会把手指掰向手背。手指完全伸直、判断不出方向时，那根手指不动。
	 */
	void BuildFist(const FReferenceSkeleton& RefSkeleton, float FingerAngleDegrees,
		float ThumbAngleDegrees, TArray<FCurledBone>& OutBones);
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
 * 手指没有物理刚体，姿势电机管不到它们，它们只能跟着动画姿势走。角色本来没有动画实例，
 * 用的就是骨架参考姿势——Mixamo 的参考姿势是五指张开的，所以无论手臂怎么走，
 * 打出去的都是一只张开的手掌，看着永远像扇巴掌。
 *
 * 这个动画实例只做一件事：输出参考姿势，并把手指卷成拳头。有刚体的骨骼随后会被物理覆盖。
 */
UCLASS()
class DELIVERY_API UDeliveryHandPoseAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	/** 每节手指弯曲多少度。三节累加起来才是握拳的总量。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Hands", meta=(ClampMin="0.0", ClampMax="90.0"))
	float FingerCurlAngle = 52.0f;

	/** 拇指弯曲多少度。拇指压在手指外侧，弯太多会陷进拳头里。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Hands", meta=(ClampMin="0.0", ClampMax="90.0"))
	float ThumbCurlAngle = 26.0f;

	/** 改过上面两个角度之后调用，重新算一遍手指的卷曲量。 */
	void RebuildFist();

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
};
