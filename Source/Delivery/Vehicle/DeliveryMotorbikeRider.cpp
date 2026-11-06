// 车辆负责收集运动和接触点，动画实例负责计算骨骼，二者通过本帧快照传递数据
#include "Vehicle/DeliveryMotorbike.h"
#include "Vehicle/DeliveryRiderAnimInstance.h"
#include "Character/DeliveryCharacter.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

// 根据车辆挂点校准骑手手脚接触位置
void ADeliveryMotorbike::CalibrateRiderContacts()
{
	if (!RiderMesh || !RiderMesh->GetSkeletalMeshAsset())
		return;
	DeliveryRiderPose::FRig Rig;
	if (!Rig.Initialize(RiderMesh->GetSkeletalMeshAsset()->GetRefSkeleton()))
		return;
	USceneComponent* Targets[] = {LeftHandGrip, RightHandGrip, LeftFootPeg, RightFootPeg};
	// 将原坐姿末端作为接触基准；之后手目标随车把转，脚目标只随车架走
	for (int32 I = 0; I < 4; ++I)
	{
		const FTransform World = Rig.Reference[Rig.Limbs[I].End] * RiderMesh->GetComponentTransform();
		Targets[I]->SetRelativeTransform(
		    World.GetRelativeTransform(Targets[I]->GetAttachParent()->GetComponentTransform()));
	}
}

// 清空骑手随车运动的缓存
void ADeliveryMotorbike::ResetRiderMotion()
{
	LocalRiderMotion = {};
	ReplicatedRiderMotion = {};
	RiderImpact = FVector2D::ZeroVector;
	bRiderSampleValid = false;
	RiderNetElapsed = 0;
	ForceNetUpdate();
}

// 根据车速、转向和撞击更新骑手身体偏移
void ADeliveryMotorbike::UpdateRiderMotion(float DeltaTime)
{
	if (!Driver)
	{
		bRiderSampleValid = false;
		return;
	}
	if (!HasAuthority() && !IsLocallyControlled())
		return;
	const float Z = GetActorLocation().Z;
	// 首帧或长时间卡顿后不计算差分，避免一次位置跳变被误当成巨大颠簸
	const bool bValid = bRiderSampleValid && DeltaTime > SMALL_NUMBER && DeltaTime <= .1f;
	const float VerticalSpeed = bValid ? (Z - RiderSampleZ) / DeltaTime : 0.f;
	LocalRiderMotion.Speed = FMath::Clamp(CurrentSpeed / FMath::Max(MaxSpeed, 1.f), -1.f, 1.f);
	LocalRiderMotion.Steer = SteerInput;
	// 速度差除以时间得到加速度，再按车辆加速能力换算到 -1～1
	LocalRiderMotion.Acceleration = bValid ? FMath::Clamp((CurrentSpeed - RiderSampleSpeed) / DeltaTime /
	                                                          FMath::Max(ThrottleAcceleration, 1.f),
	                                                      -1.f, 1.f)
	                                       : 0.f;
	// 竖直速度的变化表示上下颠簸，3500 用于把幅度换算到动画输入范围
	LocalRiderMotion.Bump =
	    bValid ? FMath::Clamp((VerticalSpeed - RiderSampleVerticalSpeed) / DeltaTime / 3500.f, -1.f, 1.f)
		       : 0.f;
	RiderSampleZ = Z;
	RiderSampleSpeed = CurrentSpeed;
	RiderSampleVerticalSpeed = VerticalSpeed;
	bRiderSampleValid = true;
	if (HasAuthority())
	{
		RiderImpact *= FMath::Exp(-6.f * DeltaTime);
		LocalRiderMotion.Impact = RiderImpact;
		RiderNetElapsed += DeltaTime;
		// 每隔 0.05 秒更新待复制的数据，实际网络到达频率仍受复制调度影响
		if (RiderNetElapsed >= .05f)
		{
			ReplicatedRiderMotion = LocalRiderMotion;
			RiderNetElapsed = 0;
		}
	}
	else
		LocalRiderMotion.Impact = ReplicatedRiderMotion.Impact;
}

// 整理本帧骑手姿态数据，交给动画实例使用
void ADeliveryMotorbike::BuildRiderAnimationFrame(FDeliveryRiderFrame& Frame) const
{
	Frame.bActive = IsValid(Driver) && RiderMesh && RiderMesh->GetSkeletalMeshAsset();
	if (!Frame.bActive)
		return;
	// 本机驾驶用本地数据，其他玩家看到的骑手用服务器复制数据
	Frame.Motion = HasAuthority() || IsLocallyControlled() ? LocalRiderMotion : ReplicatedRiderMotion;
	Frame.Settings = RiderAnimationSettings;
	// 骨骼求解使用相对骑手模型的坐标，车身方向和握把脚踏也必须转到相同坐标
	const FTransform MeshWorld = RiderMesh->GetComponentTransform();
	Frame.Forward = MeshWorld.InverseTransformVectorNoScale(MeshRoot->GetForwardVector()).GetSafeNormal();
	Frame.Right = MeshWorld.InverseTransformVectorNoScale(MeshRoot->GetRightVector()).GetSafeNormal();
	const USceneComponent* Targets[] = {LeftHandGrip, RightHandGrip, LeftFootPeg, RightFootPeg};
	for (int32 I = 0; I < 4; ++I)
		Frame.Contacts[I] = Targets[I]->GetComponentTransform().GetRelativeTransform(MeshWorld);
}
