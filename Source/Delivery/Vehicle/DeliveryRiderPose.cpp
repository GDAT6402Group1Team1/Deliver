#include "Vehicle/DeliveryRiderPose.h"

void DeliveryRiderPose::FSpring::Step(float Target, float DeltaTime, float Frequency, float Damping, float Limit)
{
	// 小步积分让低帧率和瞬间卡顿不会把弹簧炸开；上限同时限制幅度与越界速度。
	float Remaining = FMath::Clamp(DeltaTime, 0.f, 0.1f);
	const float Omega = 2.f * PI * FMath::Clamp(Frequency, 1.f, 5.f);
	Target = FMath::Clamp(Target, -Limit, Limit);
	while (Remaining > SMALL_NUMBER)
	{
		const float Dt = FMath::Min(Remaining, 1.f / 120.f);
		Velocity += (Omega * Omega * (Target - Value) - 2.f * FMath::Clamp(Damping, .4f, 1.f) * Omega * Velocity) * Dt;
		Value += Velocity * Dt;
		if (FMath::Abs(Value) > Limit) { Value = FMath::Clamp(Value, -Limit, Limit); Velocity = 0; }
		Remaining -= Dt;
	}
}

bool DeliveryRiderPose::FRig::Initialize(const FReferenceSkeleton& Skeleton)
{
	const auto Find = [&](const TCHAR* Name)
	{
		int32 Index = Skeleton.FindBoneIndex(FName(*FString::Printf(TEXT("mixamorig:%s"), Name)));
		return Index != INDEX_NONE ? Index : Skeleton.FindBoneIndex(FName(Name));
	};
	Hips = Find(TEXT("Hips")); Spine = Find(TEXT("Spine"));
	Chest = Find(TEXT("Spine2")); Neck = Find(TEXT("Neck"));
	Limbs[0] = { Find(TEXT("LeftArm")), Find(TEXT("LeftForeArm")), Find(TEXT("LeftHand")) };
	Limbs[1] = { Find(TEXT("RightArm")), Find(TEXT("RightForeArm")), Find(TEXT("RightHand")) };
	Limbs[2] = { Find(TEXT("LeftUpLeg")), Find(TEXT("LeftLeg")), Find(TEXT("LeftFoot")) };
	Limbs[3] = { Find(TEXT("RightUpLeg")), Find(TEXT("RightLeg")), Find(TEXT("RightFoot")) };
	Reference = Skeleton.GetRefBonePose(); Parents.SetNum(Reference.Num());
	for (int32 I = 0; I < Reference.Num(); ++I)
	{
		Parents[I] = Skeleton.GetParentIndex(I);
		if (Parents[I] != INDEX_NONE) Reference[I] *= Reference[Parents[I]];
	}
	return IsReady();
}

bool DeliveryRiderPose::FRig::IsReady() const
{
	if (Hips == INDEX_NONE || Spine == INDEX_NONE || Chest == INDEX_NONE || Neck == INDEX_NONE) return false;
	for (const FLimb& Limb : Limbs)
		if (Limb.Root == INDEX_NONE || Limb.Mid == INDEX_NONE || Limb.End == INDEX_NONE) return false;
	return true;
}

void DeliveryRiderPose::FRig::RotateBranch(TArray<FTransform>& Pose, int32 Bone, const FQuat& Rotation) const
{
	const FVector Pivot = Pose[Bone].GetLocation();
	for (int32 I = Bone; I < Pose.Num(); ++I)
	{
		int32 Ancestor = I;
		while (Ancestor != INDEX_NONE && Ancestor != Bone) Ancestor = Parents[Ancestor];
		if (Ancestor == INDEX_NONE) continue;
		Pose[I].SetLocation(Pivot + Rotation.RotateVector(Pose[I].GetLocation() - Pivot));
		Pose[I].SetRotation((Rotation * Pose[I].GetRotation()).GetNormalized());
	}
}

void DeliveryRiderPose::FRig::BuildBody(const FPoseOffset& Offset, float Weight, const FVector& Forward,
	const FVector& Right, TArray<FTransform>& Pose) const
{
	Pose = Reference;
	const FVector Up = FVector::CrossProduct(Forward, Right).GetSafeNormal();
	// 座位附近只允许很小的起伏；胸腰承担大部分夸张动作，头稍微反向保持视线。
	for (int32 I = Hips; I < Pose.Num(); ++I)
	{
		int32 Ancestor = I;
		while (Ancestor != INDEX_NONE && Ancestor != Hips) Ancestor = Parents[Ancestor];
		if (Ancestor == Hips) Pose[I].AddToTranslation(Up * Offset.Bounce * Weight);
	}
	const auto Tilt = [&](float Fraction)
	{
		return FQuat(Up, FMath::DegreesToRadians(Offset.Yaw * Fraction * Weight))
			* FQuat(Forward, FMath::DegreesToRadians(Offset.Roll * Fraction * Weight))
			* FQuat(Right, FMath::DegreesToRadians(Offset.Pitch * Fraction * Weight));
	};
	RotateBranch(Pose, Spine, Tilt(.55f));
	RotateBranch(Pose, Chest, Tilt(.45f));
	RotateBranch(Pose, Neck, FQuat(Forward, FMath::DegreesToRadians(-Offset.Roll * .35f * Weight))
		* FQuat(Right, FMath::DegreesToRadians(-Offset.Pitch * .3f * Weight)));
}

bool DeliveryRiderPose::FRig::CanFeetReach(const TArray<FTransform>& Pose, const FTransform* Contacts) const
{
	for (int32 I = 2; I < 4; ++I)
	{
		const FLimb& L = Limbs[I];
		const float A = FVector::Distance(Pose[L.Root].GetLocation(), Pose[L.Mid].GetLocation());
		const float B = FVector::Distance(Pose[L.Mid].GetLocation(), Pose[L.End].GetLocation());
		const float D = FVector::Distance(Pose[L.Root].GetLocation(), Contacts[I].GetLocation());
		if (D > A + B - .01f || D < FMath::Abs(A - B) + .01f) return false;
	}
	return true;
}

void DeliveryRiderPose::FRig::SolveLimb(TArray<FTransform>& Pose, const FLimb& L, const FTransform& Contact) const
{
	const FVector Root = Pose[L.Root].GetLocation();
	const FVector Mid = Pose[L.Mid].GetLocation();
	const FVector End = Pose[L.End].GetLocation();
	const float A = FVector::Distance(Root, Mid), B = FVector::Distance(Mid, End);
	if (FMath::Min(A, B) < .01f) return;
	const FVector Direction = (Contact.GetLocation() - Root).GetSafeNormal();
	const float Distance = FMath::Clamp(float(FVector::Distance(Root, Contact.GetLocation())),
		FMath::Abs(A - B) + .001f, A + B - .001f);
	// 肘/膝向参考坐姿所在的一侧弯，避免直线附近突然翻到反关节方向。
	FVector Bend = FVector::VectorPlaneProject(Mid - Root, Direction).GetSafeNormal();
	if (Bend.IsNearlyZero()) Bend = FVector::VectorPlaneProject(FVector::UpVector, Direction).GetSafeNormal();
	const float Along = (A*A - B*B + Distance*Distance) / (2.f * Distance);
	const FVector NewMid = Root + Direction * Along + Bend * FMath::Sqrt(FMath::Max(0.f, A*A - Along*Along));
	const FVector NewEnd = Root + Direction * Distance;
	RotateBranch(Pose, L.Root, FQuat::FindBetweenNormals((Mid - Root).GetSafeNormal(), (NewMid - Root).GetSafeNormal()));
	RotateBranch(Pose, L.Mid, FQuat::FindBetweenNormals(
		(Pose[L.End].GetLocation() - Pose[L.Mid].GetLocation()).GetSafeNormal(),
		(NewEnd - Pose[L.Mid].GetLocation()).GetSafeNormal()));
	RotateBranch(Pose, L.End, Contact.GetRotation() * Pose[L.End].GetRotation().Inverse());
}

void DeliveryRiderPose::FRig::Solve(const FPoseOffset& Offset, const FVector& Forward, const FVector& Right,
	const FTransform* Contacts, TArray<FTransform>& OutPose) const
{
	if (!IsReady()) { OutPose = Reference; return; }
	BuildBody(Offset, 1.f, Forward, Right, OutPose);
	// 仅用腿部可达性限制起伏。手臂由后面的腰部连续补偿处理；
	// 把手臂也放进全身缩幅判定，会在某侧刚够不到车把时突然将整套摆动归零。
	if (!CanFeetReach(OutPose, Contacts))
	{
		float Low = 0, High = 1;
		for (int32 I = 0; I < 8; ++I)
		{
			const float Mid = (Low + High) * .5f;
			BuildBody(Offset, Mid, Forward, Right, OutPose);
			if (CanFeetReach(OutPose, Contacts)) Low = Mid; else High = Mid;
		}
		BuildBody(Offset, Low, Forward, Right, OutPose);
	}
	// 转向后的外侧车把可能连中立坐姿都够不到。此时仅收小摆动无效：
	// 让腰部朝较远的手适度前倾/转身，把肩送入可达范围，而不是拉长手臂。
	for (int32 Iteration = 0; Iteration < 32; ++Iteration)
	{
		bool bAdjusted = false;
		for (int32 I = 0; I < 2; ++I)
		{
			const FLimb& L = Limbs[I];
			const FVector Shoulder = OutPose[L.Root].GetLocation();
			const FVector Hand = Contacts[I].GetLocation();
			const float Reach = FVector::Distance(Shoulder, OutPose[L.Mid].GetLocation())
				+ FVector::Distance(OutPose[L.Mid].GetLocation(), OutPose[L.End].GetLocation()) - .1f
				+ FMath::Clamp(Offset.HandSlack, 0.f, 5.f);
			if (FVector::Distance(Shoulder, Hand) <= Reach) continue;
			const FVector Pivot = OutPose[Spine].GetLocation();
			const FVector DesiredShoulder = Hand + (Shoulder - Hand).GetSafeNormal() * Reach;
			const FQuat Correction = FQuat::FindBetweenNormals((Shoulder-Pivot).GetSafeNormal(), (DesiredShoulder-Pivot).GetSafeNormal());
			RotateBranch(OutPose, Spine, FQuat::Slerp(FQuat::Identity, Correction, .8f));
			bAdjusted = true;
		}
		if (!bAdjusted) break;
	}
	for (int32 I = 0; I < 4; ++I) SolveLimb(OutPose, Limbs[I], Contacts[I]);
	// 颈部位于手臂链之外，最后叠加不会移动握把或脚踏接触点。
	// 不乘上面的可达性 Weight，避免握把限制把头部的风摆也压到接近零。
	RotateBranch(OutPose, Neck,
		FQuat(Forward, FMath::DegreesToRadians(FMath::Clamp(Offset.HeadRoll, -40.f, 40.f)))
		* FQuat(Right, FMath::DegreesToRadians(FMath::Clamp(Offset.HeadPitch, -45.f, 22.f))));
}
