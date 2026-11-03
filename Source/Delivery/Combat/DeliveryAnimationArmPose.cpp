#include "Combat/DeliveryAnimationArmPose.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"

// 根据片段进度平滑淡入和淡出，片段之外不参与姿态混合
float DeliveryAnimationArmPose::BlendWeight(float Time, float Duration, float BlendIn, float BlendOut)
{
	if (Time < 0 || Time >= Duration)
		return 0;
	// 动作开始时逐渐接入，结束时逐渐退出；过渡时长不能为零
	return FMath::Min(FMath::SmoothStep(0.f, FMath::Max(BlendIn, .001f), Time),
	                  FMath::SmoothStep(0.f, FMath::Max(BlendOut, .001f), Duration - Time));
}

// 提取动画中的双臂方向，供不同骨架的物理角色使用
bool DeliveryAnimationArmPose::Sample(const UAnimSequence* Animation, float Time,
                                      FDeliveryAnimationArmPose& Out)
{
	if (!Animation || !Animation->GetSkeleton())
		return false;
	const FReferenceSkeleton& Skeleton = Animation->GetSkeleton()->GetReferenceSkeleton();
	// 兼容带命名空间或前缀的骨骼名，例如 rig:LeftArm 和 rig_LeftArm
	const auto FindBoneIndex = [&](const TCHAR* Name) -> int32
	{
		for (int32 BoneIndex = 0; BoneIndex < Skeleton.GetNum(); ++BoneIndex)
		{
			const FString Bone = Skeleton.GetBoneName(BoneIndex).ToString();
			if (Bone == Name || Bone.EndsWith(FString(TEXT(":")) + Name) ||
			    Bone.EndsWith(FString(TEXT("_")) + Name))
				return BoneIndex;
		}
		return INDEX_NONE;
	};
	// 查找双臂、髋部和头部，缺少任一必要骨骼时放弃本次采样
	const int32 RequiredBoneIndices[] = {
	    FindBoneIndex(TEXT("LeftArm")),      FindBoneIndex(TEXT("LeftForeArm")),
	    FindBoneIndex(TEXT("LeftHand")),     FindBoneIndex(TEXT("RightArm")),
	    FindBoneIndex(TEXT("RightForeArm")), FindBoneIndex(TEXT("RightHand")),
	    FindBoneIndex(TEXT("Hips")),         FindBoneIndex(TEXT("Head"))};
	for (int32 Bone : RequiredBoneIndices)
		if (Bone == INDEX_NONE)
			return false;
	// 分别保存参考姿势和当前动画姿势
	TArray<FTransform> ReferencePose = Skeleton.GetRefBonePose();
	TArray<FTransform> AnimationPose;
	// 为每根骨骼准备一个姿态记录
	AnimationPose.SetNum(Skeleton.GetNum());
	const FAnimExtractContext ExtractionContext(
	    static_cast<double>(FMath::Clamp(Time, 0.f, Animation->GetPlayLength())), false);
	for (int32 BoneIndex = 0; BoneIndex < Skeleton.GetNum(); ++BoneIndex)
	{
		AnimationPose[BoneIndex] = Skeleton.GetRefBonePose()[BoneIndex];
		Animation->GetBoneTransform(AnimationPose[BoneIndex], FSkeletonPoseBoneIndex(BoneIndex),
		                            ExtractionContext, false);
		const int32 ParentBoneIndex = Skeleton.GetParentIndex(BoneIndex);
		if (ParentBoneIndex != INDEX_NONE)
		{
			// 累计父骨骼变换，让各骨骼的位置能够直接相减
			AnimationPose[BoneIndex] *= AnimationPose[ParentBoneIndex];
			ReferencePose[BoneIndex] *= ReferencePose[ParentBoneIndex];
		}
	}
	// 用参考姿势建立前、右、上三个方向，避免依赖源骨架的坐标朝向
	const FVector ReferenceUp = (ReferencePose[RequiredBoneIndices[7]].GetLocation() -
	                             ReferencePose[RequiredBoneIndices[6]].GetLocation())
	                                .GetSafeNormal();
	// 将左右肩连线投影到水平面，去除参考姿势中双肩高度差的影响
	const FVector ReferenceRight =
	    FVector::VectorPlaneProject(ReferencePose[RequiredBoneIndices[3]].GetLocation() -
		                                ReferencePose[RequiredBoneIndices[0]].GetLocation(),
		                            ReferenceUp)
	        .GetSafeNormal();
	const FVector ReferenceForward = FVector::CrossProduct(ReferenceRight, ReferenceUp).GetSafeNormal();
	if (ReferenceForward.IsNearlyZero() || ReferenceRight.IsNearlyZero() || ReferenceUp.IsNearlyZero())
		return false;
	// 将手臂方向换算为向前、向右和向上的三个分量
	const auto ToReferenceDirection = [&](FVector Direction)
	{
		Direction.Normalize();
		return FVector(Direction.Dot(ReferenceForward), Direction.Dot(ReferenceRight),
		               Direction.Dot(ReferenceUp));
	};
	// 用肩到肘、肘到手的方向表示双臂姿态
	for (int32 ArmIndex = 0; ArmIndex < 2; ++ArmIndex)
	{
		// 左臂使用前三个骨骼索引，右臂使用接下来的三个
		const int32 ArmBoneOffset = ArmIndex * 3;
		Out.Upper[ArmIndex] =
		    ToReferenceDirection(AnimationPose[RequiredBoneIndices[ArmBoneOffset + 1]].GetLocation() -
			                     AnimationPose[RequiredBoneIndices[ArmBoneOffset]].GetLocation());
		Out.Lower[ArmIndex] =
		    ToReferenceDirection(AnimationPose[RequiredBoneIndices[ArmBoneOffset + 2]].GetLocation() -
			                     AnimationPose[RequiredBoneIndices[ArmBoneOffset + 1]].GetLocation());
		if (Out.Upper[ArmIndex].IsNearlyZero() || Out.Lower[ArmIndex].IsNearlyZero())
			return false;
	}
	return true;
}
