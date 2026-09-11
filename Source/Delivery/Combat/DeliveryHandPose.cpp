#include "Combat/DeliveryHandPose.h"

#include "Animation/AnimInstance.h"
#include "BonePose.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"

namespace DeliveryHandPose
{
	namespace
	{
		TArray<FTransform> BuildComponentSpace(const FReferenceSkeleton& RefSkeleton)
		{
			const TArray<FTransform>& Local = RefSkeleton.GetRefBonePose();
			TArray<FTransform> ComponentSpace;
			ComponentSpace.SetNum(Local.Num());
			for (int32 Bone = 0; Bone < Local.Num(); ++Bone)
			{
				const int32 Parent = RefSkeleton.GetParentIndex(Bone);
				ComponentSpace[Bone] = Parent == INDEX_NONE
					? Local[Bone]
					: Local[Bone] * ComponentSpace[Parent];
			}
			return ComponentSpace;
		}
	}

	void BuildFist(const FReferenceSkeleton& RefSkeleton, float FingerAngleDegrees,
		float ThumbAngleDegrees, TArray<FCurledBone>& OutBones)
	{
		OutBones.Reset();
		const TArray<FTransform> ComponentSpace = BuildComponentSpace(RefSkeleton);
		static const TCHAR* Fingers[] = { TEXT("Thumb"), TEXT("Index"), TEXT("Middle"), TEXT("Ring"), TEXT("Pinky") };

		for (const TCHAR* Side : { TEXT("Left"), TEXT("Right") })
		{
			// 每根手指三节。缺一节就整只手放弃，宁可不握拳也不要把手指掰成怪样。
			int32 Segments[5][3];
			bool bComplete = true;
			for (int32 Finger = 0; Finger < 5; ++Finger)
			for (int32 Joint = 0; Joint < 3; ++Joint)
			{
				Segments[Finger][Joint] = RefSkeleton.FindBoneIndex(
					FName(*FString::Printf(TEXT("%sHand%s%d"), Side, Fingers[Finger], Joint + 1)));
				bComplete &= Segments[Finger][Joint] != INDEX_NONE;
			}
			const int32 Wrist = RefSkeleton.FindBoneIndex(FName(*FString::Printf(TEXT("%sHand"), Side)));
			if (!bComplete || Wrist == INDEX_NONE) continue;

			const auto Location = [&](int32 Bone) { return ComponentSpace[Bone].GetLocation(); };
			// 四指的指尖连线方向，以及横跨指根的方向。两者的叉乘垂直于手掌平面，
			// 但正负两个方向都垂直，还得判断哪一边是掌心。
			const FVector Along = (Location(Segments[2][2]) - Location(Segments[2][0])).GetSafeNormal();
			const FVector Across = Location(Segments[1][0]) - Location(Segments[4][0]);
			FVector Palm = FVector::CrossProduct(Across, Along).GetSafeNormal();
			if (Palm.IsNearlyZero()) continue;

			// 手指在参考姿势里本来就朝掌心弯一点，这个弯曲方向最可靠。但这套骨架有一只手
			// 的手指是完全笔直的，叉乘退化，所以让能判断的手指投票。
			float Vote = 0;
			for (int32 Finger = 1; Finger < 5; ++Finger)
			{
				const FVector First = Location(Segments[Finger][1]) - Location(Segments[Finger][0]);
				const FVector Second = Location(Segments[Finger][2]) - Location(Segments[Finger][1]);
				const FVector Bend = FVector::CrossProduct(First, Second);
				if (Bend.Size() < 0.01f * First.Size() * Second.Size()) continue;
				Vote += FVector::DotProduct(FVector::CrossProduct(Bend.GetSafeNormal(), Along), Palm);
			}
			if (FMath::IsNearlyZero(Vote))
			{
				// 一根都判断不出来时看拇指：拇指是用来对握的，指尖总是偏在掌心那一侧。
				const FVector Center = (Location(Wrist) + Location(Segments[2][0])) * 0.5f;
				Vote = FVector::DotProduct(Location(Segments[0][2]) - Center, Palm);
			}
			if (FMath::IsNearlyZero(Vote)) continue;
			Palm *= FMath::Sign(Vote);

			for (int32 Finger = 0; Finger < 5; ++Finger)
			{
				// 每根手指绕自己指向与掌心法线的公垂线弯，这样五根手指朝同一侧合拢。
				const FVector Direction = (Location(Segments[Finger][2]) - Location(Segments[Finger][0])).GetSafeNormal();
				const FVector Hinge = FVector::CrossProduct(Direction, Palm).GetSafeNormal();
				if (Hinge.IsNearlyZero()) continue;
				const float Angle = FMath::DegreesToRadians(Finger == 0 ? ThumbAngleDegrees : FingerAngleDegrees);
				for (const int32 Bone : Segments[Finger])
				{
					const int32 Parent = RefSkeleton.GetParentIndex(Bone);
					if (Parent == INDEX_NONE) continue;
					// 局部旋转要表达在父骨骼的坐标系里，左乘上去才是绕这根关节转。
					const FVector Axis = ComponentSpace[Parent].GetRotation().UnrotateVector(Hinge);
					OutBones.Add({ Bone, FQuat(Axis.GetSafeNormal(), Angle) });
				}
			}
		}
	}
}

void FDeliveryHandPoseProxy::Build(const UAnimInstance* Instance)
{
	CurledBones.Reset();
	const UDeliveryHandPoseAnimInstance* Hands = Cast<UDeliveryHandPoseAnimInstance>(Instance);
	const USkeletalMeshComponent* Mesh = Instance ? Instance->GetSkelMeshComponent() : nullptr;
	const USkeletalMesh* Asset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	if (Hands && Asset)
	{
		DeliveryHandPose::BuildFist(Asset->GetRefSkeleton(),
			Hands->FingerCurlAngle, Hands->ThumbCurlAngle, CurledBones);
	}
}

void FDeliveryHandPoseProxy::Initialize(UAnimInstance* InAnimInstance)
{
	FAnimInstanceProxy::Initialize(InAnimInstance);
	Build(InAnimInstance);
}

bool FDeliveryHandPoseProxy::Evaluate(FPoseContext& Output)
{
	Output.ResetToRefPose();
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	for (const DeliveryHandPose::FCurledBone& Curled : CurledBones)
	{
		const FCompactPoseBoneIndex Index = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(Curled.BoneIndex));
		if (Index == INDEX_NONE) continue;
		FTransform& Bone = Output.Pose[Index];
		Bone.SetRotation((Curled.Delta * Bone.GetRotation()).GetNormalized());
	}
	return true;
}

void UDeliveryHandPoseAnimInstance::RebuildFist()
{
	GetProxyOnGameThread<FDeliveryHandPoseProxy>().Build(this);
}

FAnimInstanceProxy* UDeliveryHandPoseAnimInstance::CreateAnimInstanceProxy()
{
	return new FDeliveryHandPoseProxy(this);
}
