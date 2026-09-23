#pragma once

#include "Components/SkeletalMeshComponent.h"

// 启动时骨骼可能存在、物理资产却没有对应刚体。
// 需要向某节身体施加力时，先确认它真的参与物理模拟。
namespace DeliveryRagdollPhysicsHelpers
{
inline bool HasPhysicsBody(const USkeletalMeshComponent* Mesh, FName Bone)
{
	return Mesh && Bone != NAME_None
		&& Mesh->GetBoneIndex(Bone) != INDEX_NONE
		&& Mesh->GetBodyInstance(Bone) != nullptr;
}
}
