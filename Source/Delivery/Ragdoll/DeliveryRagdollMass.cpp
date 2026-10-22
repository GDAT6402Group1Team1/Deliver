#include "DeliveryRagdollPhysicsHelpers.h"
#include "Delivery.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsControlComponent.h"
#include "Engine/SkeletalMesh.h"

TArray<FName> DeliveryRagdollPhysicsHelpers::CreateReferencePoseControls(
 USkeletalMeshComponent* Mesh, UPhysicsControlComponent* Controls,
 const TArray<FName>& Bones, FPhysicsControlData Data, FName Set)
{
 TArray<FName> Result;
 if (!Mesh || !Mesh->GetSkeletalMeshAsset() || !Controls) return Result;
 const FReferenceSkeleton& Ref = Mesh->GetSkeletalMeshAsset()->GetRefSkeleton();
 Data.bUseSkeletalAnimation = false;
 for (FName Bone : Bones)
 {
  int32 Index = Ref.FindBoneIndex(Bone);
  if (Index == INDEX_NONE || !HasPhysicsBody(Mesh, Bone)) continue;
  FQuat Relative = Ref.GetRefBonePose()[Index].GetRotation();
  int32 Parent = Ref.GetParentIndex(Index);
  while (Parent != INDEX_NONE && !HasPhysicsBody(Mesh, Ref.GetBoneName(Parent)))
  {
   Relative = Ref.GetRefBonePose()[Parent].GetRotation() * Relative;
   Parent = Ref.GetParentIndex(Parent);
  }
  if (Parent == INDEX_NONE) continue;
  const TArray<FName> Names = Controls->CreateControlsFromSkeletalMesh(
   Mesh, {Bone}, EPhysicsControlType::ParentSpace, Data, Set);
  for (FName Name : Names)
   Controls->SetControlTargetOrientation(Name, Relative.GetNormalized().Rotator(), 0.f, true, false, true, false);
  Result.Append(Names);
 }
 return Result;
}

bool DeliveryRagdollPhysicsHelpers::StabilizePelvisMass(USkeletalMeshComponent* Mesh, FName Pelvis)
{
	FBodyInstance* Body = Mesh ? Mesh->GetBodyInstance(Pelvis) : nullptr;
	const UPhysicsAsset* Asset = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
	if (!Body || !Asset) return false;
	float LargestNeighborMass = 0.f;
	double SmallestNeighborInertia = TNumericLimits<double>::Max();
	for (const UPhysicsConstraintTemplate* Joint : Asset->ConstraintSetup)
	{
		if (!Joint) continue;
		const FConstraintInstance& Setup = Joint->DefaultInstance;
		const FName Neighbor = Setup.ConstraintBone1 == Pelvis ? Setup.ConstraintBone2
			: Setup.ConstraintBone2 == Pelvis ? Setup.ConstraintBone1 : NAME_None;
		const FBodyInstance* Other = Neighbor.IsNone() ? nullptr : Mesh->GetBodyInstance(Neighbor);
		if (!Other) continue;
		LargestNeighborMass = FMath::Max(LargestNeighborMass, Other->GetBodyMass());
		const double Inertia = Other->GetBodyInertiaTensor().GetMin();
		if (Inertia > UE_SMALL_NUMBER) SmallestNeighborInertia = FMath::Min(SmallestNeighborInertia, Inertia);
	}
	const float OldMass = Body->GetBodyMass();
	// Leave ordinary rigs and deliberately moderate mass ratios alone. The renwu
	// pelvis was 0.0115 kg between 24-32 kg bodies (>2000:1).
	if (OldMass <= 0.f || LargestNeighborMass <= OldMass * 20.f) return false;
	const FVector OldInertia = Body->GetBodyInertiaTensor();
	Body->SetMassOverride(LargestNeighborMass * 0.5f, true);
	Body->UpdateMassProperties();
	// Its tiny collision capsule also yields tiny rotational inertia. Correcting
	// mass alone is insufficient; use a conservative isotropic inertia floor
	// based on adjacent bodies, without enlarging collisions into the thighs.
	if (SmallestNeighborInertia < TNumericLimits<double>::Max())
	{
		const double Floor = SmallestNeighborInertia * 0.25;
		const FVector BaseScale = Body->InertiaTensorScale;
		// Chaos treats this as equivalent shape scale, not a direct inertia
		// multiplier (uniform scale can yield S^2 or S^5). Fit against the actual
		// tensor so we do not accidentally create an excessively rigid pelvis.
		double Low=1.0, High=1.0;
		while (Body->GetBodyInertiaTensor().GetMin()<Floor && High<4096.0)
		{
			High*=2.0;
			Body->InertiaTensorScale=BaseScale*High;
			Body->UpdateMassProperties();
		}
		for (int32 Iteration=0; High>1.0 && Iteration<16; ++Iteration)
		{
			const double Mid=(Low+High)*0.5;
			Body->InertiaTensorScale=BaseScale*Mid;
			Body->UpdateMassProperties();
			if (Body->GetBodyInertiaTensor().GetMin()<Floor) Low=Mid;
			else High=Mid;
		}
		Body->InertiaTensorScale=BaseScale*High;
		Body->UpdateMassProperties();
	}
	UE_LOG(LogDelivery, Log, TEXT("PelvisMassRepair bone=%s massKg=%.4f->%.4f neighborMaxKg=%.4f inertia=%s->%s"),
		*Pelvis.ToString(), OldMass, Body->GetBodyMass(), LargestNeighborMass,
		*OldInertia.ToCompactString(), *Body->GetBodyInertiaTensor().ToCompactString());
	return true;
}
