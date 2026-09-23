#pragma once

#include "CoreMinimal.h"
#include "ReferenceSkeleton.h"
#include "DeliveryRiderPose.generated.h"

/** 只复制骑手表现需要的输入；车的移动和碰撞仍由摩托车自身计算。 */
USTRUCT()
struct FDeliveryRiderMotion
{
	GENERATED_BODY()
	UPROPERTY() float Speed = 0;
	UPROPERTY() float Acceleration = 0;
	UPROPERTY() float Steer = 0;
	UPROPERTY() float Bump = 0;
	UPROPERTY() FVector2D Impact = FVector2D::ZeroVector;
};

/** 一组可在摩托车蓝图里调的表现参数。角度单位为度，位移单位为 cm。 */
USTRUCT(BlueprintType)
struct FDeliveryRiderSettings
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rider", meta=(ClampMin="0", ClampMax="15"))
	float WindLean = 5;
	/** 持续行驶时的上身风摆幅度；不依赖玩家转向或路面颠簸。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rider", meta=(ClampMin="0", ClampMax="20"))
	float WindSway = 14;
	/** 迎面风造成的头部后仰，不受手脚 IK 的可达性缩幅影响。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rider", meta=(ClampMin="0", ClampMax="25"))
	float HeadWindSway = 18;
	/** 搞笑风摆总倍率；独立新字段使旧蓝图保存的基础幅度也能受益。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rider", meta=(ClampMin="0", ClampMax="2.5"))
	float WindExaggeration = 1.7f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rider", meta=(ClampMin="0", ClampMax="20"))
	float InertiaLean = 10;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rider", meta=(ClampMin="0", ClampMax="20"))
	float TurnLean = 9;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rider", meta=(ClampMin="0", ClampMax="25"))
	float ImpactLean = 16;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rider", meta=(ClampMin="0", ClampMax="5"))
	float BumpHeight = 2;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rider", meta=(ClampMin="1", ClampMax="5"))
	float ResponseFrequency = 2.6f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rider", meta=(ClampMin="0.4", ClampMax="1"))
	float Damping = 0.65f;
};

namespace DeliveryRiderPose
{
// 每帧从坐姿参考姿势重新计算，不能在上一帧姿势上累加。
struct FPoseOffset
{
	float Pitch = 0, Roll = 0, Yaw = 0, Bounce = 0;
	float HeadRoll = 0, HeadPitch = 0;
	// 允许手短暂离开握把的最大距离（cm），零值保留严格贴合求解。
	float HandSlack = 0;
};

struct FSpring
{
	float Value = 0, Velocity = 0;
	void Step(float Target, float DeltaTime, float Frequency, float Damping, float Limit);
};

struct FLimb { int32 Root = INDEX_NONE, Mid = INDEX_NONE, End = INDEX_NONE; };

/** 无场景依赖的姿态求解器。输入坐姿骨架和接触点，输出每根骨骼的组件空间变换。 */
struct FRig
{
	TArray<FTransform> Reference;
	TArray<int32> Parents;
	FLimb Limbs[4]; // 左手、右手、左脚、右脚
	int32 Hips = INDEX_NONE, Spine = INDEX_NONE, Chest = INDEX_NONE, Neck = INDEX_NONE;
	bool Initialize(const FReferenceSkeleton& Skeleton);
	bool IsReady() const;
	void Solve(const FPoseOffset& Offset, const FVector& Forward, const FVector& Right,
		const FTransform* Contacts, TArray<FTransform>& OutPose) const;
private:
	void RotateBranch(TArray<FTransform>& Pose, int32 Bone, const FQuat& Rotation) const;
	void BuildBody(const FPoseOffset& Offset, float Weight, const FVector& Forward,
		const FVector& Right, TArray<FTransform>& Pose) const;
	bool CanFeetReach(const TArray<FTransform>& Pose, const FTransform* Contacts) const;
	void SolveLimb(TArray<FTransform>& Pose, const FLimb& Limb, const FTransform& Contact) const;
};
}
