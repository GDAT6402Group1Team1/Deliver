// 阅读顺序：Create 建立手臂电机，Update 用纯拳路计算目标，再把目标旋转交给电机。
// 物理资产补全单独放在 DeliveryBoxingPhysicsAsset.cpp，游戏中的每帧动作在这里。
#include "DeliveryBoxingPose.h"
#include "DeliveryPunchTrajectory.h"
#include "Components/SkeletalMeshComponent.h"
#include "PhysicsControlComponent.h"
#include "Delivery.h"

using DeliveryPunchTrajectory::FArmReach;
using DeliveryPunchTrajectory::MakeReach;
using DeliveryPunchTrajectory::BlendReach;

void FDeliveryBoxingPose::Create(USkeletalMeshComponent* Mesh, UPhysicsControlComponent* Controls,
	const FDeliveryArmPoseSettings& InSettings)
{
	// 每只手臂分别记下“刚进游戏时”的骨骼方向与长度；后续只改变目标姿势，
	// 不在出拳时改骨骼长度。每段骨骼都需要一个可模拟物理的刚体和一个旋转电机。
	Settings = InSettings;
	SettleTime = 0;
	ReferenceYaw = Mesh->GetOwner()->GetActorRotation().Yaw;
	for (int32 Side = 0; Side < 2; ++Side)
	{
		FArm& Arm = Arms[Side];
		Arm = FArm();
		const FString Prefix = Side == 0 ? TEXT("Left") : TEXT("Right");
		Arm.Upper = FName(*(Prefix + TEXT("Arm")));
		Arm.Lower = FName(*(Prefix + TEXT("ForeArm")));
		Arm.Hand = FName(*(Prefix + TEXT("Hand")));
		if (!Mesh->GetBodyInstance(Arm.Upper) || !Mesh->GetBodyInstance(Arm.Lower) || !Mesh->GetBodyInstance(Arm.Hand))
		{
			// 没有刚体就没有肌肉，这条手臂只会跟着身体甩。物理资产缺件时最先看这条。
			UE_LOG(LogDelivery, Warning, TEXT("Boxing: %s arm has no physics bodies"), *Prefix);
			continue;
		}
		Arm.UpperReference = Mesh->GetBoneQuaternion(Arm.Upper);
		Arm.LowerReference = Mesh->GetBoneQuaternion(Arm.Lower);
		Arm.HandReference = Mesh->GetBoneQuaternion(Arm.Hand);
		Arm.UpperDirection = Mesh->GetBoneLocation(Arm.Lower) - Mesh->GetBoneLocation(Arm.Upper);
		Arm.LowerDirection = Mesh->GetBoneLocation(Arm.Hand) - Mesh->GetBoneLocation(Arm.Lower);
		Arm.UpperLength = Arm.UpperDirection.Size();
		Arm.LowerLength = Arm.LowerDirection.Size();
		if (FMath::Min(Arm.UpperLength, Arm.LowerLength) < 1) continue;
		FPhysicsControlData Data;
		Data.AngularStrength = Settings.RestStrength;
		Data.AngularDampingRatio = Settings.DampingRatio;
		Data.bUseSkeletalAnimation = false;
		Data.bUseAccelerationDriveMode = true;
		Data.bOnlyControlChildObject = true;
		const auto CreateControl = [&](FName Bone, FQuat Rotation)
		{
			FPhysicsControlTarget Target;
			Target.TargetOrientation = Rotation.Rotator();
			return Controls->CreateControl(nullptr, NAME_None, Mesh, Bone, Data, Target, TEXT("Boxing"));
		};
		Arm.UpperControl = CreateControl(Arm.Upper, Arm.UpperReference);
		Arm.LowerControl = CreateControl(Arm.Lower, Arm.LowerReference);
		Arm.HandControl = CreateControl(Arm.Hand, Arm.HandReference);
		Arm.bReady = !Arm.UpperControl.IsNone() && !Arm.LowerControl.IsNone() && !Arm.HandControl.IsNone();
	}
}

FVector FDeliveryBoxingPose::SolveElbow(const FVector& Shoulder, const FVector& Hand,
	const FVector& Pole, float UpperLength, float LowerLength)
{
	// 已知肩、手、上臂长和前臂长，求肘的位置。Along 是肘沿肩→手方向
	// 前进的距离；Height 是肘离这条直线的距离；Pole 决定肘向哪一侧弯。
	// 把手的距离夹在可达范围内，避免完全伸直时出现无解或数值抖动。
	const FVector Direction = (Hand - Shoulder).GetSafeNormal();
	const float Distance = FMath::Clamp(FVector::Distance(Hand, Shoulder),
		FMath::Abs(UpperLength - LowerLength) + 0.01f, UpperLength + LowerLength - 0.01f);
	const float Along = (UpperLength * UpperLength - LowerLength * LowerLength + Distance * Distance) / (2 * Distance);
	const float Height = FMath::Sqrt(FMath::Max(0.f, UpperLength * UpperLength - Along * Along));
	FVector Bend = FVector::VectorPlaneProject(Pole, Direction).GetSafeNormal();
	if (Bend.IsNearlyZero()) Bend = FVector::CrossProduct(Direction, FVector::RightVector).GetSafeNormal();
	return Shoulder + Direction * Along + Bend * Height;
}

void FDeliveryBoxingPose::Update(USkeletalMeshComponent* Mesh, UPhysicsControlComponent* Controls,
	float FacingYaw, float DeltaTime, int32 PunchArm, bool bReleased, FVector PunchDirection,
	const FVector* GrabGoals, uint8 GrabMask)
{
	// 同一条更新路径同时处理自然垂手、向后蓄力、向前出拳、收拳和抓取。
	// 先算手在肩附近的目标，再求肘与三段骨骼的朝向，最后交给物理电机。
	SettleTime += DeltaTime;
	const float Settle = FMath::SmoothStep(0.f, 0.5f, SettleTime);
	const FQuat Yaw(FVector::UpVector, FMath::DegreesToRadians(FacingYaw - ReferenceYaw));
	const FVector Forward = FRotator(0, FacingYaw, 0).Vector();
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward);
	// 拳头沿瞄准方向走，但不能离身体正前方太远，不然目标点落在肩膀的活动范围之外。
	FVector PunchForward = Forward;
	const FVector FlatAim = PunchDirection.GetSafeNormal2D();
	if (!FlatAim.IsNearlyZero())
	{
		const float Deviation = FMath::Clamp(FMath::FindDeltaAngleDegrees(FacingYaw, FlatAim.Rotation().Yaw),
			-Settings.PunchYawLimit, Settings.PunchYawLimit);
		PunchForward = FRotator(0, FacingYaw + Deviation, 0).Vector();
	}
	for (int32 Side = 0; Side < 2; ++Side)
	{
		FArm& Arm = Arms[Side];
		if (!Arm.bReady) continue;
		// 只有出拳那只手离开 A 姿势，另一只手继续垂着。
		const bool bWindup = Side == PunchArm;
		const bool bStrike = bWindup && bReleased;
		Arm.Windup = FMath::FInterpConstantTo(Arm.Windup, bWindup ? 1.f : 0.f, DeltaTime,
			bWindup ? Settings.WindupSpeed : Settings.WindupReleaseSpeed);
		Arm.Extension = FMath::FInterpConstantTo(Arm.Extension, bStrike ? 1.f : 0.f, DeltaTime,
			bStrike ? Settings.PunchExtendSpeed : Settings.PunchRetractSpeed);
		const float WindupAlpha = FMath::SmoothStep(0.f, 1.f, Arm.Windup);
		// 出拳前段快速向前，末端减速；收拳则两端都放缓。
		// 若出拳起点也缓入，看起来会像慢慢把对方推开，而不是挥拳。
		const float StrikeAlpha = bStrike
			? DeliveryPunchTrajectory::StrikeProgress(Arm.Extension)
			: DeliveryPunchTrajectory::RetractProgress(Arm.Extension);
		const FVector Outward = Right * (Side == 0 ? -1.f : 1.f);
		const float Length = Arm.UpperLength + Arm.LowerLength;
		const float MaxReach = Length * 0.99f;

		// 参考姿态：自己算的 A 姿势。手臂自然下垂、略微外张，肘部鼓向身后。
		const float Spread = FMath::DegreesToRadians(Settings.RestSpreadAngle);
		FArmReach Reach = MakeReach(
			(-FVector::UpVector * FMath::Cos(Spread) + Outward * FMath::Sin(Spread)
				+ Forward * FMath::Tan(FMath::DegreesToRadians(Settings.RestForwardAngle))) * Length * Settings.RestReach,
			-Forward + Outward * 0.35f, MaxReach);

		// 先沿瞄准方向向侧后方收拳，再从此点向前打出。少量外侧距离
		// 让手经过肩旁而非穿过肩关节原点，保持屈肘方向连续。
		const FVector WindupOffset = DeliveryPunchTrajectory::WindupOffset(PunchForward,
			Outward, Length, Settings.WindupBack, Settings.WindupUp, Settings.WindupOutward);
		Reach = BlendReach(Reach, MakeReach(WindupOffset, -FVector::UpVector + Outward * 0.4f, MaxReach),
			WindupAlpha, WindupAlpha);

		// 前送走肩到手的直线：直接插值偏移。BlendReach 先转方向再伸长，方向差一丁点就是弧。
		const FVector PunchOffset = DeliveryPunchTrajectory::StrikeOffset(PunchForward,
			Outward, Length, Settings.PunchReach, Settings.PunchInward, Settings.PunchDrop);
		Reach = MakeReach(
			FMath::Lerp(Reach.Direction * Reach.Distance, PunchOffset, StrikeAlpha),
			FMath::Lerp(Reach.Pole, (-FVector::UpVector * 0.85f + Outward * 0.3f).GetSafeNormal(), StrikeAlpha),
			MaxReach);

		const FVector Shoulder = Mesh->GetBoneLocation(Arm.Upper);
		const bool bGrab = GrabGoals && (GrabMask & (1 << Side));
		if (bGrab)
		{
			// 仍由这套唯一的手臂电机求解；抓取时只改目标，不叠加第二套位置电机。
			Reach = MakeReach(GrabGoals[Side] - Shoulder,
				-FVector::UpVector + Outward * 0.4f, MaxReach);
		}
		const FVector Hand = Shoulder + Reach.Direction * Reach.Distance;
		// 目标“拳头在哪里”还不能直接驱动整条手臂：先求肘，再把肩→肘、
		// 肘→手两个方向分别换成上臂、前臂的目标旋转。
		const FVector Elbow = SolveElbow(Shoulder, Hand, Reach.Pole, Arm.UpperLength, Arm.LowerLength);
		const FQuat UpperBase = Yaw * Arm.UpperReference;
		const FQuat LowerBase = Yaw * Arm.LowerReference;
		const FQuat Upper = FQuat::FindBetweenNormals(Yaw.RotateVector(Arm.UpperDirection).GetSafeNormal(),
			(Elbow - Shoulder).GetSafeNormal()) * UpperBase;
		const FQuat Lower = FQuat::FindBetweenNormals(Yaw.RotateVector(Arm.LowerDirection).GetSafeNormal(),
			(Hand - Elbow).GetSafeNormal()) * LowerBase;
		const FQuat Wrist = Lower * Arm.LowerReference.Inverse() * Arm.HandReference;

		// 站着可以松，收拳要绷住，前送必须硬，否则拳头追不上目标点。
		const float Strength = FMath::Lerp(
			FMath::Lerp(Settings.RestStrength, Settings.WindupStrength, WindupAlpha),
			Settings.PunchStrength, StrikeAlpha);
		const float AppliedStrength = bGrab ? Settings.GrabStrength : Strength;
		if (!FMath::IsNearlyEqual(Arm.AppliedStrength, AppliedStrength, 0.1f))
		{
			Arm.AppliedStrength = AppliedStrength;
			const TPair<FName, float> Links[] = {
				{ Arm.UpperControl, Settings.UpperArmStrengthScale },
				{ Arm.LowerControl, 1.f },
				{ Arm.HandControl, Settings.HandStrengthScale } };
			for (const TPair<FName, float>& Link : Links)
			{
				Controls->SetControlAngularData(Link.Key, AppliedStrength * Link.Value, Settings.DampingRatio, 0, 0, true, true, false);
			}
		}
		// 写给实际的三个控制器（不是控制器集合）；DeltaTime 让电机知道目标转动速度，
		// 快速出拳时能及时跟上。实际手臂依旧由刚体模拟，不会直接传送到目标姿势。
		Controls->SetControlTargetOrientation(Arm.UpperControl, FQuat::Slerp(UpperBase, Upper, Settle).Rotator(), DeltaTime, true, false, true, false);
		Controls->SetControlTargetOrientation(Arm.LowerControl, FQuat::Slerp(LowerBase, Lower, Settle).Rotator(), DeltaTime, true, false, true, false);
		Controls->SetControlTargetOrientation(Arm.HandControl, FQuat::Slerp(Yaw * Arm.HandReference, Wrist, Settle).Rotator(), DeltaTime, true, false, true, false);
	}
}
