#include "DeliveryBoxingPose.h"
#include "Components/SkeletalMeshComponent.h"
#include "PhysicsControlComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SphylElem.h"
#include "Delivery.h"

namespace
{
	/** 关节限位是否已经放到出拳需要的范围。够宽就不用再复制一份物理资产。 */
	bool JointIsOpenedUp(const FConstraintInstance& Joint, float SwingLimit, float TwistLimit)
	{
		const bool bSwingOpen = Joint.GetAngularSwing1Motion() == ACM_Free
			|| (Joint.GetAngularSwing1Motion() == ACM_Limited && Joint.GetAngularSwing1Limit() >= SwingLimit - 0.5f);
		const bool bTwistOpen = Joint.GetAngularTwistMotion() == ACM_Free
			|| (Joint.GetAngularTwistMotion() == ACM_Limited && Joint.GetAngularTwistLimit() >= TwistLimit - 0.5f);
		return bSwingOpen && bTwistOpen;
	}

	void OpenUpJoint(FConstraintInstance& Joint, float SwingLimit, float TwistLimit)
	{
		Joint.SetAngularSwing1Limit(ACM_Limited, SwingLimit);
		Joint.SetAngularSwing2Limit(ACM_Limited, SwingLimit);
		Joint.SetAngularTwistLimit(ACM_Limited, TwistLimit);
	}

	/** 一个姿势：肩到手的方向、距离，加上肘部要鼓向哪边。 */
	struct FArmReach
	{
		FVector Direction;
		float Distance;
		FVector Pole;
	};

	FArmReach MakeReach(const FVector& Offset, const FVector& Pole, float MaxDistance)
	{
		return { Offset.GetSafeNormal(), FMath::Min(float(Offset.Size()), MaxDistance), Pole.GetSafeNormal() };
	}

	/**
	 * 先转方向再插值长度。直接插值手的位置会让手贴着肋骨划过去。
	 * DistanceAlpha 可以比 Alpha 慢：手臂在方向还没转到位时保持收着，
	 * 划出去的弧线半径就小，最后才伸直。
	 */
	FArmReach BlendReach(const FArmReach& From, const FArmReach& To, float Alpha, float DistanceAlpha)
	{
		FArmReach Out;
		Out.Direction = FMath::Lerp(From.Direction, To.Direction, Alpha).GetSafeNormal();
		if (Out.Direction.IsNearlyZero()) Out.Direction = To.Direction;
		Out.Distance = FMath::Lerp(From.Distance, To.Distance, DistanceAlpha);
		Out.Pole = FMath::Lerp(From.Pole, To.Pole, Alpha).GetSafeNormal();
		if (Out.Pole.IsNearlyZero()) Out.Pole = To.Pole;
		return Out;
	}
}

void FDeliveryBoxingPose::CompletePhysicsAsset(USkeletalMeshComponent* Mesh, const FDeliveryArmPoseSettings& Settings)
{
	UPhysicsAsset* Source = Mesh->GetPhysicsAsset();
	if (!Source) return;
	UPhysicsAsset* Asset = nullptr;
	for (const FString Side : { FString(TEXT("Left")), FString(TEXT("Right")) })
	{
		const FName Upper(*(Side + TEXT("Arm"))), Lower(*(Side + TEXT("ForeArm"))), Hand(*(Side + TEXT("Hand")));
		if (Mesh->GetBoneIndex(Lower) == INDEX_NONE
			|| Source->FindBodyIndex(Upper) == INDEX_NONE || Source->FindBodyIndex(Hand) == INDEX_NONE) continue;
		const bool bNeedsForearm = Source->FindBodyIndex(Lower) == INDEX_NONE;
		bool bNeedsJointRange = false;
		for (const UPhysicsConstraintTemplate* Template : Source->ConstraintSetup)
		{
			const FConstraintInstance& Joint = Template->DefaultInstance;
			if (Joint.ConstraintBone1 == Upper)
			{
				bNeedsJointRange |= !JointIsOpenedUp(Joint, Settings.ShoulderSwingLimit, Settings.ShoulderTwistLimit);
			}
			else if (Joint.ConstraintBone1 == Lower)
			{
				bNeedsJointRange |= !JointIsOpenedUp(Joint, Settings.ElbowSwingLimit, Settings.ElbowTwistLimit);
			}
		}
		if (!bNeedsForearm && !bNeedsJointRange) continue;
		if (!Asset)
		{
			Asset = DuplicateObject<UPhysicsAsset>(Source, Mesh);
			Asset->SetFlags(RF_Transient);
		}

		// 肩和肘按出拳需要的活动范围放开。默认锥角只有几十度，上臂抬不到肩前，
		// 收拳会停在肚子前面，出拳也只剩小臂在折。
		bool bShoulderFound = false;
		for (UPhysicsConstraintTemplate* Template : Asset->ConstraintSetup)
		{
			FConstraintInstance& Joint = Template->DefaultInstance;
			if (Joint.ConstraintBone1 == Upper)
			{
				OpenUpJoint(Joint, Settings.ShoulderSwingLimit, Settings.ShoulderTwistLimit);
				bShoulderFound = true;
			}
			else if (Joint.ConstraintBone1 == Lower)
			{
				OpenUpJoint(Joint, Settings.ElbowSwingLimit, Settings.ElbowTwistLimit);
			}
		}
		if (!bShoulderFound)
		{
			// 没找到肩关节就没法放开限位，出拳会被物理资产原本的锥角卡住。
			UE_LOG(LogDelivery, Warning, TEXT("Boxing: no %s shoulder joint owned by %s"), *Side, *Upper.ToString());
		}
		if (!bNeedsForearm) continue;

		const FTransform UpperTM = Mesh->GetSocketTransform(Upper);
		const FTransform LowerTM = Mesh->GetSocketTransform(Lower);
		const FTransform HandTM = Mesh->GetSocketTransform(Hand);
		const FVector LocalEnd = LowerTM.InverseTransformPosition(HandTM.GetLocation());
		USkeletalBodySetup* Body = NewObject<USkeletalBodySetup>(Asset, NAME_None, RF_Transient);
		Body->BoneName = Lower;
		Body->PhysicsType = PhysType_Default;
		FKSphylElem Capsule;
		Capsule.Center = LocalEnd * 0.5f;
		Capsule.Radius = FMath::Clamp(LocalEnd.Size() * 0.12f, 2.0f, 4.0f);
		Capsule.Length = FMath::Max(1.0f, LocalEnd.Size() - 2 * Capsule.Radius);
		Capsule.Rotation = FQuat::FindBetweenNormals(FVector::UpVector, LocalEnd.GetSafeNormal()).Rotator();
		Body->AggGeom.SphylElems.Add(Capsule);
		Body->CreatePhysicsMeshes();
		Asset->SkeletalBodySetups.Add(Body);
		Asset->UpdateBodySetupIndexMap();

		// Replace the old hand-to-upper-arm joint with a wrist, then add an elbow.
		bool bWristFound = false;
		for (UPhysicsConstraintTemplate* Template : Asset->ConstraintSetup)
		{
			FConstraintInstance& Joint = Template->DefaultInstance;
			const bool bHandFirst = Joint.ConstraintBone1 == Hand && Joint.ConstraintBone2 == Upper;
			const bool bHandSecond = Joint.ConstraintBone2 == Hand && Joint.ConstraintBone1 == Upper;
			if (!bHandFirst && !bHandSecond) continue;
			Joint.ConstraintBone1 = Hand;
			Joint.ConstraintBone2 = Lower;
			const FTransform WorldFrame(HandTM.GetRotation(), HandTM.GetLocation());
			Joint.SetRefFrame(EConstraintFrame::Frame1, WorldFrame.GetRelativeTransform(HandTM));
			Joint.SetRefFrame(EConstraintFrame::Frame2, WorldFrame.GetRelativeTransform(LowerTM));
			Joint.SetAngularSwing1Limit(ACM_Limited, 15);
			Joint.SetAngularSwing2Limit(ACM_Limited, 15);
			Joint.SetAngularTwistLimit(ACM_Limited, 15);
			Joint.SetDisableCollision(true);
			bWristFound = true;
		}
		if (!bWristFound)
		{
			UE_LOG(LogDelivery, Warning, TEXT("Boxing: missing %s wrist connection"), *Side);
		}
		UPhysicsConstraintTemplate* Elbow = NewObject<UPhysicsConstraintTemplate>(Asset, NAME_None, RF_Transient);
		FConstraintInstance& Joint = Elbow->DefaultInstance;
		Joint.JointName = Lower;
		Joint.ConstraintBone1 = Lower;
		Joint.ConstraintBone2 = Upper;
		const FTransform WorldFrame(LowerTM.GetRotation(), LowerTM.GetLocation());
		Joint.SetRefFrame(EConstraintFrame::Frame1, WorldFrame.GetRelativeTransform(LowerTM));
		Joint.SetRefFrame(EConstraintFrame::Frame2, WorldFrame.GetRelativeTransform(UpperTM));
		Joint.SetLinearLimits(LCM_Locked, LCM_Locked, LCM_Locked, 0);
		// Orientation is driven from the solved bend plane, not a guessed local hinge axis.
		OpenUpJoint(Joint, Settings.ElbowSwingLimit, Settings.ElbowTwistLimit);
		Joint.SetDisableCollision(true);
		Asset->ConstraintSetup.Add(Elbow);
		Asset->DisableCollision(Asset->FindBodyIndex(Lower), Asset->FindBodyIndex(Upper));
		Asset->DisableCollision(Asset->FindBodyIndex(Lower), Asset->FindBodyIndex(Hand));
		UE_LOG(LogDelivery, Log, TEXT("Boxing: added runtime %s elbow and forearm"), *Side);
	}
	if (Asset)
	{
		Asset->UpdateBoundsBodiesArray();
		Mesh->SetPhysicsAsset(Asset, true);
	}
}

void FDeliveryBoxingPose::Create(USkeletalMeshComponent* Mesh, UPhysicsControlComponent* Controls,
	const FDeliveryArmPoseSettings& InSettings)
{
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
	float FacingYaw, float DeltaTime, int32 PunchArm, bool bReleased, FVector PunchDirection)
{
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
		// 收拳可以两头都缓，前送不行：缓入等于出手先慢慢加速，那是推不是打。
		// 一出手就给最大速度，末端再减速，收在伸直的位置上。
		const float StrikeAlpha = 1.f - FMath::Square(1.f - Arm.Extension);
		const FVector Outward = Right * (Side == 0 ? -1.f : 1.f);
		const float Length = Arm.UpperLength + Arm.LowerLength;
		const float MaxReach = Length * 0.99f;

		// 参考姿态：自己算的 A 姿势。手臂自然下垂、略微外张，肘部鼓向身后。
		const float Spread = FMath::DegreesToRadians(Settings.RestSpreadAngle);
		FArmReach Reach = MakeReach(
			(-FVector::UpVector * FMath::Cos(Spread) + Outward * FMath::Sin(Spread)
				+ Forward * FMath::Tan(FMath::DegreesToRadians(Settings.RestForwardAngle))) * Length * Settings.RestReach,
			-Forward + Outward * 0.35f, MaxReach);

		// 收拳和终点都沿瞄准方向，不沿身体正前方。瞄准偏一点时，护架如果还对着身体前方，
		// 前送就得先把方向转过来，拳头会横着划过去。
		const FVector WindupOffset =
			(-PunchForward * Settings.WindupBack + FVector::UpVector * Settings.WindupUp
				+ Outward * Settings.WindupOutward) * Length;
		Reach = BlendReach(Reach, MakeReach(WindupOffset, -FVector::UpVector + Outward * 0.4f, MaxReach),
			WindupAlpha, WindupAlpha);

		// 前送走肩到手的直线：直接插值偏移。BlendReach 先转方向再伸长，方向差一丁点就是弧。
		const FVector PunchOffset =
			(PunchForward * Settings.PunchReach - Outward * Settings.PunchInward
				- FVector::UpVector * Settings.PunchDrop) * Length;
		Reach = MakeReach(
			FMath::Lerp(Reach.Direction * Reach.Distance, PunchOffset, StrikeAlpha),
			FMath::Lerp(Reach.Pole, (-FVector::UpVector * 0.85f + Outward * 0.3f).GetSafeNormal(), StrikeAlpha),
			MaxReach);

		const FVector Shoulder = Mesh->GetBoneLocation(Arm.Upper);
		const FVector Hand = Shoulder + Reach.Direction * Reach.Distance;
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
		if (!FMath::IsNearlyEqual(Arm.AppliedStrength, Strength, 0.1f))
		{
			Arm.AppliedStrength = Strength;
			const TPair<FName, float> Links[] = {
				{ Arm.UpperControl, Settings.UpperArmStrengthScale },
				{ Arm.LowerControl, 1.f },
				{ Arm.HandControl, Settings.HandStrengthScale } };
			for (const TPair<FName, float>& Link : Links)
			{
				Controls->SetControlAngularData(Link.Key, Strength * Link.Value, Settings.DampingRatio, 0, 0, true, true, false);
			}
		}
		// Target the actual controls: the last two flags select controls=true, sets=false.
		// 传 DeltaTime 让电机顺带拿到目标角速度，快动作才不会一直落后。
		Controls->SetControlTargetOrientation(Arm.UpperControl, FQuat::Slerp(UpperBase, Upper, Settle).Rotator(), DeltaTime, true, false, true, false);
		Controls->SetControlTargetOrientation(Arm.LowerControl, FQuat::Slerp(LowerBase, Lower, Settle).Rotator(), DeltaTime, true, false, true, false);
		Controls->SetControlTargetOrientation(Arm.HandControl, FQuat::Slerp(Yaw * Arm.HandReference, Wrist, Settle).Rotator(), DeltaTime, true, false, true, false);
	}
}
