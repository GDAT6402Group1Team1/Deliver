// 身体姿态：输入方向 → 贴地的髋部目标 → 面向和上身旋转 → 物理电机。
// 目标只是电机想去的位置；角色刚体的位置仍由物理模拟决定。
#include "DeliveryActiveRagdollComponent.h"
#include "DeliveryCharacter.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Math/RotationMatrix.h"
#include "PhysicsControlComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

void UDeliveryActiveRagdollComponent::UpdateControlTargets(float DeltaTime)
{
	// 这是每帧控制入口。先让髋部选定落点，双脚才能依据同一个目标规划步伐。
	if (GetOwner() && GetOwner()->HasAuthority() && GetOwner()->GetLocalRole() == ROLE_Authority)
	{
		const APawn* Pawn = Cast<APawn>(GetOwner());
		if (Pawn && !Pawn->IsLocallyControlled() && GetWorld()
			&& GetWorld()->GetTimeSeconds() - LastMoveInputTime > ServerInputTimeout)
		{
			// 高频移动 RPC 允许丢包，超时归零可避免松键包丢失后继续移动。
			MoveInput = FVector2D::ZeroVector;
		}
	}

	const FVector Wish = GetWishDir();
	if (bPunchFeetPlanted && FVector::Dist2D(
		Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace), PunchSupportLocation) > PunchSupportBreakDistance)
	{
		bPunchSupportInterrupted = true;
	}
	// 只有站稳、没移动且面向拳路时才暂时支撑双脚。走动、跳起、受击或身体已经
	// 离开原支撑位置后必须松开，否则脚电机会把正在移动/下落的身体钉在原地。
	SetPunchFeetPlanted(bBodyDrivenPunchActive && !bPunchSupportInterrupted
		&& !bHitFeetPlanted && !bIsLimp && IsGrounded() && Wish.IsNearlyZero()
		&& GetUprightDot() > 0.65f
		&& (bPunchFeetPlanted || Mesh->GetPhysicsLinearVelocity(Bones.Hips).Size2D() < 80.0f)
		&& FMath::Abs(FMath::FindDeltaAngleDegrees(CurrentFacingYaw, PunchAimDirection.Rotation().Yaw)) < 35.0f);
	const FVector EffectiveWish = ReplicatedControlMode == EDeliveryRagdollControlMode::Recovering
		|| bRisingFromLimp || StartupPlantRemaining > 0.0f || bHitFeetPlanted
		? FVector::ZeroVector : Wish;
	// 先确定本帧髋部打算移动到哪里；脚的目标和身体朝向都以这个结果为基准。
	UpdatePelvisTarget(DeltaTime, EffectiveWish);
	// 空中不跑步态：髋已经被抬高，这时规划落点会让脚去追够不到的地面点。
	// 腿改由各自的父空间角度电机拉回站立姿势，落地后再恢复迈步。
	if (!bRisingFromLimp && !bHitFeetPlanted && !bPunchFeetPlanted && !bJumping && !bPelvisAirborne)
	{
		UpdateFeet(DeltaTime, EffectiveWish);
	}
	const ADeliveryCharacter* GrabCharacter = Cast<ADeliveryCharacter>(GetOwner());
	const bool bCarryingProp = GrabCharacter && GrabCharacter->GetGrabComponent()
		&& GrabCharacter->GetGrabComponent()->IsCarryingProp();
	const bool bDraggingCharacter = GrabCharacter && GrabCharacter->GetGrabComponent()
		&& GrabCharacter->GetGrabComponent()->IsDraggingCharacter();
	DragReachAlpha = FMath::FInterpTo(DragReachAlpha,
		bDraggingCharacter && !bJumping ? 1.0f : 0.0f, DeltaTime, 7.0f);
	BraceTorsoForAction(DeltaTime, bBodyDrivenPunchActive, bCarryingProp);
	FVector GrabGoals[2] = { FVector::ZeroVector, FVector::ZeroVector };
	uint8 GrabMask = 0;
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner()))
	{
		if (const UDeliveryGrabComponent* Grab = Character->GetGrabComponent())
		{
			if (Grab->GetHandGoal(true, GrabGoals[0])) GrabMask |= 1;
			if (Grab->GetHandGoal(false, GrabGoals[1])) GrabMask |= 2;
		}
	}
	BoxingPose.Update(Mesh, PhysicsControl, CurrentFacingYaw, DeltaTime,
		bBodyDrivenPunchActive ? (PunchHandSide > 0 ? 0 : 1) : -1, bBodyDrivenPunchReleased,
		bBodyDrivenPunchActive ? PunchAimDirection : FVector::ZeroVector, GrabGoals, GrabMask);
}

void UDeliveryActiveRagdollComponent::UpdatePelvisTarget(float DeltaTime, const FVector& Wish)
{
	// 输入：Wish、当前刚体速度和地面命中结果。输出：PlannedPelvisTarget 与三个身体电机目标。
	// 中途失去地面时直接关髋部位置电机，避免角色被旧目标悬在空中。
	const ADeliveryCharacter* GrabCharacter = Cast<ADeliveryCharacter>(GetOwner());
	const bool bCarryingProp = GrabCharacter && GrabCharacter->GetGrabComponent()
		&& GrabCharacter->GetGrabComponent()->IsCarryingProp();
	const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
	const FVector Velocity = Mesh->GetPhysicsLinearVelocity(Bones.Hips);
	if (bRisingFromLimp)
	{
		LimpRecoveryRiseAlpha = FMath::Min(1.0f,
			LimpRecoveryRiseAlpha + DeltaTime / FMath::Max(LimpRecoveryRiseDuration, 0.05f));
		SetLimpRecoveryDriveStrength(LimpRecoveryRiseAlpha);
		if (LimpRecoveryRiseAlpha >= 1.0f)
		{
			bRisingFromLimp = false;
			bPendingStopRecovery = true;
		}
	}

	FGroundHit GroundUnderHips;
	bool bHasHipGround = TraceGround(Hips, CurrentGroundNormal, GroundUnderHips);
	if (!bHasHipGround)
	{
		bHasHipGround = TraceGround(Hips, FVector::UpVector, GroundUnderHips);
	}

	// 把水平 Wish 和当前速度都投影到坡面上，用它们的差估计髋要沿坡往前领多远。
	WishOnSlope = FVector::VectorPlaneProject(Wish, CurrentGroundNormal).GetSafeNormal() * Wish.Size();
	const FVector VelocityOnSlope = FVector::VectorPlaneProject(Velocity, CurrentGroundNormal);
	const FVector DesiredVelocity = WishOnSlope * DesiredMoveSpeed;
	const FVector RawLead = ((DesiredVelocity - VelocityOnSlope) * TargetLeadTime)
		.GetClampedToMaxSize(MaxTargetLead);
	SmoothedMoveLead = FMath::VInterpTo(
		SmoothedMoveLead, RawLead, DeltaTime, DriveTargetSmoothingSpeed);
	float GaitPulse = 0.0f;
	float Wobble = 0.0f;
	if (LeftFoot.Alpha < 1.0f)
	{
		GaitPulse = FMath::Square(FMath::Sin(PI * LeftFoot.Alpha));
		Wobble = GaitPulse;
	}
	else if (RightFoot.Alpha < 1.0f)
	{
		GaitPulse = FMath::Square(FMath::Sin(PI * RightFoot.Alpha));
		Wobble = -GaitPulse;
	}

	FVector Target = Hips + SmoothedMoveLead;

	// 原地站立时，以两脚中点作为支撑范围的中心；让全身质心稍向它靠拢，
	// 减少无输入时慢慢倾倒。这个偏移有上限，不能强行把身体拖回原位。
	FVector DesiredBalanceOffset = FVector::ZeroVector;
	if (Wish.IsNearlyZero())
	{
		const FVector SupportCenter = 0.5f * (
			Mesh->GetCenterOfMass(LeftFoot.Bone) + Mesh->GetCenterOfMass(RightFoot.Bone));
		const FVector BodyCenter = GetWholeBodyCenterOfMass();
		DesiredBalanceOffset = FVector::VectorPlaneProject(
			SupportCenter - BodyCenter, CurrentGroundNormal)
			.GetClampedToMaxSize(MaxCenterOfMassCorrection) * CenterOfMassCorrection;
	}
	SmoothedBalanceOffset = FMath::VInterpTo(
		SmoothedBalanceOffset, DesiredBalanceOffset, DeltaTime, BalanceResponseSpeed);
	Target += SmoothedBalanceOffset;

	FVector DesiredNormal = CurrentGroundNormal;
	FVector DesiredGroundPoint = SmoothedGroundPoint;
	// 在预计要去的位置找地面；找不到就试髋正下方，避免坡沿附近目标悬空。
	FGroundHit Ground;
	const bool bHasPlannedGround = SampleGround(Target, Hips, CurrentGroundNormal, Ground);
	if (bHasPlannedGround)
	{
		DesiredNormal = Ground.Normal;
		DesiredGroundPoint = Ground.Point;
	}
	else if (bHasHipGround)
	{
		DesiredNormal = GroundUnderHips.Normal;
		DesiredGroundPoint = GroundUnderHips.Point;
	}
	// 车撞飞后短时没有可站的地面：关闭髋部世界空间电机，避免它继续追着
	// 上一帧的站立点把人吊在空中；落地时重新播种目标再启用。
	if (!bJumping && !bHasPlannedGround && !bHasHipGround)
	{
		if (!bPelvisAirborne)
		{
			bPelvisAirborne = true;
			CancelFootSteps();
		}
		// 从 Limp 恢复时 SetControlsInSetEnabled 会重新打开所有电机，
		// 即使 bPelvisAirborne 早已为 true，这里也必须每次明确关闭髋电机。
		PhysicsControl->SetControlEnabled(PelvisControl, false, true, false);
		return;
	}
	if (bPelvisAirborne)
	{
		bPelvisAirborne = false;
		CurrentGroundNormal = DesiredNormal;
		SmoothedGroundPoint = DesiredGroundPoint;
		PhysicsControl->SetControlEnabled(PelvisControl, true, true, false);
	}

	// 探测结果可以马上变。发给电机的法线和贴地点慢慢靠过去，坡顶接到平面时才不会抽一下。
	CurrentGroundNormal = FMath::VInterpTo(
		CurrentGroundNormal, DesiredNormal, DeltaTime, GroundNormalSmoothingSpeed).GetSafeNormal();
	if (CurrentGroundNormal.IsNearlyZero())
	{
		CurrentGroundNormal = FVector::UpVector;
	}
	SmoothedGroundPoint = FMath::VInterpTo(
		SmoothedGroundPoint, DesiredGroundPoint, DeltaTime, GroundNormalSmoothingSpeed);
	UpdateJumpHeight(DeltaTime);

	// 髋目标等于平滑后的贴地点，再加上站立高度沿法线抬起来。
	const float TargetStandHeight = bRisingFromLimp
		? FMath::Lerp(LimpRecoveryStartHeight, StandHeight,
			FMath::InterpEaseInOut(0.0f, 1.0f, LimpRecoveryRiseAlpha, 2.0f))
		: StandHeight;
	Target = SmoothedGroundPoint + CurrentGroundNormal
		* (TargetStandHeight + SmoothBounceHeight * GaitPulse + JumpOffset);
	// 目标倒地时仅靠站立高度，肩到地面的距离超过手臂长度；平滑屈膝，
	// 让单手真的够到附近的身体表面，而不是永远差一截、建不了约束。
	Target -= CurrentGroundNormal * (DragReachCrouchHeight * DragReachAlpha);

	// 原地支撑时固定髋目标的水平基准，少量前压不再累积成整个人向前上步。
	if (bPunchFeetPlanted)
	{
		Target = PunchSupportLocation + CurrentGroundNormal
			* FVector::DotProduct(Target - PunchSupportLocation, CurrentGroundNormal);
	}
	PunchLunge = FMath::FInterpTo(PunchLunge,
		bBodyDrivenPunchActive && bBodyDrivenPunchReleased ? 1.0f : 0.0f, DeltaTime, PunchLungeSpeed);
	if (PunchLunge > KINDA_SMALL_NUMBER && !PunchAimDirection.IsNearlyZero())
	{
		Target += FVector::VectorPlaneProject(PunchAimDirection, CurrentGroundNormal).GetSafeNormal()
			* (PunchLungeDistance * PunchLunge);
	}

	// 挨打不只是上身晃一下，髋部目标本身也顺着来拳方向往后挪一段再弹回来，
	// 整个人才像真的被打退了一步，而不是脚踩死原地光看上身在倒。
	HitPushAlpha = FMath::FInterpTo(
		HitPushAlpha, bHitFeetPlanted ? 1.0f : 0.0f, DeltaTime, HitReactionPelvisSlideSpeed);
	if (HitPushAlpha > KINDA_SMALL_NUMBER && !HitPushDirection.IsNearlyZero())
	{
		Target += FVector::VectorPlaneProject(HitPushDirection, CurrentGroundNormal).GetSafeNormal()
			* (HitReactionPelvisSlideDistance * HitPushAlpha);
	}

	PlannedPelvisTarget = Target;

	UpdateFacingYaw(DeltaTime, Wish, bCarryingProp);
	WriteBodyRotationTargets(DeltaTime, Wish, Wobble, bCarryingProp, Target);
}

void UDeliveryActiveRagdollComponent::UpdateJumpHeight(float DeltaTime)
{
	// 跳跃只抬高髋部目标，物理刚体仍由电机和重力驱动。
	if (bJumping)
	{
		JumpElapsed += DeltaTime;
		const float Alpha = JumpDuration > KINDA_SMALL_NUMBER
			? FMath::Clamp(JumpElapsed / JumpDuration, 0.0f, 1.0f)
			: 1.0f;
		// 0→1 的抛物线在两端为零、中途达到 JumpHeight；结束后仍靠物理落地。
		JumpOffset = 4.0f * JumpHeight * Alpha * (1.0f - Alpha);
		if (Alpha >= 1.0f)
		{
			bJumping = false;
			JumpOffset = 0.0f;
			CancelFootSteps();
			bWasMoving = false;
			bPendingStopRecovery = true;
		}
	}
	else
	{
		JumpOffset = 0.0f;
	}
}

void UDeliveryActiveRagdollComponent::UpdateFacingYaw(float DeltaTime, const FVector& Wish, bool bCarryingProp)
{
	// 转向优先级：出拳、抓取、受击、普通移动。先选目标角，再平滑改变当前朝向。
	const FVector FacingWish = WishOnSlope.IsNearlyZero() ? Wish : WishOnSlope;
	float DesiredYaw = FacingWish.IsNearlyZero() ? CurrentFacingYaw : FacingWish.GetSafeNormal2D().Rotation().Yaw;
	if (bBodyDrivenPunchActive && !PunchAimDirection.IsNearlyZero())
	{
		// 出拳时身体转向瞄准方向。不转身的话拳头目标会落在肩膀活动范围之外，看起来就只有小臂在动。
		DesiredYaw = bPunchFeetPlanted ? PunchSupportYaw : PunchAimDirection.Rotation().Yaw;
	}
	else if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
		Character && Character->GetGrabComponent() && Character->GetGrabComponent()->IsGrabbing())
	{
		// 手臂追物体世界抓点时，身体也必须面向抓点；只跟镜头转会让手从背后够箱子。
		FVector ToGrip;
		if (Character->GetGrabComponent()->GetGrabFacingDirection(ToGrip))
		{
			DesiredYaw = ToGrip.Rotation().Yaw;
		}
		else if (Character->GetController())
		{
			DesiredYaw = Character->GetController()->GetControlRotation().Yaw;
		}
	}
	else if (bHasHitFacing)
	{
		// 挨打之后转过去面对来拳方向，而不是保持原朝向挨第二拳。
		DesiredYaw = HitFacingYaw;
	}
	const bool bTurningToHit = bHasHitFacing && !bBodyDrivenPunchActive;
	const float YawError = FMath::FindDeltaAngleDegrees(CurrentFacingYaw, DesiredYaw);
	CurrentFacingYaw = FMath::UnwindDegrees(bCarryingProp
		? CurrentFacingYaw + FMath::Clamp(YawError, -CarryTurnRate * DeltaTime, CarryTurnRate * DeltaTime)
		: FMath::FInterpTo(CurrentFacingYaw, CurrentFacingYaw + YawError, DeltaTime,
			bTurningToHit ? HitReactionFacingTurnSpeed : TurnResponsiveness));
	if (!Wish.IsNearlyZero())
	{
		LastWishDirection = FRotator(0.0f, CurrentFacingYaw, 0.0f).Vector();
	}
}

void UDeliveryActiveRagdollComponent::WriteBodyRotationTargets(float DeltaTime, const FVector& Wish, float Wobble, bool bCarryingProp, const FVector& Target)
{
	// 胸部可以围绕骨盆拧身，骨盆仍负责站稳；最后把目标写给髋、胸、头电机。
	const ADeliveryCharacter* GrabCharacter = Cast<ADeliveryCharacter>(GetOwner());
	const FVector StanceUp = FMath::Lerp(FVector::UpVector, CurrentGroundNormal, 0.45f).GetSafeNormal();
	// 先把缓存的站立姿势略微贴合坡面，再叠加转身和前倾；坡面法线不直接
	// 完全接管身体竖直方向，否则稍陡的坡就会把上半身也大幅带歪。
	const FQuat SlopeAlign = FQuat::FindBetweenNormals(FVector::UpVector, StanceUp);
	const FQuat YawDelta(CurrentGroundNormal, FMath::DegreesToRadians(
		FMath::FindDeltaAngleDegrees(ReferenceFacingYaw, CurrentFacingYaw)));
	FQuat Lean = FQuat::Identity;
	const FVector SlopeForward = GetSlopeForward(Wish);
	const FVector SlopeRight = GetSlopeRight(Wish);
	const bool bHasMoveWish = !Wish.IsNearlyZero();
	if (bHasMoveWish && !bHadMoveWishLastTick)
	{
		// 只在起步时给一次前倾，匀速下坡时不再不断追加前倾。
		AccelerationLeanRemaining = AccelerationLeanDuration;
	}
	else if (!bHasMoveWish)
	{
		AccelerationLeanRemaining = 0.0f;
	}
	bHadMoveWishLastTick = bHasMoveWish;
	const float StartupLeanAlpha = AccelerationLeanDuration > KINDA_SMALL_NUMBER
		? AccelerationLeanRemaining / AccelerationLeanDuration : 0.0f;
	SmoothedAccelerationAlpha = FMath::FInterpTo(
		SmoothedAccelerationAlpha, StartupLeanAlpha, DeltaTime, DriveTargetSmoothingSpeed);
	AccelerationLeanRemaining = FMath::Max(0.0f, AccelerationLeanRemaining - DeltaTime);
	if (!Wish.IsNearlyZero())
	{
		const float LeanRadians = FMath::DegreesToRadians(
			AccelerationLeanAngle * SmoothedAccelerationAlpha * FMath::Clamp(Wish.Size(), 0.0f, 1.0f));
		const float WobbleRadians = FMath::DegreesToRadians(
			BouncyPelvisWobbleAngle * Wobble * (bCarryingProp || bBodyDrivenPunchActive ? 0.2f : 1.0f));
		const FVector DesiredUp = (StanceUp
			+ WishOnSlope * FMath::Tan(LeanRadians)
			+ SlopeRight * FMath::Tan(WobbleRadians)).GetSafeNormal();
		Lean = FQuat::FindBetweenNormals(StanceUp, DesiredUp);
	}
	if (DragReachAlpha > KINDA_SMALL_NUMBER)
	{
		FVector DragForward;
		if (GrabCharacter && GrabCharacter->GetGrabComponent()
			&& GrabCharacter->GetGrabComponent()->GetGrabFacingDirection(DragForward))
		{
			const FVector LeanUp = (StanceUp + DragForward.GetSafeNormal2D()
				* FMath::Tan(FMath::DegreesToRadians(DragReachLeanAngle * DragReachAlpha))).GetSafeNormal();
			Lean = FQuat::FindBetweenNormals(StanceUp, LeanUp) * Lean;
		}
	}

	// 纯水平拧身，不弯腰也不侧倾：蓄力时出拳侧的肩膀往后拧，释放后拧回来再带出去一点。
	// 正的 Yaw 会把左肩转向前方，所以左拳（PunchHandSide 为正）蓄力要给负角度。
	const float DesiredPunchTwist = !bBodyDrivenPunchActive ? 0.0f
		: PunchHandSide * (bBodyDrivenPunchReleased ? PunchFollowThroughAngle : -PunchSideStanceAngle);
	// 这个目标角度是阶跃的：出拳、释放、收拳各跳一次。胸腰电机很硬，把阶跃直接喂进去
	// 会把整个上半身连着伸出去的手臂横甩过去，看着就是在扇耳光。必须平滑。
	PunchTwist = FMath::FInterpTo(PunchTwist, DesiredPunchTwist, DeltaTime, PunchTwistSpeed);
	const FQuat PunchSideRotation = FMath::IsNearlyZero(PunchTwist)
		? FQuat::Identity
		: FQuat(CurrentGroundNormal, FMath::DegreesToRadians(PunchTwist));
	const FQuat PelvisRotation = Lean * YawDelta * SlopeAlign * ReferencePelvisRotation;
	// 蓄力和释放的拧身由腰胸完成，骨盆不跟着左右扭动两条腿。
	FQuat SpineRelativeRotation = (PelvisRotation.Inverse() * PunchSideRotation
		* PelvisRotation * ReferenceSpineRelativeRotation).GetNormalized();
	if (!bCarryingProp && !bBodyDrivenPunchActive && !Wish.IsNearlyZero() && !FMath::IsNearlyZero(Wobble)
		&& !SlopeForward.IsNearlyZero())
	{
		const float SwingRadians = FMath::DegreesToRadians(-LooseTorsoSwingAngle * Wobble);
		const float TwistRadians = FMath::DegreesToRadians(LooseTorsoSwingAngle * 0.35f * Wobble);
		const FQuat SpineWorldRotation =
			PunchSideRotation * FQuat(CurrentGroundNormal, TwistRadians)
			* FQuat(SlopeForward, SwingRadians)
			* PelvisRotation
			* ReferenceSpineRelativeRotation;
		SpineRelativeRotation = PelvisRotation.Inverse() * SpineWorldRotation;
		SpineRelativeRotation.Normalize();
	}

	// 至此只算出了目标旋转；真正让身体朝目标靠近的是下面的物理电机。
	// 髋用世界位置目标，胸/头主要使用相对旋转目标，所以后二者位置传零。
	PhysicsControl->SetControlTargetPositionAndOrientation(
		PelvisControl, Target, PelvisRotation.Rotator(), DeltaTime, true, true, true, false);
	PhysicsControl->SetControlTargetPositionAndOrientation(
		ChestControl, FVector::ZeroVector, SpineRelativeRotation.Rotator(), DeltaTime, true, false, true, false);

	const float StandingAlpha = 1.0f - FMath::Clamp(Wish.Size(), 0.0f, 1.0f);
	const FQuat BodyRotationDelta = PelvisRotation * ReferencePelvisRotation.Inverse();
	const FQuat HeadCorrection(
		SlopeRight, FMath::DegreesToRadians(StandingHeadCorrectionAngle * StandingAlpha));
	const FQuat HeadRotation = HeadCorrection * BodyRotationDelta * ReferenceHeadRotation;
	PhysicsControl->SetControlTargetPositionAndOrientation(
		HeadControl, FVector::ZeroVector, HeadRotation.Rotator(), DeltaTime, true, false, true, false);
}

FVector UDeliveryActiveRagdollComponent::GetWholeBodyCenterOfMass() const
{
	// 用物理资产中各刚体的质量加权求平均，而不是直接拿髋部当全身中心。
	const UPhysicsAsset* PhysicsAsset = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
	if (!PhysicsAsset)
	{
		return Mesh ? Mesh->GetComponentLocation() : FVector::ZeroVector;
	}

	FVector WeightedCenter = FVector::ZeroVector;
	float TotalMass = 0.0f;
	for (const TObjectPtr<USkeletalBodySetup>& Setup : PhysicsAsset->SkeletalBodySetups)
	{
		if (!Setup)
		{
			continue;
		}

		const FBodyInstance* Body = Mesh->GetBodyInstance(Setup->BoneName);
		if (!Body)
		{
			continue;
		}

		const float Mass = Body->GetBodyMass();
		if (Mass <= UE_SMALL_NUMBER)
		{
			continue;
		}

		WeightedCenter += Mesh->GetCenterOfMass(Setup->BoneName) * Mass;
		TotalMass += Mass;
	}

	return TotalMass > UE_SMALL_NUMBER ? WeightedCenter / TotalMass : Mesh->GetComponentLocation();
}
