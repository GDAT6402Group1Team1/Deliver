// 脚步：从髋部目标推算两脚落点，沿坡面探测真实地面，再把摆动目标交给脚电机。
// 脚的实际位置由物理模拟给出，因此落点与刚体之间允许短暂误差。
#include "DeliveryActiveRagdollComponent.h"
#include "DeliveryFootPlacement.h"
#include "Components/SkeletalMeshComponent.h"
#include "Math/RotationMatrix.h"
#include "PhysicsControlComponent.h"

void UDeliveryActiveRagdollComponent::UpdateFeet(float DeltaTime, const FVector& Wish)
{
	// 调用者已经排除了跳跃和晕倒；这里只在角色还能站立时把两只脚维持在各自一侧。
	if (GetUprightDot() >= MinimumStepUprightDot) KeepFeetOnOwnSide();
	UpdateFootTarget(LeftFoot, DeltaTime);
	UpdateFootTarget(RightFoot, DeltaTime);

	const bool bMoving = !Wish.IsNearlyZero();
	// 记住“刚从移动变成停下”：普通行走停住后，如果脚离稳定位置太远，要补一步。
	if (bWasMoving && !bMoving)
	{
		bPendingStopRecovery = true;
	}
	else if (bMoving)
	{
		bPendingStopRecovery = false;
	}
	bWasMoving = bMoving;

	if (StartupPlantRemaining > 0.0f)
	{
		StartupPlantRemaining = FMath::Max(0.0f, StartupPlantRemaining - DeltaTime);
		if (StartupPlantRemaining <= 0.0f)
		{
			PhysicsControl->SetControlEnabled(LeftFoot.Control, false, true, false);
			PhysicsControl->SetControlEnabled(RightFoot.Control, false, true, false);
		}
		return;
	}

	if (GetUprightDot() < MinimumStepUprightDot
		|| LeftFoot.Alpha < 1.0f || RightFoot.Alpha < 1.0f)
	{
		return;
	}

	// 哪只脚在后面、有没有交叉到身体另一侧，看现在的身体姿态。落点本身从 PlannedPelvisTarget 推。
	const FVector Forward = GetSlopeForward(Wish);
	const FVector Right = GetSlopeRight(Wish);
	const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
	const float LeftSide = FVector::DotProduct(Mesh->GetCenterOfMass(LeftFoot.Bone) - Hips, Right);
	const float RightSide = FVector::DotProduct(Mesh->GetCenterOfMass(RightFoot.Bone) - Hips, Right);
	// 正常迈步前先修复过窄的站姿，松开 WASD 后也要检查。
	// 不能等到脚完全跨过身体中线才处理，那时膝盖可能已经相互缠住。
	const float RecoverySide = FMath::Max(2.0f, StableMinimumFootSide * 0.5f);
	const float LeftClearance = LeftSide * LeftFoot.SideSign;
	const float RightClearance = RightSide * RightFoot.SideSign;
	if (FMath::Min(LeftClearance, RightClearance) < RecoverySide)
	{
		const bool bLeft = LeftClearance < RightClearance;
		if (BeginStep(bLeft ? LeftFoot : RightFoot, Wish)) bStepLeftNext = !bLeft;
		return;
	}

	if (Wish.IsNearlyZero())
	{
		if (!bPendingStopRecovery)
		{
			return;
		}

		const FVector LeftStand = PlannedPelvisTarget + Right * (LeftFoot.SideSign * StableComedyStance);
		const FVector RightStand = PlannedPelvisTarget + Right * (RightFoot.SideSign * StableComedyStance);
		const FVector LeftDelta = FVector::VectorPlaneProject(
			Mesh->GetCenterOfMass(LeftFoot.Bone) - LeftStand, CurrentGroundNormal);
		const FVector RightDelta = FVector::VectorPlaneProject(
			Mesh->GetCenterOfMass(RightFoot.Bone) - RightStand, CurrentGroundNormal);
		const float LeftError = LeftDelta.Size();
		const float RightError = RightDelta.Size();

		// 刚松开移动时最多收一次脚，把脚收到髋目标两侧的站宽上，免得站着不停倒脚。
		if (FMath::Max(LeftError, RightError) > StopRecoveryDistance)
		{
			const bool bRecoverLeft = LeftError > RightError;
			if (BeginStep(bRecoverLeft ? LeftFoot : RightFoot, Wish))
			{
				bStepLeftNext = !bRecoverLeft;
				bPendingStopRecovery = false;
			}
			// 坡沿落点暂时探不到地面时保留待补步状态，下帧重试。
			// 旧逻辑不论成功与否都清标记，一次失败后就再也不补脚。
			return;
		}
		bPendingStopRecovery = false;
		return;
	}

	const float LeftForward = FVector::DotProduct(
		Mesh->GetCenterOfMass(LeftFoot.Bone) - Hips, Forward);
	const float RightForward = FVector::DotProduct(
		Mesh->GetCenterOfMass(RightFoot.Bone) - Hips, Forward);
	const bool bStepLeft = FMath::Abs(LeftForward - RightForward) > MovingCrossingRecoveryMargin
		? LeftForward < RightForward
		: bStepLeftNext;
	const bool bStarted = BeginStep(bStepLeft ? LeftFoot : RightFoot, Wish);
	if (bStarted)
	{
		bStepLeftNext = !bStepLeft;
	}
}

bool UDeliveryActiveRagdollComponent::BeginStep(FFoot& Foot, const FVector& Wish)
{
	if (!PlanFootLanding(Foot, Wish))
	{
		return false;
	}

	// 记下抬脚时的位置，打开这只脚的位置电机。落点在摆动过程中还会按本帧髋目标重算。
	Foot.Start = Mesh->GetCenterOfMass(Foot.Bone);
	Foot.Alpha = 0.0f;
	Foot.Elapsed = 0.0f;
	// 关闭期间控制器仍保存上一次迈步的世界目标。被撞走后直接启用，
	// 会先拉回旧脚位；下一帧又把新旧目标差 / DeltaTime 当作巨大速度。
	// 在同一次调用中播种当前脚位、清零目标速度并启用，不能等下一帧。
	PhysicsControl->SetControlTargetPositionAndOrientation(
		Foot.Control, Foot.Start, Foot.TargetRotation.Rotator(),
		0.0f, true, true, true, false);
	return true;
}

void UDeliveryActiveRagdollComponent::UpdateFootTarget(FFoot& Foot, float DeltaTime)
{
	if (Foot.Alpha >= 1.0f)
	{
		return;
	}

	Foot.Elapsed += DeltaTime;
	Foot.Alpha = FMath::Min(1.0f, Foot.Elapsed / FMath::Max(ControlledStrideDuration, 0.05f));
	// 前 85% 可以随着身体移动重算落点；最后一段固定落点，给刚体真正到位的时间。
	// 如果一直追着移动中的下坡目标、到时又立刻关电机，脚容易停在半空。
	if (Foot.Alpha < 0.85f)
	{
		PlanFootLanding(Foot, WishOnSlope.IsNearlyZero() ? GetWishDir() : WishOnSlope);
	}

	// 落点已经在上面重算过。这条曲线只描述这一脚怎么从现在的位置走到落点：两端慢起慢停，中间沿法线抬起。
	float SmoothAlpha = 0.0f;
	FVector Position = DeliveryFootPlacement::SwingPosition(Foot.Start, Foot.Target,
		CurrentGroundNormal, Foot.Alpha, ControlledStepHeight, SmoothAlpha);
	Position = DeliveryFootPlacement::KeepOnSide(Position, PlannedPelvisTarget,
		Foot.SideAxis, Foot.SideSign, StableMinimumFootSide * SmoothAlpha);

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
		// 只额外等待有限时间：让物理脚有机会落到目标，又不让被障碍挡住的腿永远锁住。
		// 关闭位置电机后，下方 KeepFeetOnOwnSide 仍会保护支撑脚不跨过中线。
		PhysicsControl->SetControlEnabled(Foot.Control, false, true, false);
	}
}

bool UDeliveryActiveRagdollComponent::PlanFootLanding(FFoot& Foot, const FVector& Wish)
{
	// 在切平面上从髋目标推出落点，再沿法线投到坡面上，并用前进方向和法线摆正脚底板。
	const FVector Forward = GetSlopeForward(Wish);
	const FVector Right = GetSlopeRight(Wish);
	if (Forward.IsNearlyZero() || Right.IsNearlyZero())
	{
		return false;
	}

	const float ForwardDistance = ControlledStrideLength * FMath::Clamp(Wish.Size(), 0.0f, 1.0f);
	// 已经沿前进方向运动的髋需要让脚稍微“走在前面”，否则脚落地时又落到身体后方。
	const float ForwardSpeed = FVector::DotProduct(
		FVector::VectorPlaneProject(Mesh->GetPhysicsLinearVelocity(Bones.Hips), CurrentGroundNormal),
		Forward);
	const float VelocityLead = Wish.IsNearlyZero()
		? 0.0f
		: FMath::Clamp(
			ForwardSpeed * ControlledStrideDuration * 0.35f,
			0.0f, ControlledStrideLength * 0.5f);
	const FVector Hips = Mesh->GetCenterOfMass(Bones.Hips);
	// 先修正到本侧再问地面高度，避免在另一侧地面采到错误的落点。
	const FVector DestinationOnPlane = DeliveryFootPlacement::LandingOnPlane(PlannedPelvisTarget,
		Forward, Right, Hips, Foot.SideSign, ForwardDistance, VelocityLead,
		StableComedyStance, StableMinimumFootSide);

	FGroundHit Ground;
	if (!TraceGround(DestinationOnPlane, CurrentGroundNormal, Ground))
	{
		// 落点沿法线投不到地面，这一步取消，免得把脚目标留在半空。
		return false;
	}

	Foot.Target = Ground.Point + Ground.Normal * Foot.GroundOffset;
	Foot.SideAxis = Right;
	Foot.TargetRotation = MakeSlopeAlignedFootRotation(Foot, Forward);
	return true;
}

FVector UDeliveryActiveRagdollComponent::GetSlopeForward(const FVector& Wish) const
{
	// 有移动输入时沿输入方向；原地则沿当前面向。所有方向都投影到坡面，
	// 避免“向前一步”变成水平穿入坡面或竖直抬脚。
	FVector WorldForward = Wish.IsNearlyZero()
		? FRotator(0.0f, CurrentFacingYaw, 0.0f).Vector()
		: Wish;
	FVector Forward = FVector::VectorPlaneProject(WorldForward, CurrentGroundNormal).GetSafeNormal();
	if (Forward.IsNearlyZero())
	{
		Forward = FVector::VectorPlaneProject(LastWishDirection, CurrentGroundNormal).GetSafeNormal();
	}
	if (Forward.IsNearlyZero() && GetOwner())
	{
		Forward = FVector::VectorPlaneProject(GetOwner()->GetActorForwardVector(), CurrentGroundNormal).GetSafeNormal();
	}
	return Forward;
}

FVector UDeliveryActiveRagdollComponent::GetSlopeRight(const FVector& /*Wish*/) const
{
	// 左右以身体平滑后的朝向为准，不跟随瞬间改变的移动输入。
	// 比如玩家突然反向按键时，髋还没转过来，不能立刻把左右脚的归属对调。
	return DeliveryFootPlacement::SideAxis(CurrentGroundNormal, CurrentFacingYaw);
}

void UDeliveryActiveRagdollComponent::KeepFeetOnOwnSide()
{
	// 这里修正的是脚的真实物理刚体，不只是迈步目标。支撑脚没有位置电机，
	// 在下坡滑动时也可能跨过身体中线，所以需要一个有限的向外分离力。
	const FVector Right = FRotationMatrix(FRotator(0.0f, CurrentFacingYaw, 0.0f)).GetUnitAxis(EAxis::Y);
	const FVector Hips = Mesh->GetCenterOfMass(Bones.Hips);
	const FVector HipVelocity = Mesh->GetPhysicsLinearVelocity(Bones.Hips);
	for (FFoot* Foot : { &LeftFoot, &RightFoot })
	{
		const FVector Outward = Right * Foot->SideSign;
		const float Side = FVector::DotProduct(Mesh->GetCenterOfMass(Foot->Bone) - Hips, Outward);
		if (Side >= StableMinimumFootSide) continue;
		const float OutwardSpeed = FVector::DotProduct(Mesh->GetPhysicsLinearVelocity(Foot->Bone) - HipVelocity, Outward);
		const float Acceleration = DeliveryFootPlacement::SeparationAcceleration(Side, OutwardSpeed, StableMinimumFootSide);
		Mesh->AddForce(Outward * Acceleration, Foot->Bone, true);
	}
}

FQuat UDeliveryActiveRagdollComponent::MakeSlopeAlignedFootRotation(
	const FFoot& Foot, const FVector& SlopeForward) const
{
	// 先用坡面法线作“脚底朝上”方向，再把前进方向放进坡面；最后把
	// 启动时脚骨骼与参考坐标系的偏差补回来，避免直接套坡面旋转后脚板歪斜。
	FVector AxisZ = CurrentGroundNormal.GetSafeNormal();
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
