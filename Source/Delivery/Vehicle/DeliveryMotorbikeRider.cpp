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
	const bool bValid = bRiderSampleValid && DeltaTime > SMALL_NUMBER && DeltaTime <= .1f;
	const float VerticalSpeed = bValid ? (Z - RiderSampleZ) / DeltaTime : 0.f;
	LocalRiderMotion.Speed = FMath::Clamp(CurrentSpeed / FMath::Max(MaxSpeed, 1.f), -1.f, 1.f);
	LocalRiderMotion.Steer = SteerInput;
	LocalRiderMotion.Acceleration = bValid ? FMath::Clamp((CurrentSpeed - RiderSampleSpeed) / DeltaTime /
	                                                          FMath::Max(ThrottleAcceleration, 1.f),
	                                                      -1.f, 1.f)
	                                       : 0.f;
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
	Frame.Motion = HasAuthority() || IsLocallyControlled() ? LocalRiderMotion : ReplicatedRiderMotion;
	Frame.Settings = RiderAnimationSettings;
	const FTransform MeshWorld = RiderMesh->GetComponentTransform();
	Frame.Forward = MeshWorld.InverseTransformVectorNoScale(MeshRoot->GetForwardVector()).GetSafeNormal();
	Frame.Right = MeshWorld.InverseTransformVectorNoScale(MeshRoot->GetRightVector()).GetSafeNormal();
	const USceneComponent* Targets[] = {LeftHandGrip, RightHandGrip, LeftFootPeg, RightFootPeg};
	for (int32 I = 0; I < 4; ++I)
		Frame.Contacts[I] = Targets[I]->GetComponentTransform().GetRelativeTransform(MeshWorld);
}
