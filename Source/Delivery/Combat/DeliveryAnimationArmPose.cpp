#include "DeliveryAnimationArmPose.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"

float DeliveryAnimationArmPose::BlendWeight(float Time, float Duration, float BlendIn, float BlendOut)
{
	if (Time < 0 || Time >= Duration) return 0;
	return FMath::Min(FMath::SmoothStep(0.f, FMath::Max(BlendIn, .001f), Time),
		FMath::SmoothStep(0.f, FMath::Max(BlendOut, .001f), Duration - Time));
}

bool DeliveryAnimationArmPose::Sample(const UAnimSequence* Animation, float Time, FDeliveryAnimationArmPose& Out)
{
	if (!Animation || !Animation->GetSkeleton()) return false;
	const FReferenceSkeleton& Skeleton = Animation->GetSkeleton()->GetReferenceSkeleton();
	const auto Find = [&](const TCHAR* Name) -> int32
	{
		for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
		{
			const FString Bone = Skeleton.GetBoneName(Index).ToString();
			if (Bone == Name || Bone.EndsWith(FString(TEXT(":")) + Name)
				|| Bone.EndsWith(FString(TEXT("_")) + Name)) return Index;
		}
		return INDEX_NONE;
	};
	const int32 Bones[] = { Find(TEXT("LeftArm")), Find(TEXT("LeftForeArm")), Find(TEXT("LeftHand")),
		Find(TEXT("RightArm")), Find(TEXT("RightForeArm")), Find(TEXT("RightHand")),
		Find(TEXT("Hips")), Find(TEXT("Head")) };
	for (int32 Bone : Bones) if (Bone == INDEX_NONE) return false;
	TArray<FTransform> Reference = Skeleton.GetRefBonePose();
	TArray<FTransform> Pose;
	Pose.SetNum(Skeleton.GetNum());
	const FAnimExtractContext Context(static_cast<double>(FMath::Clamp(Time, 0.f, Animation->GetPlayLength())), false);
	for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
	{
		Pose[Index] = Skeleton.GetRefBonePose()[Index];
		Animation->GetBoneTransform(Pose[Index], FSkeletonPoseBoneIndex(Index), Context, false);
		const int32 Parent = Skeleton.GetParentIndex(Index);
		if (Parent != INDEX_NONE)
		{
			Pose[Index] *= Pose[Parent];
			Reference[Index] *= Reference[Parent];
		}
	}
	const FVector Up = (Reference[Bones[7]].GetLocation() - Reference[Bones[6]].GetLocation()).GetSafeNormal();
	const FVector Right = FVector::VectorPlaneProject(
		Reference[Bones[3]].GetLocation() - Reference[Bones[0]].GetLocation(), Up).GetSafeNormal();
	const FVector Forward = FVector::CrossProduct(Right, Up).GetSafeNormal();
	if (Forward.IsNearlyZero() || Right.IsNearlyZero() || Up.IsNearlyZero()) return false;
	const auto Canonical = [&](FVector Direction)
	{
		Direction.Normalize();
		return FVector(Direction.Dot(Forward), Direction.Dot(Right), Direction.Dot(Up));
	};
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const int32 Offset = Side * 3;
		Out.Upper[Side] = Canonical(Pose[Bones[Offset+1]].GetLocation() - Pose[Bones[Offset]].GetLocation());
		Out.Lower[Side] = Canonical(Pose[Bones[Offset+2]].GetLocation() - Pose[Bones[Offset+1]].GetLocation());
		if (Out.Upper[Side].IsNearlyZero() || Out.Lower[Side].IsNearlyZero()) return false;
	}
	return true;
}
