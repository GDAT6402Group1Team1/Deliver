// 根据参考骨架创建物理控制目标，并修正异常的髋部质量与惯量
#include "Ragdoll/DeliveryRagdollPhysicsHelpers.h"
#include "Delivery.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsControlComponent.h"
#include "Engine/SkeletalMesh.h"

// 使用固定参考姿势建立父空间控制，避免动画或物理缓存改变关节目标
TArray<FName> DeliveryRagdollPhysicsHelpers::CreateReferencePoseControls(USkeletalMeshComponent* Mesh,
                                                                         UPhysicsControlComponent* Controls,
                                                                         const TArray<FName>& Bones,
                                                                         FPhysicsControlData Data, FName Set)
{
	TArray<FName> Result;
	if (!Mesh || !Mesh->GetSkeletalMeshAsset() || !Controls)
		return Result;
	const FReferenceSkeleton& Ref = Mesh->GetSkeletalMeshAsset()->GetRefSkeleton();
	Data.bUseSkeletalAnimation = false;
	for (FName Bone : Bones)
	{
		int32 Index = Ref.FindBoneIndex(Bone);
		if (Index == INDEX_NONE || !HasPhysicsBody(Mesh, Bone))
			continue;
		FQuat Relative = Ref.GetRefBonePose()[Index].GetRotation();
		int32 Parent = Ref.GetParentIndex(Index);
		// 跳过没有刚体的中间骨骼，同时累计相对旋转，直到找到实际物理父体
		while (Parent != INDEX_NONE && !HasPhysicsBody(Mesh, Ref.GetBoneName(Parent)))
		{
			Relative = Ref.GetRefBonePose()[Parent].GetRotation() * Relative;
			Parent = Ref.GetParentIndex(Parent);
		}
		if (Parent == INDEX_NONE)
			continue;
		const TArray<FName> Names = Controls->CreateControlsFromSkeletalMesh(
		    Mesh, {Bone}, EPhysicsControlType::ParentSpace, Data, Set);
		for (FName Name : Names)
			Controls->SetControlTargetOrientation(Name, Relative.GetNormalized().Rotator(), 0.f, true, false,
			                                      true, false);
		Result.Append(Names);
	}
	return Result;
}

// 调整髋部及相关刚体质量，改善身体控制的稳定性
bool DeliveryRagdollPhysicsHelpers::StabilizePelvisMass(USkeletalMeshComponent* Mesh, FName Pelvis)
{
	FBodyInstance* Body = Mesh ? Mesh->GetBodyInstance(Pelvis) : nullptr;
	const UPhysicsAsset* Asset = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
	if (!Body || !Asset)
		return false;
	float LargestNeighborMass = 0.f;
	double SmallestNeighborInertia = TNumericLimits<double>::Max();
	for (const UPhysicsConstraintTemplate* Joint : Asset->ConstraintSetup)
	{
		if (!Joint)
			continue;
		const FConstraintInstance& Setup = Joint->DefaultInstance;
		const FName Neighbor = Setup.ConstraintBone1 == Pelvis   ? Setup.ConstraintBone2
		                       : Setup.ConstraintBone2 == Pelvis ? Setup.ConstraintBone1
		                                                         : NAME_None;
		const FBodyInstance* Other = Neighbor.IsNone() ? nullptr : Mesh->GetBodyInstance(Neighbor);
		if (!Other)
			continue;
		LargestNeighborMass = FMath::Max(LargestNeighborMass, Other->GetBodyMass());
		const double Inertia = Other->GetBodyInertiaTensor().GetMin();
		if (Inertia > UE_SMALL_NUMBER)
			SmallestNeighborInertia = FMath::Min(SmallestNeighborInertia, Inertia);
	}
	const float OldMass = Body->GetBodyMass();
	// 正常质量比例保持不变，只处理明显过轻的髋部
	// 当前角色资源的髋部质量远低于相邻身体部位，会使约束不稳定
	// 仅修正相邻最大质量超过髋部 20 倍的异常情况，普通骨架保持原值
	if (OldMass <= 0.f || LargestNeighborMass <= OldMass * 20.f)
		return false;
	const FVector OldInertia = Body->GetBodyInertiaTensor();
	Body->SetMassOverride(LargestNeighborMass * 0.5f, true);
	Body->UpdateMassProperties();
	// 髋部碰撞体过小还会导致转动惯量过低，仅增加质量不够
	// 参考相邻刚体，为各方向的转动惯量设置下限
	// 不扩大碰撞体，避免碰撞形状挤入大腿
	if (SmallestNeighborInertia < TNumericLimits<double>::Max())
	{
		// 惯量下限取相邻刚体最小主惯量的四分之一，不通过放大碰撞体修正
		const double Floor = SmallestNeighborInertia * 0.25;
		const FVector BaseScale = Body->InertiaTensorScale;
		// 引擎将该参数作为形状缩放，而不是直接作为惯量倍数
		// 因此根据实际计算出的惯量反推缩放值
		// 避免髋部被修正得过于难以转动
		// 先扩大缩放上界，再二分逼近目标惯量；缩放与实际惯量并非简单线性关系
		double Low = 1.0, High = 1.0;
		while (Body->GetBodyInertiaTensor().GetMin() < Floor && High < 4096.0)
		{
			High *= 2.0;
			Body->InertiaTensorScale = BaseScale * High;
			Body->UpdateMassProperties();
		}
		for (int32 Iteration = 0; High > 1.0 && Iteration < 16; ++Iteration)
		{
			const double Mid = (Low + High) * 0.5;
			Body->InertiaTensorScale = BaseScale * Mid;
			Body->UpdateMassProperties();
			if (Body->GetBodyInertiaTensor().GetMin() < Floor)
				Low = Mid;
			else
				High = Mid;
		}
		Body->InertiaTensorScale = BaseScale * High;
		Body->UpdateMassProperties();
	}
	UE_LOG(LogDelivery, Log,
	       TEXT("PelvisMassRepair bone=%s massKg=%.4f->%.4f neighborMaxKg=%.4f inertia=%s->%s"),
	       *Pelvis.ToString(), OldMass, Body->GetBodyMass(), LargestNeighborMass,
	       *OldInertia.ToCompactString(), *Body->GetBodyInertiaTensor().ToCompactString());
	return true;
}
