#pragma once

#include "Components/SkeletalMeshComponent.h"
#include "PhysicsControlData.h"

class UPhysicsControlComponent;

// 启动时骨骼可能存在、物理资产却没有对应刚体
// 需要向某节身体施加力时，先确认它真的参与物理模拟
namespace DeliveryRagdollPhysicsHelpers
{
// 物理刚体创建后，仅修正质量明显异常的髋部
// 只修改当前刚体，不修改源资源或碰撞形状
bool StabilizePelvisMass(USkeletalMeshComponent* Mesh, FName Pelvis);
// 根据参考姿势计算相对父刚体的固定目标，不读取变化中的动画和物理缓存
TArray<FName> CreateReferencePoseControls(USkeletalMeshComponent* Mesh, UPhysicsControlComponent* Controls,
                                          const TArray<FName>& Bones, FPhysicsControlData Data, FName Set);

inline bool HasPhysicsBody(const USkeletalMeshComponent* Mesh, FName Bone)
{
	return Mesh && Bone != NAME_None && Mesh->GetBoneIndex(Bone) != INDEX_NONE &&
	       Mesh->GetBodyInstance(Bone) != nullptr;
}
}
