// 地面探测与陡坡翻滚。普通行走只接受可站立坡面；翻滚探测需要读取更陡的原始坡面。
#include "DeliveryActiveRagdollComponent.h"
#include "DeliveryRagdollPhysicsHelpers.h"
#include "CollisionQueryParams.h"
#include "DeliveryCharacter.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
using DeliveryRagdollPhysicsHelpers::HasPhysicsBody;

bool UDeliveryActiveRagdollComponent::SampleGround(
	const FVector& Planned, const FVector& Fallback, const FVector& AlongNormal, FGroundHit& OutHit) const
{
	// 优先检查预计落点；坡沿上这个点可能悬空，再退回髋下方探一次。
	// 两次都没有可站立表面时，上层会暂停髋部的世界空间支撑电机。
	if (TraceGround(Planned, AlongNormal, OutHit))
	{
		return true;
	}
	return TraceGround(Fallback, AlongNormal, OutHit);
}

float UDeliveryActiveRagdollComponent::GetUprightDot() const
{
	if (!Mesh)
	{
		return 0.0f;
	}

	// 把启动时髋里的头顶方向转到现在，再和坡面法线做点积。不要和世界竖直向上比。
	const FQuat PelvisRotation = Mesh->GetBoneQuaternion(Bones.Hips, EBoneSpaces::WorldSpace);
	const FVector Up = PelvisRotation.RotateVector(UprightInPelvisSpace).GetSafeNormal();
	return FVector::DotProduct(Up, CurrentGroundNormal);
}

bool UDeliveryActiveRagdollComponent::TraceGround(
	const FVector& Around, const FVector& AlongNormal, FGroundHit& OutHit) const
{
	// Around 是探测中心，AlongNormal 指明沿哪条“局部竖直线”寻找坡面；
	// 命中的位置和法线一起返回，供髋部高度、脚步落点和站立判断共用。
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FVector Normal = AlongNormal.GetSafeNormal();
	if (Normal.IsNearlyZero() || Normal.Z < 0.0f)
	{
		Normal = FVector::UpVector;
	}

	const FVector Start = Around + Normal * 60.0f;
	const FVector End = Around - Normal * 220.0f;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(RagdollGround), false, GetOwner());
	FHitResult Hit;
	FCollisionObjectQueryParams GroundObjects;
	GroundObjects.AddObjectTypesToQuery(ECC_WorldStatic);
	GroundObjects.AddObjectTypesToQuery(ECC_WorldDynamic);
	// 电瓶车是 Vehicle；只认静态/动态地面，不把车顶误当成可站立坡面。
	if (World->LineTraceSingleByObjectType(Hit, Start, End, GroundObjects, Params))
	{
		FVector HitNormal = Hit.ImpactNormal.GetSafeNormal();
		if (HitNormal.Z < 0.0f)
		{
			HitNormal = -HitNormal;
		}
		const float MinimumWalkableNormalZ = FMath::Cos(FMath::DegreesToRadians(MaxWalkableSlopeDegrees));
		if (HitNormal.Z < MinimumWalkableNormalZ)
		{
			// 比 MaxWalkableSlopeDegrees 更陡的面不按坡面走，例如立面和台阶踢面。
			return false;
		}

		OutHit.Point = Hit.ImpactPoint;
		OutHit.Normal = HitNormal;
		return true;
	}
	return false;
}

bool UDeliveryActiveRagdollComponent::TraceTumbleSurface(FGroundHit& OutHit) const
{
	// 翻滚检测允许读取比普通行走更陡的坡；不能拿“可站立”规则过滤掉触发翻滚的坡面。
	if (!Mesh || !GetWorld())
	{
		return false;
	}
	const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(RagdollTumbleGround), false, GetOwner());
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_WorldStatic);
	Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
	FHitResult Hit;
	if (!GetWorld()->LineTraceSingleByObjectType(Hit, Hips + FVector(0, 0, 25),
		Hips - FVector(0, 0, 220), Objects, Params)
		|| Hit.ImpactNormal.Z < FMath::Cos(FMath::DegreesToRadians(75.0f))
		|| Hips.Z - Hit.ImpactPoint.Z > StandHeight * 1.5f + 20.0f)
	{
		return false;
	}
	OutHit.Point = Hit.ImpactPoint;
	OutHit.Normal = Hit.ImpactNormal.GetSafeNormal();
	return true;
}

void UDeliveryActiveRagdollComponent::UpdateSlopeTumble(float DeltaTime)
{
	// 服务器根据坡度和沿坡下滑速度决定是否进入全物理翻滚；
	// 缓坡继续交给站立/脚步控制，陡坡上不强迫两脚维持正常走路姿势。
	if (!bIsActive || !Mesh || !GetOwner())
	{
		return;
	}
	if (bExternalLimpRequested)
	{
		// 受伤晕倒始终优先；坡地计时不能在 HP 恢复之前自行开电机。
		bSlopeTumbling = false;
		TumbleEntryTime = TumbleElapsed = TumbleRecoveryTime = 0.0f;
		return;
	}

	FGroundHit Surface;
	const bool bOnSurface = TraceTumbleSurface(Surface);
	const float SlopeDegrees = bOnSurface
		? FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Surface.Normal.Z, 0.0f, 1.0f)))
		: 0.0f;
	const FVector Velocity = Mesh->GetPhysicsLinearVelocity(Bones.Hips);
	const FVector Downhill = bOnSurface
		? FVector(Surface.Normal.X, Surface.Normal.Y, 0.0f).GetSafeNormal()
		: FVector::ZeroVector;
	const float DownhillSpeed = FVector::DotProduct(Velocity, Downhill);

	if (!bSlopeTumbling)
	{
		TumbleCooldownTime = FMath::Max(0.0f, TumbleCooldownTime - DeltaTime);
		const bool bShouldTumble = TumbleCooldownTime <= 0.0f && !bJumping && bOnSurface
			&& SlopeDegrees >= TumbleSlopeDegrees && DownhillSpeed >= TumbleDownhillSpeed;
		TumbleEntryTime = bShouldTumble ? TumbleEntryTime + DeltaTime : 0.0f;
		if (TumbleEntryTime < TumbleEntryDelay)
		{
			return;
		}
		bSlopeTumbling = true;
		TumbleElapsed = TumbleRecoveryTime = 0.0f;
		TumbleEntryTime = 0.0f;
		if (ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner()))
		{
			if (UDeliveryGrabComponent* Grab = Character->GetGrabComponent())
			{
				Grab->ForceRelease();
			}
		}
		EndBodyDrivenPunch();
		ApplyLimpState(true);
		if (HasPhysicsBody(Mesh, Bones.Spine) && !Downhill.IsNearlyZero())
		{
			Mesh->AddAngularImpulseInRadians(
				FVector::CrossProduct(Downhill, FVector::UpVector) * TumbleStartAngularSpeed,
				Bones.Spine, true);
		}
		return;
	}

	TumbleElapsed += DeltaTime;
	const bool bSafeToRise = bOnSurface && SlopeDegrees <= TumbleRecoverySlopeDegrees
		&& Velocity.Size2D() <= TumbleRecoverySpeed;
	TumbleRecoveryTime = bSafeToRise ? TumbleRecoveryTime + DeltaTime : 0.0f;
	if (TumbleElapsed >= TumbleMinimumDuration && TumbleRecoveryTime >= TumbleRecoveryDelay)
	{
		bSlopeTumbling = false;
		TumbleRecoveryTime = 0.0f;
		TumbleCooldownTime = 1.0f;
		ApplyLimpState(false);
	}
}
