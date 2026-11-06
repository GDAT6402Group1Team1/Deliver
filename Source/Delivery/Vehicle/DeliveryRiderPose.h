#pragma once

#include "CoreMinimal.h"
#include "ReferenceSkeleton.h"
#include "DeliveryRiderPose.generated.h"

/** 只复制骑手表现需要的输入；车的移动和碰撞仍由摩托车自身计算 */
USTRUCT()

struct FDeliveryRiderMotion
{
	GENERATED_BODY()
	// 车速除以最高速度，负数表示倒车
	UPROPERTY() float Speed = 0;
	// 加减速程度，正数加速，负数减速，范围 -1～1
	UPROPERTY() float Acceleration = 0;
	UPROPERTY() float Steer = 0;
	// 竖直速度变化换算出的颠簸强度，范围 -1～1
	UPROPERTY() float Bump = 0;
	// 撞击在车身前后、左右方向上的分量
	UPROPERTY() FVector2D Impact = FVector2D::ZeroVector;
};

/** 一组可在摩托车蓝图里调的表现参数；角度单位为度，位移单位为 cm */
USTRUCT(BlueprintType)

struct FDeliveryRiderSettings
{
	GENERATED_BODY()
	// 迎风与惯性表现
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider", meta = (ClampMin = "0", ClampMax = "15"))
	float WindLean = 5;
	/** 持续行驶时的上身风摆幅度；不依赖玩家转向或路面颠簸 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider", meta = (ClampMin = "0", ClampMax = "20"))
	float WindSway = 14;
	/** 迎面风造成的头部后仰，不受手脚 IK 的可达性缩幅影响 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider", meta = (ClampMin = "0", ClampMax = "25"))
	float HeadWindSway = 18;
	/** 放大身体和头部的风摆，零值关闭这部分风摆 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider", meta = (ClampMin = "0", ClampMax = "2.5"))
	float WindExaggeration = 1.7f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider", meta = (ClampMin = "0", ClampMax = "20"))
	float InertiaLean = 10;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider", meta = (ClampMin = "0", ClampMax = "20"))
	float TurnLean = 9;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider", meta = (ClampMin = "0", ClampMax = "25"))
	float ImpactLean = 16;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider", meta = (ClampMin = "0", ClampMax = "5"))
	float BumpHeight = 2;
	// 姿态响应
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider", meta = (ClampMin = "1", ClampMax = "5"))
	float ResponseFrequency = 2.6f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rider", meta = (ClampMin = "0.4", ClampMax = "1"))
	float Damping = 0.65f;
};

namespace DeliveryRiderPose
{
// 每帧从坐姿参考姿势重新计算，不能在上一帧姿势上累加
struct FPoseOffset
{
	// 身体前后倾、左右倾、转身角度，以及上下位移
	float Pitch = 0, Roll = 0, Yaw = 0, Bounce = 0;
	float HeadRoll = 0, HeadPitch = 0;
	// 上身补偿时允许的手部距离容差，单位厘米，不会拉长手臂
	float HandSlack = 0;
};

// 数值弹簧，只平滑动画参数，不创建物理约束
struct FSpring
{
	// 当前偏移及其变化速度，保存到下一帧以产生连续跟随和回弹
	float Value = 0, Velocity = 0;
	// 向 Target 靠近，Frequency 控制响应快慢，Damping 控制回弹，Limit 限制幅度
	void Step(float Target, float DeltaTime, float Frequency, float Damping, float Limit);
};

struct FLimb
{
	// 手臂是肩、肘、手，腿是髋、膝、脚，这里保存骨骼编号
	int32 Root = INDEX_NONE, Mid = INDEX_NONE, End = INDEX_NONE;
};

/** 无场景依赖的姿态求解器；输入坐姿骨架和接触点，输出每根骨骼的组件空间变换 */
struct FRig
{
	// 原始坐姿已换算到模型坐标，求解时复制它，不直接修改
	TArray<FTransform> Reference;
	TArray<int32> Parents;
	FLimb Limbs[4]; // 左手、右手、左脚、右脚
	int32 Hips = INDEX_NONE, Spine = INDEX_NONE, Chest = INDEX_NONE, Neck = INDEX_NONE;
	// 骨骼准备与姿势求解
	bool Initialize(const FReferenceSkeleton& Skeleton);
	bool IsReady() const;
	// Contacts 按左手、右手、左脚、右脚排列，OutPose 输出模型坐标下的骨骼变换
	void Solve(const FPoseOffset& Offset, const FVector& Forward, const FVector& Right,
	           const FTransform* Contacts, TArray<FTransform>& OutPose) const;

  private:
	// 身体调整与四肢接触计算
	void RotateBranch(TArray<FTransform>& Pose, int32 Bone, const FQuat& Rotation) const;
	void BuildBody(const FPoseOffset& Offset, float Weight, const FVector& Forward, const FVector& Right,
	               TArray<FTransform>& Pose) const;
	bool CanFeetReach(const TArray<FTransform>& Pose, const FTransform* Contacts) const;
	void SolveLimb(TArray<FTransform>& Pose, const FLimb& Limb, const FTransform& Contact) const;
};
}
