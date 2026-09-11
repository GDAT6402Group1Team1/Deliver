#pragma once

#include "CoreMinimal.h"
#include "Combat/DeliveryCombatTypes.h"

class USkeletalMeshComponent;
class UPhysicsControlComponent;

/** Physical two-link arms. Targets use world space, never the skeleton's non-physical parent. */
struct FDeliveryBoxingPose
{
	struct FArm
	{
		FName Upper, Lower, Hand;
		FName UpperControl, LowerControl, HandControl;
		FQuat UpperReference, LowerReference, HandReference;
		FVector UpperDirection, LowerDirection;
		float UpperLength = 0, LowerLength = 0;
		/** 0 是站立走路的 A 姿势，1 是拳头拉到身后的收拳。只有出拳那只手才往 1 走。 */
		float Windup = 0;
		float Extension = 0;
		float AppliedStrength = -1;
		bool bReady = false;
	};
	FArm Arms[2];
	FDeliveryArmPoseSettings Settings;
	float ReferenceYaw = 0;
	float SettleTime = 0;

	static void CompletePhysicsAsset(USkeletalMeshComponent* Mesh,
		const FDeliveryArmPoseSettings& Settings = FDeliveryArmPoseSettings());
	void Create(USkeletalMeshComponent* Mesh, UPhysicsControlComponent* Controls,
		const FDeliveryArmPoseSettings& InSettings = FDeliveryArmPoseSettings());
	void Update(USkeletalMeshComponent* Mesh, UPhysicsControlComponent* Controls,
		float FacingYaw, float DeltaTime, int32 PunchArm, bool bReleased,
		FVector PunchDirection = FVector::ZeroVector);
	bool IsReady(int32 Side) const { return Side >= 0 && Side < 2 && Arms[Side].bReady; }
	static FVector SolveElbow(const FVector& Shoulder, const FVector& Hand,
		const FVector& Pole, float UpperLength, float LowerLength);
};
