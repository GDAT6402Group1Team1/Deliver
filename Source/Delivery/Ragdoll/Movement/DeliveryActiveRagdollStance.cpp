// 计算身体的站立、朝向和髋部目标，再交给物理控制器追赶
// 身体姿态：输入方向 → 贴地的髋部目标 → 面向和上身旋转 → 物理电机
// 目标只是电机想去的位置；角色刚体的位置仍由物理模拟决定
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Combat/DeliveryPunchTrajectory.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Math/RotationMatrix.h"
#include "PhysicsControlComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

// 身体目标：把当前输入和状态整理为电机目标；目标位置不等于身体已经到达的位置；真实身体还受碰撞与约束影响
void UDeliveryActiveRagdollComponent::UpdateControlTargets(float DeltaTime)
{
	// 低帧率时物理模拟会限制步长，脚步也按相同步长推进，避免脚还没落地就超时
	const UPhysicsSettings* PhysicsSettings = UPhysicsSettings::Get();
	const float MaxStep = PhysicsSettings->bSubstepping
	    ? PhysicsSettings->MaxSubstepDeltaTime * PhysicsSettings->MaxSubsteps
	    : PhysicsSettings->MaxPhysicsDeltaTime;
	if (MaxStep > 0.0f)
		DeltaTime = FMath::Min(DeltaTime, MaxStep);

	// 这是每帧控制入口；先让髋部选定落点，双脚才能依据同一个目标规划步伐
	if (GetOwner() && GetOwner()->HasAuthority() && GetOwner()->GetLocalRole() == ROLE_Authority)
	{
		const APawn* Pawn = Cast<APawn>(GetOwner());
		if (Pawn && !Pawn->IsLocallyControlled() && GetWorld() &&
		    GetWorld()->GetTimeSeconds() - Movement.LastMoveInputTime > ServerInputTimeout)
		{
			// 高频移动 RPC 允许丢包，超时归零可避免松键包丢失后继续移动
			Movement.MoveInput = FVector2D::ZeroVector;
		}
	}

	const FVector Wish = GetWishDir();
	// 起步事件取玩家意图，不能取下面被恢复/脚步超时临时归零的 EffectiveWish
	// 否则一直按住移动也会在每次松脚后重新触发前倾
	const bool bHasMoveWish = !Wish.IsNearlyZero();
	if (bHasMoveWish && !Movement.bHadMoveWishLastTick)
	{
		Movement.AccelerationLeanRemaining = AccelerationLeanDuration;
	}
	else if (!bHasMoveWish)
	{
		Movement.AccelerationLeanRemaining = 0.0f;
	}
	Movement.bHadMoveWishLastTick = bHasMoveWish;
	Movement.AccelerationLeanRemaining = FMath::Max(0.0f, Movement.AccelerationLeanRemaining - DeltaTime);
	Movement.BlockedStepRecoveryRemaining = FMath::Max(0.f, Movement.BlockedStepRecoveryRemaining - DeltaTime);
	if (bPunchFeetPlanted && FVector::Dist2D(Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace),
	                                         PunchSupportLocation) > PunchSupportBreakDistance)
	{
		bPunchSupportInterrupted = true;
	}
	// 只有站稳、没移动且面向拳路时才暂时支撑双脚；走动、跳起、受击或身体已经
	// 离开原支撑位置后必须松开，否则脚电机会把正在移动/下落的身体钉在原地
	SetPunchFeetPlanted(
	    Punch.IsActive() && !bPunchSupportInterrupted && !bHitFeetPlanted && !bIsLimp && IsGrounded() &&
	    Wish.IsNearlyZero() && GetUprightDot() > 0.65f &&
	    (bPunchFeetPlanted || Mesh->GetPhysicsLinearVelocity(Bones.Hips).Size2D() < 80.0f) &&
	    FMath::Abs(FMath::FindDeltaAngleDegrees(CurrentFacingYaw, Punch.GetDirection().Rotation().Yaw)) < 35.0f);
	const FVector EffectiveWish = ReplicatedControlMode == EDeliveryRagdollControlMode::Recovering ||
	                                      Recovery.IsRising() || Movement.StartupPlantRemaining > 0.0f ||
	                                      bHitFeetPlanted || Movement.BlockedStepRecoveryRemaining > 0.0f
	                                  ? FVector::ZeroVector
	                                  : Wish;
	// 先确定本帧髋部打算移动到哪里；脚的目标和身体朝向都以这个结果为基准
	UpdatePelvisTarget(DeltaTime, EffectiveWish);
	// 空中不跑步态：髋已经被抬高，这时规划落点会让脚去追够不到的地面点
	// 腿改由各自的父空间角度电机拉回站立姿势，落地后再恢复迈步
	if (!Recovery.IsRising() && !bHitFeetPlanted && !bPunchFeetPlanted && !Movement.bJumping && !Movement.bPelvisAirborne)
	{
		UpdateFeet(DeltaTime, EffectiveWish);
	}
	const ADeliveryCharacter* GrabCharacter = Cast<ADeliveryCharacter>(GetOwner());
	const bool bCarryingProp = GrabCharacter && GrabCharacter->GetGrabComponent() &&
	                           GrabCharacter->GetGrabComponent()->IsCarryingProp();
	const bool bDraggingCharacter = GrabCharacter && GrabCharacter->GetGrabComponent() &&
	                                GrabCharacter->GetGrabComponent()->IsDraggingCharacter();
	DragReachAlpha =
	    FMath::FInterpTo(DragReachAlpha, bDraggingCharacter && !Movement.bJumping ? 1.0f : 0.0f, DeltaTime, 7.0f);
	FDeliveryAnimationArmPose AnimationPose;
	const bool bAxeSwing = SampleAxeSwing(AnimationPose);
	// EndPunch 结束的是攻击窗口，手臂电机此时还在收拳；支撑必须覆盖收拳阶段
	BraceTorsoForAction(DeltaTime,
	                    Punch.IsActive() || BoxingPose.HasPendingPunchRecovery() || bAxeSwing,
	                    bCarryingProp);
	FVector GrabGoals[2] = {FVector::ZeroVector, FVector::ZeroVector};
	uint8 GrabMask = 0;
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner()))
	{
		if (const UDeliveryGrabComponent* Grab = Character->GetGrabComponent())
		{
			if (Grab->GetHandGoal(true, GrabGoals[0]))
				GrabMask |= 1;
			if (Grab->GetHandGoal(false, GrabGoals[1]))
				GrabMask |= 2;
		}
	}
	BoxingPose.Update(Mesh, PhysicsControl, CurrentFacingYaw, DeltaTime,
	                  Punch.IsActive() ? (Punch.GetHandSide() > 0 ? 0 : 1) : -1, Punch.IsReleased(),
	                  Punch.IsActive() ? Punch.GetDirection() : FVector::ZeroVector, GrabGoals, GrabMask,
	                  bAxeSwing ? &AnimationPose : nullptr);
}

// 计算髋部的高度和水平目标，结合移动、坡面与身体状态更新控制
void UDeliveryActiveRagdollComponent::UpdatePelvisTarget(float DeltaTime, const FVector& Wish)
{
	// 输入：Wish、当前刚体速度和地面命中结果；输出：PlannedPelvisTarget 与三个身体电机目标
	// 中途失去地面时直接关髋部位置电机，避免角色被旧目标悬在空中
	const ADeliveryCharacter* GrabCharacter = Cast<ADeliveryCharacter>(GetOwner());
	const bool bCarryingProp = GrabCharacter && GrabCharacter->GetGrabComponent() &&
	                           GrabCharacter->GetGrabComponent()->IsCarryingProp();
	const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
	const FVector Velocity = Mesh->GetPhysicsLinearVelocity(Bones.Hips);
	if (Recovery.IsRising())
	{
		Recovery.AdvanceRise(DeltaTime, LimpRecoveryRiseDuration);
		SetLimpRecoveryDriveStrength(Recovery.GetRiseAlpha());
		if (Recovery.GetRiseAlpha() >= 1.0f)
		{
			Recovery.FinishRise();
			Movement.bPendingStopRecovery = true;
		}
	}

	FGroundHit GroundUnderHips;
	bool bHasHipGround = TraceGround(Hips, Movement.CurrentGroundNormal, GroundUnderHips);
	if (!bHasHipGround)
	{
		bHasHipGround = TraceGround(Hips, FVector::UpVector, GroundUnderHips);
	}

	// 把水平 Wish 和当前速度都投影到坡面上，用它们的差估计髋要沿坡往前领多远
	Movement.WishOnSlope = FVector::VectorPlaneProject(Wish, Movement.CurrentGroundNormal).GetSafeNormal() * Wish.Size();
	const FVector VelocityOnSlope = FVector::VectorPlaneProject(Velocity, Movement.CurrentGroundNormal);
	const FVector DesiredVelocity = Movement.WishOnSlope * DesiredMoveSpeed;
	const FVector RawLead =
	    ((DesiredVelocity - VelocityOnSlope) * TargetLeadTime).GetClampedToMaxSize(MaxTargetLead);
	Movement.SmoothedMoveLead = FMath::VInterpTo(Movement.SmoothedMoveLead, RawLead, DeltaTime, DriveTargetSmoothingSpeed);
	float GaitPulse = 0.0f;
	float Wobble = 0.0f;
	if (Movement.LeftFoot.Alpha < 1.0f)
	{
		GaitPulse = FMath::Square(FMath::Sin(PI * Movement.LeftFoot.Alpha));
		Wobble = GaitPulse;
	}
	else if (Movement.RightFoot.Alpha < 1.0f)
	{
		GaitPulse = FMath::Square(FMath::Sin(PI * Movement.RightFoot.Alpha));
		Wobble = -GaitPulse;
	}

	FVector Target = Hips + Movement.SmoothedMoveLead;

	// 原地站立时，以两脚中点作为支撑范围的中心；让全身质心稍向它靠拢，
	// 减少无输入时慢慢倾倒；这个偏移有上限，不能强行把身体拖回原位
	FVector DesiredBalanceOffset = FVector::ZeroVector;
	if (Wish.IsNearlyZero())
	{
		const FVector SupportCenter =
		    0.5f * (Mesh->GetCenterOfMass(Movement.LeftFoot.Bone) + Mesh->GetCenterOfMass(Movement.RightFoot.Bone));
		const FVector BodyCenter = GetWholeBodyCenterOfMass();
		DesiredBalanceOffset = FVector::VectorPlaneProject(SupportCenter - BodyCenter, Movement.CurrentGroundNormal)
		                           .GetClampedToMaxSize(MaxCenterOfMassCorrection) *
		                       CenterOfMassCorrection;
	}
	Movement.SmoothedBalanceOffset =
	    FMath::VInterpTo(Movement.SmoothedBalanceOffset, DesiredBalanceOffset, DeltaTime, BalanceResponseSpeed);
	Target += Movement.SmoothedBalanceOffset;

	// 松开移动后保留站立位置，避免目标跟着身体偏移，造成滑步和反复补脚
	// 只固定身体控制目标，受击、起跳或明显偏离时解除
	const bool bCanStandStill = GetWishDir().IsNearlyZero() && !Recovery.IsRising() && !Movement.bJumping &&
	                         !Movement.bPelvisAirborne && !Punch.IsActive() && !bHitFeetPlanted &&
	                         ReplicatedControlMode == EDeliveryRagdollControlMode::Active &&
	                         GetUprightDot() > 0.65f && bHasHipGround &&
	                         Movement.CurrentGroundNormal.Z > FMath::Cos(FMath::DegreesToRadians(TumbleSlopeDegrees));
	if (!bCanStandStill)
		Movement.bStandAnchored = false;
	else
	{
		if (Movement.bStandAnchored && bHasHipGround)
			Movement.StandAnchor += GroundUnderHips.SurfaceVelocity * DeltaTime;
		if (!Movement.bStandAnchored || FVector::Dist2D(Hips, Movement.StandAnchor) > 80.f)
		{
			Movement.StandAnchor = Hips;
			Movement.bStandAnchored = true;
		}
		Target = Movement.StandAnchor +
		         Movement.CurrentGroundNormal * FVector::DotProduct(Target - Movement.StandAnchor, Movement.CurrentGroundNormal);
	}

	FVector DesiredNormal = Movement.CurrentGroundNormal;
	FVector DesiredGroundPoint = Movement.SmoothedGroundPoint;
	// 在预计要去的位置找地面；找不到就试髋正下方，避免坡沿附近目标悬空
	FGroundHit Ground;
	const bool bHasPlannedGround = SampleGround(Target, Hips, Movement.CurrentGroundNormal, Ground);
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
	// 上一帧的站立点把人吊在空中；落地时重新播种目标再启用
	if (!Movement.bJumping && !bHasPlannedGround && !bHasHipGround)
	{
		if (!Movement.bPelvisAirborne)
		{
			Movement.bPelvisAirborne = true;
			CancelFootSteps();
		}
		// 从 Limp 恢复时 SetControlsInSetEnabled 会重新打开所有电机，
		// 即使 bPelvisAirborne 早已为 真，这里也必须每次明确关闭髋电机
		PhysicsControl->SetControlEnabled(PelvisControl, false, true, false);
		return;
	}
	if (Movement.bPelvisAirborne)
	{
		Movement.bPelvisAirborne = false;
		Movement.CurrentGroundNormal = DesiredNormal;
		Movement.SmoothedGroundPoint = DesiredGroundPoint;
		PhysicsControl->SetControlEnabled(PelvisControl, true, true, false);
	}

	// 探测结果可以马上变；发给电机的法线和贴地点慢慢靠过去，坡顶接到平面时才不会抽一下
	Movement.CurrentGroundNormal =
	    FMath::VInterpTo(Movement.CurrentGroundNormal, DesiredNormal, DeltaTime, GroundNormalSmoothingSpeed)
	        .GetSafeNormal();
	if (Movement.CurrentGroundNormal.IsNearlyZero())
	{
		Movement.CurrentGroundNormal = FVector::UpVector;
	}
	Movement.SmoothedGroundPoint =
	    FMath::VInterpTo(Movement.SmoothedGroundPoint, DesiredGroundPoint, DeltaTime, GroundNormalSmoothingSpeed);
	UpdateJumpHeight(DeltaTime);

	// 髋目标等于平滑后的贴地点，再加上站立高度沿法线抬起来
	const float TargetStandHeight =
	    Recovery.IsRising() ? FMath::Lerp(Recovery.GetStartHeight(), StandHeight,
		                              FMath::InterpEaseInOut(0.0f, 1.0f, Recovery.GetRiseAlpha(), 2.0f))
		                : StandHeight;
	Target = Movement.SmoothedGroundPoint +
	         Movement.CurrentGroundNormal * (TargetStandHeight + SmoothBounceHeight * GaitPulse + Movement.JumpOffset);
	// 目标倒地时仅靠站立高度，肩到地面的距离超过手臂长度；平滑屈膝，
	// 让单手真的够到附近的身体表面，而不是永远差一截、建不了约束
	Target -= Movement.CurrentGroundNormal * (DragReachCrouchHeight * DragReachAlpha);

	// 原地支撑时固定髋目标的水平基准，少量前压不再累积成整个人向前上步
	if (bPunchFeetPlanted)
	{
		Target =
		    PunchSupportLocation +
		    Movement.CurrentGroundNormal * FVector::DotProduct(Target - PunchSupportLocation, Movement.CurrentGroundNormal);
	}
	PunchLunge =
	    FMath::FInterpTo(PunchLunge, Punch.IsActive() && Punch.IsReleased() ? 1.0f : 0.0f,
		                 DeltaTime, PunchLungeSpeed);
	if (PunchLunge > KINDA_SMALL_NUMBER && !Punch.GetDirection().IsNearlyZero())
	{
		Target += FVector::VectorPlaneProject(Punch.GetDirection(), Movement.CurrentGroundNormal).GetSafeNormal() *
		          (PunchLungeDistance * PunchLunge);
	}

	// 挨打不只是上身晃一下，髋部目标本身也顺着来拳方向往后挪一段再弹回来，
	// 整个人才像真的被打退了一步，而不是脚踩死原地光看上身在倒
	HitPushAlpha =
	    FMath::FInterpTo(HitPushAlpha, bHitFeetPlanted ? 1.0f : 0.0f, DeltaTime, HitReactionPelvisSlideSpeed);
	if (HitPushAlpha > KINDA_SMALL_NUMBER && !HitPushDirection.IsNearlyZero())
	{
		Target += FVector::VectorPlaneProject(HitPushDirection, Movement.CurrentGroundNormal).GetSafeNormal() *
		          (HitReactionPelvisSlideDistance * HitPushAlpha);
	}

	Movement.PlannedPelvisTarget = Target;

	UpdateFacingYaw(DeltaTime, Wish, bCarryingProp);
	WriteBodyRotationTargets(DeltaTime, Wish, Wobble, bCarryingProp, Target);
}

// 更新跳跃阶段对身体高度的影响
void UDeliveryActiveRagdollComponent::UpdateJumpHeight(float DeltaTime)
{
	// 跳跃只抬高髋部目标，物理刚体仍由电机和重力驱动
	if (Movement.bJumping)
	{
		Movement.JumpElapsed += DeltaTime;
		const float Alpha =
		    JumpDuration > KINDA_SMALL_NUMBER ? FMath::Clamp(Movement.JumpElapsed / JumpDuration, 0.0f, 1.0f) : 1.0f;
		// 0→1 的抛物线在两端为零、中途达到 JumpHeight；结束后仍靠物理落地
		Movement.JumpOffset = 4.0f * JumpHeight * Alpha * (1.0f - Alpha);
		if (Alpha >= 1.0f)
		{
			Movement.bJumping = false;
			Movement.JumpOffset = 0.0f;
			CancelFootSteps();
			Movement.bWasMoving = false;
			Movement.bPendingStopRecovery = true;
		}
	}
	else
	{
		Movement.JumpOffset = 0.0f;
	}
}

// 平滑更新身体朝向，协调移动、抓取和攻击方向
void UDeliveryActiveRagdollComponent::UpdateFacingYaw(float DeltaTime, const FVector& Wish,
                                                      bool bCarryingProp)
{
	// 转向优先级：出拳、抓取、受击、普通移动；先选目标角，再平滑改变当前朝向
	const FVector FacingWish = Movement.WishOnSlope.IsNearlyZero() ? Wish : Movement.WishOnSlope;
	float DesiredYaw =
	    FacingWish.IsNearlyZero() ? CurrentFacingYaw : FacingWish.GetSafeNormal2D().Rotation().Yaw;
	if (Punch.IsActive() && !Punch.GetDirection().IsNearlyZero())
	{
		// 出拳时身体转向瞄准方向；不转身的话拳头目标会落在肩膀活动范围之外，看起来就只有小臂在动
		DesiredYaw = bPunchFeetPlanted ? PunchSupportYaw : Punch.GetDirection().Rotation().Yaw;
	}
	else if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
	         Character && Character->GetGrabComponent() && Character->GetGrabComponent()->IsGrabbing())
	{
		// 手臂追物体世界抓点时，身体也必须面向抓点；只跟镜头转会让手从背后够箱子
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
		// 挨打之后转过去面对来拳方向，而不是保持原朝向挨第二拳
		DesiredYaw = HitFacingYaw;
	}
	const bool bTurningToHit = bHasHitFacing && !Punch.IsActive();
	const float YawError = FMath::FindDeltaAngleDegrees(CurrentFacingYaw, DesiredYaw);
	CurrentFacingYaw = FMath::UnwindDegrees(
	    bCarryingProp
	        ? CurrentFacingYaw + FMath::Clamp(YawError, -CarryTurnRate * DeltaTime, CarryTurnRate * DeltaTime)
	        : FMath::FInterpTo(CurrentFacingYaw, CurrentFacingYaw + YawError, DeltaTime,
	                           bTurningToHit ? HitReactionFacingTurnSpeed : TurnResponsiveness));
	if (!Wish.IsNearlyZero())
	{
		Movement.LastWishDirection = FRotator(0.0f, CurrentFacingYaw, 0.0f).Vector();
	}
}

// 把期望朝向写入身体各部位的旋转控制
void UDeliveryActiveRagdollComponent::WriteBodyRotationTargets(float DeltaTime, const FVector& Wish,
                                                               float Wobble, bool bCarryingProp,
                                                               const FVector& Target)
{
	// 胸部可以围绕骨盆拧身，骨盆仍负责站稳；最后把目标写给髋、胸、头电机
	const ADeliveryCharacter* GrabCharacter = Cast<ADeliveryCharacter>(GetOwner());
	const FVector StanceUp = FMath::Lerp(FVector::UpVector, Movement.CurrentGroundNormal, 0.45f).GetSafeNormal();
	// 先把缓存的站立姿势略微贴合坡面，再叠加转身和前倾；坡面法线不直接
	// 完全接管身体竖直方向，否则稍陡的坡就会把上半身也大幅带歪
	const FQuat SlopeAlign = FQuat::FindBetweenNormals(FVector::UpVector, StanceUp);
	const FQuat YawDelta(Movement.CurrentGroundNormal, FMath::DegreesToRadians(FMath::FindDeltaAngleDegrees(
	                                              ReferenceFacingYaw, CurrentFacingYaw)));
	FQuat Lean = FQuat::Identity;
	const FVector SlopeForward = GetSlopeForward(Wish);
	const FVector SlopeRight = GetSlopeRight(Wish);
	const float StartupLeanAlpha = AccelerationLeanDuration > KINDA_SMALL_NUMBER
	                                   ? Movement.AccelerationLeanRemaining / AccelerationLeanDuration
	                                   : 0.0f;
	Movement.SmoothedAccelerationAlpha =
	    FMath::FInterpTo(Movement.SmoothedAccelerationAlpha, StartupLeanAlpha, DeltaTime, DriveTargetSmoothingSpeed);
	if (!Wish.IsNearlyZero())
	{
		const float LeanRadians = FMath::DegreesToRadians(AccelerationLeanAngle * Movement.SmoothedAccelerationAlpha *
		                                                  FMath::Clamp(Wish.Size(), 0.0f, 1.0f));
		const float WobbleRadians = FMath::DegreesToRadians(
		    BouncyPelvisWobbleAngle * Wobble * (bCarryingProp || Punch.IsActive() ? 0.2f : 1.0f));
		const FVector DesiredUp =
		    (StanceUp + Movement.WishOnSlope * FMath::Tan(LeanRadians) + SlopeRight * FMath::Tan(WobbleRadians))
		        .GetSafeNormal();
		Lean = FQuat::FindBetweenNormals(StanceUp, DesiredUp);
	}
	if (DragReachAlpha > KINDA_SMALL_NUMBER)
	{
		FVector DragForward;
		if (GrabCharacter && GrabCharacter->GetGrabComponent() &&
		    GrabCharacter->GetGrabComponent()->GetGrabFacingDirection(DragForward))
		{
			const FVector LeanUp =
			    (StanceUp + DragForward.GetSafeNormal2D() *
				                FMath::Tan(FMath::DegreesToRadians(DragReachLeanAngle * DragReachAlpha)))
			        .GetSafeNormal();
			Lean = FQuat::FindBetweenNormals(StanceUp, LeanUp) * Lean;
		}
	}

	// 胸腰转体配合沿拳路的小幅前后摆，骨盆保持原本的平衡目标
	// 正的 Yaw 会把左肩转向前方，所以左拳（PunchHandSide 为正）蓄力要给负角度
	const float DesiredPunchTwist =
	    !Punch.IsActive()
	        ? 0.0f
	        : Punch.GetHandSide() * (Punch.IsReleased() ? PunchFollowThroughAngle : -PunchSideStanceAngle);
	// 这个目标角度是阶跃的：出拳、释放、收拳各跳一次；胸腰电机很硬，把阶跃直接喂进去
	// 会把整个上半身连着伸出去的手臂横甩过去，看着就是在扇耳光；必须平滑
	PunchTwist = FMath::FInterpTo(PunchTwist, DesiredPunchTwist, DeltaTime, PunchTwistSpeed);
	const float DesiredPunchLean =
	    !Punch.IsActive() || bCarryingProp
	        ? 0.f
	        : (Punch.IsReleased() ? PunchTorsoFollowLeanAngle : -PunchTorsoWindupLeanAngle);
	PunchTorsoLean = FMath::FInterpTo(PunchTorsoLean, DesiredPunchLean, DeltaTime, PunchTwistSpeed);
	const FQuat PunchSideRotation = DeliveryPunchTrajectory::TorsoSway(Movement.CurrentGroundNormal, Punch.GetDirection(),
	                                                                   PunchTwist, PunchTorsoLean);
	const FQuat PelvisRotation = Lean * YawDelta * SlopeAlign * ReferencePelvisRotation;
	// 蓄力和释放的拧身由腰胸完成，骨盆不跟着左右扭动两条腿
	FQuat SpineRelativeRotation =
	    (PelvisRotation.Inverse() * PunchSideRotation * PelvisRotation * ReferenceSpineRelativeRotation)
	        .GetNormalized();
	if (!bCarryingProp && !Punch.IsActive() && !Wish.IsNearlyZero() && !FMath::IsNearlyZero(Wobble) &&
	    !SlopeForward.IsNearlyZero())
	{
		const float SwingRadians = FMath::DegreesToRadians(-LooseTorsoSwingAngle * Wobble);
		const float TwistRadians = FMath::DegreesToRadians(LooseTorsoSwingAngle * 0.35f * Wobble);
		const FQuat SpineWorldRotation = PunchSideRotation * FQuat(Movement.CurrentGroundNormal, TwistRadians) *
		                                 FQuat(SlopeForward, SwingRadians) * PelvisRotation *
		                                 ReferenceSpineRelativeRotation;
		SpineRelativeRotation = PelvisRotation.Inverse() * SpineWorldRotation;
		SpineRelativeRotation.Normalize();
	}

	// 至此只算出了目标旋转；真正让身体朝目标靠近的是下面的物理电机
	// 髋用世界位置目标，胸/头主要使用相对旋转目标，所以后二者位置传零
	PhysicsControl->SetControlTargetPositionAndOrientation(PelvisControl, Target, PelvisRotation.Rotator(),
	                                                       DeltaTime, true, true, true, false);
	PhysicsControl->SetControlTargetPositionAndOrientation(ChestControl, FVector::ZeroVector,
	                                                       SpineRelativeRotation.Rotator(), DeltaTime, true,
	                                                       false, true, false);

	const float StandingAlpha = 1.0f - FMath::Clamp(Wish.Size(), 0.0f, 1.0f);
	const FQuat BodyRotationDelta = PelvisRotation * ReferencePelvisRotation.Inverse();
	const FQuat HeadCorrection(SlopeRight,
	                           FMath::DegreesToRadians(StandingHeadCorrectionAngle * StandingAlpha));
	const FQuat HeadRotation = HeadCorrection * BodyRotationDelta * ReferenceHeadRotation;
	PhysicsControl->SetControlTargetPositionAndOrientation(
	    HeadControl, FVector::ZeroVector, HeadRotation.Rotator(), DeltaTime, true, false, true, false);
}

// 按各刚体质量计算整体重心
FVector UDeliveryActiveRagdollComponent::GetWholeBodyCenterOfMass() const
{
	// 用物理资产中各刚体的质量加权求平均，而不是直接拿髋部当全身中心
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
