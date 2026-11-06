// 安排左右脚的支撑与迈步，并限制落脚位置和坡面滑动
// 脚步：从髋部目标推算两脚落点，沿坡面探测真实地面，再把摆动目标交给脚电机
// 脚的实际位置由物理模拟给出，因此落点与刚体之间允许短暂误差
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Ragdoll/DeliveryFootPlacement.h"
#include "Character/DeliveryCharacter.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Math/RotationMatrix.h"
#include "PhysicsControlComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Kismet/GameplayStatics.h"

// 身体左右轴取真实骨盆朝向，不随输入方向突然翻转
void UDeliveryActiveRagdollComponent::UpdateFootRecovery(float DeltaTime)
{
    const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
    const bool bGrabbing = Character && Character->GetGrabComponent() && Character->GetGrabComponent()->IsGrabbing();
    const bool bCanCorrect = !bIsLimp && !Recovery.IsRising() &&
        ReplicatedControlMode == EDeliveryRagdollControlMode::Active &&
        !Movement.bJumping && !Movement.bPelvisAirborne && Movement.StartupPlantRemaining <= 0.f &&
        !bHitFeetPlanted && !Punch.IsActive() && !BoxingPose.HasPendingPunchRecovery() &&
        !SwingAxe.IsValid() && !bGrabbing && GetUprightDot() >= MinimumStepUprightDot;
    if (!bCanCorrect)
    {
        // 战斗和空中状态优先，不能保留交叉计时并在落地后立即补判
        const bool bWasCorrecting = Recovery.IsCorrectingFeet();
        Recovery.CancelFootCorrection();
        if (bWasCorrecting) CancelFootSteps();
        return;
    }

    const FVector Right = GetSlopeRight(FVector::ZeroVector);
    const FVector LeftPosition = Mesh->GetCenterOfMass(Movement.LeftFoot.Bone);
    const FVector RightPosition = Mesh->GetCenterOfMass(Movement.RightFoot.Bone);
    float Separation = FVector::DotProduct(RightPosition - LeftPosition, Right);
    // 摆动脚还没落下时不能把瞬间分开算作站稳
    if (Recovery.IsCorrectingFeet() && (Movement.LeftFoot.Alpha < 1.f || Movement.RightFoot.Alpha < 1.f))
        Separation = FMath::Min(Separation, 0.f);
    const bool bWasCorrecting = Recovery.IsCorrectingFeet();
    if (Recovery.ObserveFeet(Separation, DeltaTime, FootCrossDistance, FootCrossConfirmTime,
        StableMinimumFootSide, FootRecoveryStableTime, FootRecoveryMaxTime))
    {
        // 撤销旧落点，先释放双脚，再从实际位置重新规划补步
        CancelFootSteps();
        Movement.bStandAnchored = false;
    }
    else if (bWasCorrecting && !Recovery.IsCorrectingFeet())
    {
        // 正常结束和超时退出都清掉未完成的补步目标
        CancelFootSteps();
    }
}

// 左右脚的迈步安排

// 先推进已经抬起的脚，再检查恢复与站姿，最后选择下一只迈步脚
// BeginStep 记录起点，UpdateFootTarget 推进目标，物理控制器带动实际脚部
void UDeliveryActiveRagdollComponent::UpdateFeet(float DeltaTime, const FVector& Wish)
{
	// 调用者已经排除了跳跃和晕倒；这里只在角色还能站立时把两只脚维持在各自一侧
	if (!Recovery.ShouldPauseWalking() && GetUprightDot() >= MinimumStepUprightDot)
		KeepFeetOnOwnSide();
	// 先完成已有步伐，避免每帧重新选择起点
	UpdateFootTarget(Movement.LeftFoot, DeltaTime);
	UpdateFootTarget(Movement.RightFoot, DeltaTime);
	ApplySlopeSupportTraction(DeltaTime);
	// 迈步超时后保留一段释放时间，让脚部恢复自由运动
	// 若立即重新选择同一只脚，会在本帧重新启用无法到达的落点
	// 这样物理模拟就没有机会真正释放脚部控制
	if (Recovery.IsReleasingFeet())
		return;

	const bool bMoving = !Wish.IsNearlyZero();
	// 记住“刚从移动变成停下”：普通行走停住后，如果脚离稳定位置太远，要补一步
	if (Movement.bWasMoving && !bMoving)
	{
		Movement.bPendingStopRecovery = true;
	}
	else if (bMoving)
	{
		Movement.bPendingStopRecovery = false;
	}
	Movement.bWasMoving = bMoving;

	if (Movement.StartupPlantRemaining > 0.0f)
	{
		Movement.StartupPlantRemaining = FMath::Max(0.0f, Movement.StartupPlantRemaining - DeltaTime);
		if (Movement.StartupPlantRemaining <= 0.0f)
		{
			PhysicsControl->SetControlEnabled(Movement.LeftFoot.Control, false, true, false);
			PhysicsControl->SetControlEnabled(Movement.RightFoot.Control, false, true, false);
		}
		return;
	}

	// 身体过度倾斜或仍有脚在摆动时，不开始下一步
	if (GetUprightDot() < MinimumStepUprightDot || Movement.LeftFoot.Alpha < 1.0f || Movement.RightFoot.Alpha < 1.0f)
	{
		return;
	}

	// 哪只脚在后面、有没有交叉到身体另一侧，看现在的身体姿态；落点本身从 PlannedPelvisTarget 推
	const FVector Forward = GetSlopeForward(Wish);
	const FVector Right = GetSlopeRight(Wish);
	const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
	const float LeftSide = FVector::DotProduct(Mesh->GetCenterOfMass(Movement.LeftFoot.Bone) - Hips, Right);
	const float RightSide = FVector::DotProduct(Mesh->GetCenterOfMass(Movement.RightFoot.Bone) - Hips, Right);
	// 正常迈步前先修复过窄的站姿，松开 WASD 后也要检查
	// 不能等到脚完全跨过身体中线才处理，那时膝盖可能已经相互缠住
	const float RecoverySide = FMath::Max(2.0f, StableMinimumFootSide * 0.5f);
	const float LeftClearance = LeftSide * Movement.LeftFoot.SideSign;
	const float RightClearance = RightSide * Movement.RightFoot.SideSign;
	if (FMath::Min(LeftClearance, RightClearance) < RecoverySide)
	{
		const bool bLeft = LeftClearance < RightClearance;
		if (BeginStep(bLeft ? Movement.LeftFoot : Movement.RightFoot, Wish))
			Movement.bStepLeftNext = !bLeft;
		return;
	}

	if (Recovery.IsCorrectingFeet())
		return;

	if (Wish.IsNearlyZero())
	{
		if (!Movement.bPendingStopRecovery)
		{
			return;
		}

		const FVector LeftStand = Movement.PlannedPelvisTarget + Right * (Movement.LeftFoot.SideSign * StableComedyStance);
		const FVector RightStand = Movement.PlannedPelvisTarget + Right * (Movement.RightFoot.SideSign * StableComedyStance);
		const FVector LeftDelta = FVector::VectorPlaneProject(
		    Mesh->GetCenterOfMass(Movement.LeftFoot.Bone) - LeftStand, Movement.CurrentGroundNormal);
		const FVector RightDelta = FVector::VectorPlaneProject(
		    Mesh->GetCenterOfMass(Movement.RightFoot.Bone) - RightStand, Movement.CurrentGroundNormal);
		const float LeftError = LeftDelta.Size();
		const float RightError = RightDelta.Size();

		// 刚松开移动时最多收一次脚，把脚收到髋目标两侧的站宽上，免得站着不停倒脚
		if (FMath::Max(LeftError, RightError) > StopRecoveryDistance)
		{
			const bool bRecoverLeft = LeftError > RightError;
			if (BeginStep(bRecoverLeft ? Movement.LeftFoot : Movement.RightFoot, Wish))
			{
				Movement.bStepLeftNext = !bRecoverLeft;
				Movement.bPendingStopRecovery = false;
			}
			// 坡沿落点暂时探不到地面时保留待补步状态，下帧重试
			// 旧逻辑不论成功与否都清标记，一次失败后就再也不补脚
			return;
		}
		Movement.bPendingStopRecovery = false;
		return;
	}

	// 优先迈落在后面的脚，两脚前后接近时按左右顺序轮换
	const float LeftForward = FVector::DotProduct(Mesh->GetCenterOfMass(Movement.LeftFoot.Bone) - Hips, Forward);
	const float RightForward = FVector::DotProduct(Mesh->GetCenterOfMass(Movement.RightFoot.Bone) - Hips, Forward);
	const bool bStepLeft = FMath::Abs(LeftForward - RightForward) > MovingCrossingRecoveryMargin
	                           ? LeftForward < RightForward
	                           : Movement.bStepLeftNext;
	const bool bStarted = BeginStep(bStepLeft ? Movement.LeftFoot : Movement.RightFoot, Wish);
	if (bStarted)
	{
		Movement.bStepLeftNext = !bStepLeft;
	}
}

// 迈步起点与落点

// 开始一步：记录本次摆动的起点与目标；后续帧沿这一段推进，避免每帧重新开始迈步
bool UDeliveryActiveRagdollComponent::BeginStep(FDeliveryRagdollFoot& Foot, const FVector& Wish)
{
	if (!PlanFootLanding(Foot, Wish))
	{
		return false;
	}

	// 记下抬脚时的位置，打开这只脚的位置电机；落点在摆动过程中还会按本帧髋目标重算
	Foot.Start = Mesh->GetCenterOfMass(Foot.Bone);
	Foot.Alpha = 0.0f;
	Foot.Elapsed = 0.0f;
	// 关闭期间控制器仍保存上一次迈步的世界目标；被撞走后直接启用，
	// 会先拉回旧脚位；下一帧又把新旧目标差 / DeltaTime 当作巨大速度
	// 在同一次调用中播种当前脚位、清零目标速度并启用，不能等下一帧
	PhysicsControl->SetControlTargetPositionAndOrientation(
	    Foot.Control, Foot.Start, Foot.TargetRotation.Rotator(), 0.0f, true, true, true, false);
	return true;
}

// 更新单脚迈步进度，计算位置和旋转目标
void UDeliveryActiveRagdollComponent::UpdateFootTarget(FDeliveryRagdollFoot& Foot, float DeltaTime)
{
	if (Foot.Alpha >= 1.0f)
	{
		return;
	}

	Foot.Elapsed += DeltaTime;
	Foot.Alpha = FMath::Min(1.0f, Foot.Elapsed / FMath::Max(ControlledStrideDuration, 0.05f));
	// 前 85% 可以随着身体移动重算落点；最后一段固定落点，给刚体真正到位的时间
	// 如果一直追着移动中的下坡目标、到时又立刻关电机，脚容易停在半空
	if (Foot.Alpha < 0.85f)
	{
		PlanFootLanding(Foot, Movement.WishOnSlope.IsNearlyZero() ? GetWishDir() : Movement.WishOnSlope);
	}

	// 落点已经在上面重算过；这条曲线只描述这一脚怎么从现在的位置走到落点：两端慢起慢停，中间沿法线抬起
	float SmoothAlpha = 0.0f;
	FVector Position = DeliveryFootPlacement::SwingPosition(Foot.Start, Foot.Target, Movement.CurrentGroundNormal,
	                                                        Foot.Alpha, ControlledStepHeight, SmoothAlpha);
	// 最后 15% 已固定落点，不能再用移动中的髋部把落脚目标推走
	if (Foot.Alpha < 0.85f)
	{
		Position = DeliveryFootPlacement::KeepOnSide(Position, Movement.PlannedPelvisTarget,
		                                             GetSlopeRight(FVector::ZeroVector), Foot.SideSign,
		                                             StableMinimumFootSide * SmoothAlpha);
	}

	PhysicsControl->SetControlTargetPositionAndOrientation(
	    Foot.Control, Position, Foot.TargetRotation.Rotator(), DeltaTime, true, true, true, false);

	if (Foot.Alpha >= 1.0f)
	{
		const float Gap = FVector::Dist(Mesh->GetCenterOfMass(Foot.Bone), Position);
		if (Gap > 18.0f && Foot.Elapsed < ControlledStrideDuration + 0.18f)
		{
			Foot.Alpha = 0.999f;
			return;
		}
		// 只额外等待有限时间：让物理脚有机会落到目标，又不让被障碍挡住的腿永远锁住
		// 关闭位置电机后，下方 KeepFeetOnOwnSide 仍会保护支撑脚不跨过中线
		PhysicsControl->SetControlEnabled(Foot.Control, false, true, false);
		if (Gap > 18.0f)
		{
			CancelFootSteps();
			Recovery.ReportIssue(EDeliveryRecoveryIssue::FootBlocked);
			Movement.bPendingStopRecovery = true;
		}

		// 到位或等待超时后结束这一步并发出脚步反馈
		// 这里包含超时退出，不表示已经确认脚掌接触地面
		const bool bLeftFoot = (&Foot == &Movement.LeftFoot);
		const FVector Landed = Mesh->GetCenterOfMass(Foot.Bone);
		OnFootPlanted.Broadcast(bLeftFoot, Landed);
		PlayFootstep(bLeftFoot, Landed, FVector::Dist2D(Foot.Start, Landed));
	}
}

// 落点规划与坡面方向

// 根据移动方向和地面情况选择下一次落脚位置
bool UDeliveryActiveRagdollComponent::PlanFootLanding(FDeliveryRagdollFoot& Foot, const FVector& Wish)
{
	// 在切平面上从髋目标推出落点，再沿法线投到坡面上，并用前进方向和法线摆正脚底板
	const FVector Forward = GetSlopeForward(Wish);
	const FVector Right = GetSlopeRight(Wish);
	if (Forward.IsNearlyZero() || Right.IsNearlyZero())
	{
		return false;
	}

	const float ForwardDistance = ControlledStrideLength * FMath::Clamp(Wish.Size(), 0.0f, 1.0f);
	// 已经沿前进方向运动的髋需要让脚稍微“走在前面”，否则脚落地时又落到身体后方
	const float ForwardSpeed = FVector::DotProduct(
	    FVector::VectorPlaneProject(Mesh->GetPhysicsLinearVelocity(Bones.Hips), Movement.CurrentGroundNormal),
	    Forward);
	const float VelocityLead = Wish.IsNearlyZero()
	                               ? 0.0f
	                               : FMath::Clamp(ForwardSpeed * ControlledStrideDuration * 0.35f, 0.0f,
	                                              ControlledStrideLength * 0.5f);
	const FVector Hips = Mesh->GetCenterOfMass(Bones.Hips);
	// 先修正到本侧再问地面高度，避免在另一侧地面采到错误的落点
	const FVector DestinationOnPlane = DeliveryFootPlacement::LandingOnPlane(
	    Movement.PlannedPelvisTarget, Forward, Right, Hips, Foot.SideSign, ForwardDistance, VelocityLead,
	    StableComedyStance, StableMinimumFootSide);

	FGroundHit Ground;
	if (!TraceGround(DestinationOnPlane, Movement.CurrentGroundNormal, Ground))
	{
		// 落点沿法线投不到地面，这一步取消，免得把脚目标留在半空
		return false;
	}

	Foot.Target = Ground.Point + Ground.Normal * Foot.GroundOffset;
	Foot.SideAxis = Right;
	Foot.TargetRotation = MakeSlopeAlignedFootRotation(Foot, Forward);
	return true;
}

// 取得沿当前地面的前进方向
FVector UDeliveryActiveRagdollComponent::GetSlopeForward(const FVector& Wish) const
{
	// 有移动输入时沿输入方向；原地则沿当前面向；所有方向都投影到坡面，
	// 避免“向前一步”变成水平穿入坡面或竖直抬脚
	FVector WorldForward = Wish.IsNearlyZero() ? FRotator(0.0f, CurrentFacingYaw, 0.0f).Vector() : Wish;
	FVector Forward = FVector::VectorPlaneProject(WorldForward, Movement.CurrentGroundNormal).GetSafeNormal();
	if (Forward.IsNearlyZero())
	{
		Forward = FVector::VectorPlaneProject(Movement.LastWishDirection, Movement.CurrentGroundNormal).GetSafeNormal();
	}
	if (Forward.IsNearlyZero() && GetOwner())
	{
		Forward = FVector::VectorPlaneProject(GetOwner()->GetActorForwardVector(), Movement.CurrentGroundNormal)
		              .GetSafeNormal();
	}
	return Forward;
}

// 取得沿当前地面的横向方向
FVector UDeliveryActiveRagdollComponent::GetSlopeRight(const FVector& /* 未使用 */) const
{
	// 左右以身体平滑后的朝向为准，不跟随瞬间改变的移动输入
	// 比如玩家突然反向按键时，髋还没转过来，不能立刻把左右脚的归属对调
	// 左右脚方向按髋部的实际朝向计算，不能使用电机要求的朝向
	// 急转身时目标朝向可能已经转过去，身体却尚未跟上
	// 使用目标朝向会交换左右侧，导致支撑腿穿向另一条腿
	if (const FBodyInstance* Pelvis = Mesh->GetBodyInstance(Bones.Hips))
	{
		const FVector Side = DeliveryFootPlacement::PhysicalSideAxis(
		    Movement.CurrentGroundNormal, Pelvis->GetUnrealWorldTransform().GetRotation(), ReferencePelvisRotation,
		    ReferenceFacingYaw);
		if (!Side.IsNearlyZero())
			return Side;
	}
	return DeliveryFootPlacement::SideAxis(Movement.CurrentGroundNormal, CurrentFacingYaw);
}

// 防止双脚交叉

// 限制左右脚越过身体中线，减少双脚交叉
void UDeliveryActiveRagdollComponent::KeepFeetOnOwnSide()
{
	// 这里修正的是脚的真实物理刚体，不只是迈步目标；支撑脚没有位置电机，
	// 在下坡滑动时也可能跨过身体中线，所以需要一个有限的向外分离力
	const FVector Right = GetSlopeRight(FVector::ZeroVector);
	const FVector Hips = Mesh->GetCenterOfMass(Bones.Hips);
	const FVector HipVelocity = Mesh->GetPhysicsLinearVelocity(Bones.Hips);
	for (FDeliveryRagdollFoot* Foot : {&Movement.LeftFoot, &Movement.RightFoot})
	{
		const FVector Outward = Right * Foot->SideSign;
		const float Side = FVector::DotProduct(Mesh->GetCenterOfMass(Foot->Bone) - Hips, Outward);
		if (Side >= StableMinimumFootSide)
			continue;
		const float OutwardSpeed =
		    FVector::DotProduct(Mesh->GetPhysicsLinearVelocity(Foot->Bone) - HipVelocity, Outward);
		const float Acceleration =
		    DeliveryFootPlacement::SeparationAcceleration(Side, OutwardSpeed, StableMinimumFootSide);
		Mesh->AddForce(Outward * Acceleration, Foot->Bone, true);
	}
}

// 坡面支撑与防滑

// 在支撑脚上施加坡面辅助力，减少静止下滑
void UDeliveryActiveRagdollComponent::ApplySlopeSupportTraction(float DeltaTime)
{
	if (GetUprightDot() < 0.65f || Recovery.IsReleasingFeet() || SlopeSupportTraction <= 0.f ||
	    !GetWorld())
		return;
	for (const FDeliveryRagdollFoot* Foot : {&Movement.LeftFoot, &Movement.RightFoot})
	{
		// 摆动、脚步释放恢复和空中都不能刹脚；否则又会把髋与腿互相拉扯
		if (Foot->Alpha < 1.f || PhysicsControl->GetControlEnabled(Foot->Control))
			continue;
		const FVector Sole = Mesh->GetBoneLocation(Foot->SoleBone, EBoneSpaces::WorldSpace);
		FGroundHit Ground;
		if (!TraceGround(Sole, Movement.CurrentGroundNormal, Ground))
			continue;
		const float Height = FVector::DotProduct(Sole - Ground.Point, Ground.Normal);
		if (Height < -5.f || Height > 5.f || Ground.Normal.Z > 0.9962f ||
		    Ground.Normal.Z < FMath::Cos(FMath::DegreesToRadians(TumbleSlopeDegrees)))
			continue;
		const FVector Velocity = Mesh->GetPhysicsLinearVelocity(Foot->Bone) - Ground.SurfaceVelocity;
		if (FMath::Abs(FVector::DotProduct(Velocity, Ground.Normal)) > 50.f)
			continue;
		Mesh->AddForce(DeliveryFootPlacement::SupportTraction(
		                   Velocity, Ground.Normal, GetWorld()->GetGravityZ(), DeltaTime,
		                   SlopeSupportTraction, SlopeSupportMaxAcceleration),
		               Foot->Bone, true);
	}
}

// 根据地面法线调整脚掌方向
FQuat UDeliveryActiveRagdollComponent::MakeSlopeAlignedFootRotation(const FDeliveryRagdollFoot& Foot,
                                                                    const FVector& SlopeForward) const
{
	// 先用坡面法线作“脚底朝上”方向，再把前进方向放进坡面；最后把
	// 启动时脚骨骼与参考坐标系的偏差补回来，避免直接套坡面旋转后脚板歪斜
	FVector AxisZ = Movement.CurrentGroundNormal.GetSafeNormal();
	FVector AxisX = FVector::VectorPlaneProject(SlopeForward, AxisZ).GetSafeNormal();
	if (AxisX.IsNearlyZero())
	{
		AxisX = FVector::VectorPlaneProject(FVector::ForwardVector, AxisZ).GetSafeNormal();
	}
	if (AxisZ.IsNearlyZero() || AxisX.IsNearlyZero())
	{
		return Foot.ReferenceRotation;
	}

	const FQuat SlopeBasis(FRotationMatrix::MakeFromZX(AxisZ, AxisX));
	const FVector ReferenceForward = FRotator(0.0f, ReferenceFacingYaw, 0.0f).Vector();
	const FQuat ReferenceBasis(FRotationMatrix::MakeFromZX(FVector::UpVector, ReferenceForward));
	return SlopeBasis * ReferenceBasis.Inverse() * Foot.ReferenceRotation;
}

// 落脚声音

// 根据落脚状态和速度播放脚步音效
void UDeliveryActiveRagdollComponent::PlayFootstep(bool bLeft, const FVector& Location, float Stride)
{
	// 碎步（平衡恢复那种）不出声，见 FootstepMinStride 的注释
	if (FootstepSounds.IsEmpty() || Stride < FootstepMinStride)
	{
		return;
	}

	// 随机挑一个，但避开上一次刚放过的那个；纯随机时 8 个变体里
	// 连续撞上同一个的概率是 1/8，走十来步就会出现一次"连着两下一样"，
	// 而那正是"音效很假"最容易被察觉的瞬间
	int32 Index = FMath::RandHelper(FootstepSounds.Num());
	if (FootstepSounds.Num() > 1 && Index == LastFootstepIndex)
	{
		Index = (Index + 1) % FootstepSounds.Num();
	}
	LastFootstepIndex = Index;

	USoundBase* Sound = FootstepSounds[Index];
	if (!Sound)
	{
		return;
	}

	// 大步重、小步轻；下限给 0.35 而不是 0：刚过门槛的那一脚也该听得见，
	// 否则门槛附近会出现"有时响有时不响"，听着像漏音
	const float Loudness = FMath::GetMappedRangeValueClamped(
	    FVector2D(FootstepMinStride, FMath::Max(FootstepFullStride, FootstepMinStride + 1.0f)),
	    FVector2D(0.35f, 1.0f), Stride);

	// 空间音，不是 PlaySound2D； 取件/交付那两个提示音是"给你本人的反馈"所以用 2D
	// 脚步是世界里真实发生的事，别人的脚步要能从方位上听出来
	UGameplayStatics::PlaySoundAtLocation(this, Sound, Location, FootstepVolume * Loudness,
	                                      bLeft ? FootstepLeftPitch : FootstepRightPitch);
}
