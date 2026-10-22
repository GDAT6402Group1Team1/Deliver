#pragma once

#include "Components/SkeletalMeshComponent.h"
#include "PhysicsControlData.h"

class UPhysicsControlComponent;

// 启动时骨骼可能存在、物理资产却没有对应刚体。
// 需要向某节身体施加力时，先确认它真的参与物理模拟。
namespace DeliveryRagdollPhysicsHelpers
{
// Repair only a pathological, near-massless pelvis after physics creation.
// Changes live body properties, never the source asset or collision geometry.
bool StabilizePelvisMass(USkeletalMeshComponent* Mesh, FName Pelvis);
// Fixed parent-space posture, independent of mutable animation/physics buffers.
TArray<FName> CreateReferencePoseControls(USkeletalMeshComponent* Mesh,
 UPhysicsControlComponent* Controls, const TArray<FName>& Bones,
 FPhysicsControlData Data, FName Set);

inline bool HasPhysicsBody(const USkeletalMeshComponent* Mesh, FName Bone)
{
	return Mesh && Bone != NAME_None
		&& Mesh->GetBoneIndex(Bone) != INDEX_NONE
		&& Mesh->GetBodyInstance(Bone) != nullptr;
}
}
