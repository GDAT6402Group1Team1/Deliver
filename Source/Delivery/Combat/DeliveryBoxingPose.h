#pragma once

#include "Combat/DeliveryAnimationArmPose.h"

#include "CoreMinimal.h"
#include "Combat/DeliveryCombatTypes.h"

class USkeletalMeshComponent;
class UPhysicsControlComponent;

/** 上臂目标使用世界坐标，肘部和手腕目标使用各自父刚体的坐标 */
struct FDeliveryBoxingPose
{
	struct FArm
	{
		// 骨骼与控制名称
		FName Upper, Lower, Hand;
		FName UpperControl, LowerControl, HandControl;
		// 参考姿势与手臂尺寸
		FQuat UpperReference, LowerReference, HandReference;
		FVector UpperDirection, LowerDirection;
		float UpperLength = 0, LowerLength = 0;
		// 当前动作进度
		/** 0 是站立走路的 A 姿势，1 是拳头拉到身后的收拳；只有出拳那只手才往 1 走 */
		float Windup = 0;
		float Extension = 0;
		FVector PunchForward = FVector::ZeroVector;
		// 保存收拳开始时相对身体朝向的关节目标
		// 蓄力和挥拳期间不累积这些旋转，避免手臂不断扭转
		FQuat UpperTransport = FQuat::Identity, LowerTransport = FQuat::Identity;
		FQuat UpperRecovery = FQuat::Identity, LowerRecovery = FQuat::Identity;
		bool bWasPunching = false;
		float AppliedStrength = -1;
		bool bReady = false;
	};

	FArm Arms[2];
	FDeliveryArmPoseSettings Settings;
	float ReferenceYaw = 0;
	float SettleTime = 0;

	// 物理准备与姿势更新
	/** 物理资产缺手或小臂刚体时当场补上，否则只有一侧能出拳 */
	static void CompletePhysicsAsset(USkeletalMeshComponent* Mesh,
	                                 const FDeliveryArmPoseSettings& Settings = FDeliveryArmPoseSettings());
	void Create(USkeletalMeshComponent* Mesh, UPhysicsControlComponent* Controls,
	            const FDeliveryArmPoseSettings& InSettings = FDeliveryArmPoseSettings());
	void Update(USkeletalMeshComponent* Mesh, UPhysicsControlComponent* Controls, float FacingYaw,
	            float DeltaTime, int32 PunchArm, bool bReleased, FVector PunchDirection = FVector::ZeroVector,
	            const FVector* GrabGoals = nullptr, uint8 GrabMask = 0,
	            const FDeliveryAnimationArmPose* AnimationPose = nullptr);

	bool IsReady(int32 Side) const
	{
		return Side >= 0 && Side < 2 && Arms[Side].bReady;
	}

	bool HasPendingPunchRecovery() const
	{
		for (const FArm& Arm : Arms)
			if (Arm.bReady && (Arm.Windup > 0.f || Arm.Extension > 0.f))
				return true;
		return false;
	}

	// 手臂几何计算
	static FVector SolveElbow(const FVector& Shoulder, const FVector& Hand, const FVector& Pole,
	                          float UpperLength, float LowerLength);
};
