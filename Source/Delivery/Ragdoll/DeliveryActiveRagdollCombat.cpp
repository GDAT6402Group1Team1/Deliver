// 出拳与受击时的身体控制。攻击节奏由战斗组件计时；这里负责物理姿态、支撑和冲量反应。
#include "DeliveryActiveRagdollComponent.h"
#include "DeliveryRagdollPhysicsHelpers.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "PhysicsControlComponent.h"
using DeliveryRagdollPhysicsHelpers::HasPhysicsBody;

void UDeliveryActiveRagdollComponent::SetHitReactionStrength(float Multiplier)
{
	// 受击时暂时减弱上身电机，让冲量能把身体推歪；恢复时传入 1。
	// 这里只调整已有控制的力度，不改变晕倒/网络代理原本关闭的控制状态。
	if (!PhysicsControl)
	{
		return;
	}

	FPhysicsControlMultiplier ControlMultiplier;
	ControlMultiplier.LinearStrengthMultiplier = FVector(Multiplier);
	ControlMultiplier.AngularStrengthMultiplier = Multiplier;
	FPhysicsControlMultiplier PelvisMultiplier;
	const float PelvisStrength = FMath::IsNearlyEqual(Multiplier, 1.0f)
		? 1.0f : FMath::Clamp(HitReactionPelvisStrengthMultiplier, 0.0f, 1.0f);
	PelvisMultiplier.LinearStrengthMultiplier = FVector(PelvisStrength);
	PelvisMultiplier.AngularStrengthMultiplier = PelvisStrength;
	// 控制名是创建时返回的真实名字。按名字更新，避免依赖可能未注册的自定义 Set。
	// 不重新启用已经因晕倒/远端代理而关闭的控制。
	if (!PelvisControl.IsNone())
	{
		PhysicsControl->SetControlMultiplier(PelvisControl, PelvisMultiplier, false, true, false);
	}
	if (!ChestControl.IsNone())
	{
		PhysicsControl->SetControlMultiplier(ChestControl, ControlMultiplier, false, true, false);
	}
	for (const FName& Control : TorsoControls)
	{
		PhysicsControl->SetControlMultiplier(Control, ControlMultiplier, false, true, false);
	}
}

void UDeliveryActiveRagdollComponent::SetHitFeetPlanted(bool bPlant)
{
	// 受击脚支撑与出拳脚支撑互斥：冲击应该能推动脚，而不是被拳击锁脚抵消。
	if (bPlant)
	{
		SetPunchFeetPlanted(false);
		bPunchSupportInterrupted = true;
	}
	if (!PhysicsControl || !Mesh || bHitFeetPlanted == bPlant
		|| LeftFoot.Control.IsNone() || RightFoot.Control.IsNone())
	{
		return;
	}

	auto SetFootPlant = [this, bPlant](FFoot& Foot)
	{
		FPhysicsControlMultiplier FootMultiplier;
		FootMultiplier.LinearStrengthMultiplier = FVector(bPlant
			? FMath::Clamp(HitReactionFootStrengthMultiplier, 0.0f, 1.0f) : 1.0f);
		PhysicsControl->SetControlMultiplier(Foot.Control, FootMultiplier, false, true, false);
		if (bPlant)
		{
			// 用当前刚体质心作世界空间位置目标，不把脚拉去步态规划的下一落点；
			// 再沿被打飞的方向推出一段，脚被拖着挪出一小步，而不是钉死在原地。
			Foot.Start = Mesh->GetCenterOfMass(Foot.Bone);
			Foot.Target = Foot.Start
				+ HitPushDirection * FMath::Max(HitReactionFootSlideDistance, 0.0f);
			Foot.Alpha = 1.0f;
			PhysicsControl->SetControlTargetPositionAndOrientation(
				Foot.Control, Foot.Target, Foot.TargetRotation.Rotator(),
				0.0f, true, true, true, false);
		}
		PhysicsControl->SetControlEnabled(Foot.Control, bPlant, true, false);
	};

	SetFootPlant(LeftFoot);
	SetFootPlant(RightFoot);
	bHitFeetPlanted = bPlant;
	if (bPlant)
	{
		bWasMoving = false;
		bPendingStopRecovery = false;
	}
}

void UDeliveryActiveRagdollComponent::SetPunchFeetPlanted(bool bPlant)
{
	// 蓄力/出拳时短暂固定双脚的目标点，让上半身发力不把整个人拖着走。
	// 这仍是物理电机目标，不会瞬移脚；移动或失去平衡时由调用者解除。
	if (bPunchFeetPlanted == bPlant || !PhysicsControl || !Mesh
		|| LeftFoot.Control.IsNone() || RightFoot.Control.IsNone()) return;
	bPunchFeetPlanted = bPlant;
	if (bPlant)
	{
		PunchSupportLocation = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
		PunchSupportYaw = CurrentFacingYaw;
		StartupPlantRemaining = 0.0f;
	}
	for (FFoot* Foot : { &LeftFoot, &RightFoot })
	{
		FPhysicsControlMultiplier Multiplier;
		Multiplier.LinearStrengthMultiplier = FVector(bPlant ? PunchFootSupportStrength : 1.0f);
		PhysicsControl->SetControlMultiplier(Foot->Control, Multiplier, false, true, false);
		if (bPlant)
		{
			Foot->Start = Mesh->GetCenterOfMass(Foot->Bone);
			Foot->Target = Foot->Start;
			FGroundHit Ground;
			if (TraceGround(Foot->Start, FVector::UpVector, Ground))
			{
				Foot->Target = Ground.Point + Ground.Normal * Foot->GroundOffset;
			}
			Foot->Alpha = 1.0f;
			PhysicsControl->SetControlTargetPositionAndOrientation(
				Foot->Control, Foot->Target, Foot->TargetRotation.Rotator(),
				0.0f, true, true, true, false);
		}
		PhysicsControl->SetControlEnabled(Foot->Control, bPlant, true, false);
	}
	bWasMoving = false;
	bPendingStopRecovery = !bPlant;
}

void UDeliveryActiveRagdollComponent::ApplyMeleeImpact(const FVector& Impulse, const FVector& ImpactPoint)
{
	// 真实受击先给刚体冲量，再短暂降低站立电机力度，让身体有时间表现后仰/失衡。
	if (!GetOwner() || !GetOwner()->HasAuthority() || !bIsActive || !Mesh
		|| !HasPhysicsBody(Mesh, Bones.Spine))
	{
		return;
	}

	// 大部分冲量留在胸部；髋只分到很小一部分，两脚仍围绕原位置支撑。
	Mesh->AddImpulseAtLocation(Impulse, ImpactPoint, Bones.Spine);
	if (HasPhysicsBody(Mesh, Bones.Hips) && HitReactionPelvisImpulseFraction > 0.0f)
	{
		Mesh->AddImpulse(
			Impulse * FMath::Clamp(HitReactionPelvisImpulseFraction, 0.0f, 1.0f), Bones.Hips);
	}
	const FVector HorizontalDirection = FVector(Impulse.X, Impulse.Y, 0.0f).GetSafeNormal();
	if (!HorizontalDirection.IsNearlyZero() && HitReactionAngularVelocity > 0.0f)
	{
		// 绕横轴给胸部一个短促后仰，速度变化与刚体质量无关；方向随来拳方向变化。
		const FVector TiltAxis = FVector::CrossProduct(FVector::UpVector, HorizontalDirection);
		Mesh->AddAngularImpulseInRadians(
			TiltAxis * HitReactionAngularVelocity, Bones.Spine, true);
	}
	if (HitReactionUpwardImpulseFraction > 0.0f)
	{
		// 纯向上的一下，跟水平冲量分开算。上半身电机这段时间很松，弹起来之后自由落体，
		// 视觉上就是整个上身带着一跳，而不只是往后仰。
		Mesh->AddImpulseAtLocation(
			FVector::UpVector * Impulse.Size() * HitReactionUpwardImpulseFraction,
			ImpactPoint, Bones.Spine);
	}
	HitPushDirection = HorizontalDirection;
	if (!bIsLimp && HitReactionDuration > 0.0f && GetWorld())
	{
		if (!HorizontalDirection.IsNearlyZero())
		{
			// 冲量把人往外推，出拳的人就在它的反向。被前面打，就转过去面向前。
			HitFacingYaw = (-HorizontalDirection).Rotation().Yaw;
			bHasHitFacing = true;
		}
		HitReactionEndTime = GetWorld()->GetTimeSeconds() + HitReactionDuration;
		SetHitReactionStrength(FMath::Clamp(HitReactionStrengthMultiplier, 0.0f, 1.0f));
		SetHitFeetPlanted(true);
	}
}

void UDeliveryActiveRagdollComponent::BraceTorsoForAction(float DeltaTime, bool bPunch, bool bCarry)
{
	// 拳击/托举时逐渐增强胸腰支撑，动作结束再逐渐放松；不突然切换电机力度。
	// 托举和出拳都要给肩膀稳定支点，否则两只手的电机反作用力会把软躯干来回拧。
	// 强度要渐变：一帧之内把整段脊柱从软切到硬，上半身会当场僵住。
	const float BraceTargetStrength = bCarry ? CarryBraceStrength : PunchBraceStrength;
	BraceAlpha = FMath::FInterpTo(BraceAlpha, bPunch || bCarry ? 1.0f : 0.0f, DeltaTime, PunchBraceSpeed);
	if (FMath::IsNearlyEqual(AppliedBraceAlpha, BraceAlpha, 0.02f)
		&& FMath::IsNearlyEqual(AppliedBraceTargetStrength, BraceTargetStrength, 0.1f)) return;
	AppliedBraceAlpha = BraceAlpha;
	AppliedBraceTargetStrength = BraceTargetStrength;
	PhysicsControl->SetControlAngularData(
		ChestControl, FMath::Lerp(LooseWaistFollowStrength, BraceTargetStrength, BraceAlpha),
		StableMuscleDampingRatio, 0.0f, 0.0f, true, true, false);
	const float Spine = FMath::Lerp(LooseComedyBodyStrength, BraceTargetStrength, BraceAlpha);
	for (const FName& Control : TorsoControls)
	{
		PhysicsControl->SetControlAngularData(
			Control, Spine, StableMuscleDampingRatio, 0.0f, 0.0f, true, true, false);
	}
}

void UDeliveryActiveRagdollComponent::BeginBodyDrivenPunch(FVector AimDirection, float HandSide)
{
	// 进入蓄力阶段：记录方向和出拳侧，后续 Tick 会据此改变胸腰与手臂目标。
	PunchAimDirection = AimDirection.GetSafeNormal2D();
	PunchHandSide = FMath::Sign(HandSide);
	bBodyDrivenPunchActive = !PunchAimDirection.IsNearlyZero();
	bBodyDrivenPunchReleased = false;
	bPunchSupportInterrupted = false;
}

void UDeliveryActiveRagdollComponent::ReleaseBodyDrivenPunch()
{
	// 放拳阶段只切换状态；拳头前送由 BoxingPose 下一帧算目标，冲量由战斗组件施加。
	bBodyDrivenPunchReleased = bBodyDrivenPunchActive;
}

void UDeliveryActiveRagdollComponent::EndBodyDrivenPunch()
{
	// 清除出拳状态，让上半身和手臂逐渐回到自然姿势。
	bBodyDrivenPunchActive = false;
	bBodyDrivenPunchReleased = false;
	SetPunchFeetPlanted(false);
}
