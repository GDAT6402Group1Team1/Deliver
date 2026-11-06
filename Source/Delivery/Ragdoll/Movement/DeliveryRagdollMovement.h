#pragma once

#include "CoreMinimal.h"

/** 一只脚的迈步状态；Alpha 小于 1 表示正在摆动，等于 1 表示落在地上做支撑 */
struct FDeliveryRagdollFoot
{
	/** 位置电机拉的刚体；没有脚刚体时是小腿 */
	FName Bone;
	/** 用来量贴地的骨骼，通常是脚尖；可以没有刚体 */
	FName SoleBone;
	FName Control;
	/** 抬脚瞬间的脚质心，摆动曲线的起点 */
	FVector Start = FVector::ZeroVector;
	/** 这一步要落到的世界位置 */
	FVector Target = FVector::ZeroVector;
	FVector SideAxis = FVector::RightVector;
	FQuat ReferenceRotation = FQuat::Identity;
	FQuat TargetRotation = FQuat::Identity;
	/** 刚体质心比脚底高出多少；落点按这个抬，让脚底着地，而不是把小腿质心按到地面 */
	float GroundOffset = 0.0f;
	/** 左脚为负、右脚为正，或按启动时相对髋的左右决定 */
	float SideSign = 1.0f;
	float Alpha = 1.0f;
	float Elapsed = 0.0f;
};


// 保存步态运行数据，不创建组件，也不单独 Tick
struct FDeliveryRagdollMovement
{
	// 玩家输入与移动目标
	FVector2D MoveInput = FVector2D::ZeroVector;
	FVector LastWishDirection = FVector::ForwardVector;
	FVector SmoothedBalanceOffset = FVector::ZeroVector;
	FVector SmoothedMoveLead = FVector::ZeroVector;
	// 本帧髋部目标，脚步规划使用同一位置
	FVector PlannedPelvisTarget = FVector::ZeroVector;
	// 地面与身体平滑缓存
	FVector CurrentGroundNormal = FVector::UpVector;
	FVector SmoothedGroundPoint = FVector::ZeroVector;
	bool bPelvisAirborne = false;
	FVector WishOnSlope = FVector::ZeroVector;
	float SmoothedAccelerationAlpha = 0.0f;
	float AccelerationLeanRemaining = 0.0f;
	// 停步时保留支撑位置，避免站立目标跟随身体漂移
	FVector StandAnchor = FVector::ZeroVector;
	bool bStandAnchored = false;
	bool bHadMoveWishLastTick = false;
	// 左右脚步及跳跃进度
	FDeliveryRagdollFoot LeftFoot;
	FDeliveryRagdollFoot RightFoot;
	float JumpElapsed = 0.0f;
	float JumpOffset = 0.0f;
	float StartupPlantRemaining = 0.0f;
	float LastMoveInputTime = 0.0f;
	// 停步补位
	bool bWasMoving = false;
	bool bPendingStopRecovery = false;

	bool bStepLeftNext = true;
	bool bJumping = false;

    // 网络位置校正时同步移动步态缓存，保留脚步进度和相对距离
    void Translate(const FVector& Offset)
    {
        LeftFoot.Start += Offset;
        LeftFoot.Target += Offset;
        RightFoot.Start += Offset;
        RightFoot.Target += Offset;
        PlannedPelvisTarget += Offset;
        SmoothedGroundPoint += Offset;
        StandAnchor += Offset;
    }

    // 停步目标跟随小幅网络校正，避免站立电机把身体拉回去
    void CorrectStandAnchor(const FVector& Error, float Alpha)
    {
        if (bStandAnchored)
            StandAnchor += FVector::VectorPlaneProject(Error * Alpha, CurrentGroundNormal);
    }
};
