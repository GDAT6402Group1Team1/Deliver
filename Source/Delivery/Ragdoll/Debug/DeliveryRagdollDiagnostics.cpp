// 只读诊断，不参与身体控制与网络决策
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"
#include "PhysicsControlComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "Math/RotationMatrix.h"
#include "Delivery.h"
#include "Animation/AnimSequence.h"
#include "HAL/IConsoleManager.h"
#include "UObject/ConstructorHelpers.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryHandheldItem.h"

#if !UE_BUILD_SHIPPING
static TAutoConsoleVariable<int32> CVarDeliveryPhysicsTrace(
    TEXT("Delivery.Physics.Trace"), 0, TEXT("Enable detailed ragdoll diagnostics"));
#endif

#if !UE_BUILD_SHIPPING
// 输出起身和身体控制的诊断数据
void UDeliveryActiveRagdollComponent::TraceRecoveryPhysics(bool bAfterPhysics, float DeltaTime)
{
	// 需要排查时用 Delivery.Physics.Trace 1 开启，避免多人运行持续输出大量日志
	if (CVarDeliveryPhysicsTrace.GetValueOnGameThread() == 0)
		return;
	if (!GetOwner() || !Mesh || !PhysicsControl || !GetWorld())
		return;
	if (!GetOwner()->HasAuthority() && GetOwner()->GetLocalRole() != ROLE_AutonomousProxy)
		return;
	const FBodyInstance* Body = Mesh->GetBodyInstance(Bones.Hips);
	if (!Body)
		return;
	const float Now = GetWorld()->GetTimeSeconds();
	const int32 Mode = static_cast<int32>(ReplicatedControlMode);
	// 髋部站直后继续采样，确认肩部、肘部和手腕也恢复到目标姿势
	
	if (bAfterPhysics && Now >= DiagnosticUpperBodyNextLog)
	{
		DiagnosticUpperBodyNextLog = Now + (Punch.IsActive() || IsPunchRecovering() ? 0.2f : 0.5f);
		UE_LOG(
		    LogDelivery, Log,
		    TEXT(
		        "PunchDiag actor=%s world=%s time=%.3f role=%d dt=%.4f mode=%d active=%d released=%d hand=%.0f aim=%s facing=%.1f twist=%.1f upright=%.3f planted=%d interrupted=%d axe=%d mesh=%s physics=%s"),
		    *GetNameSafe(GetOwner()), *GetNameSafe(GetWorld()), Now, int32(GetOwner()->GetLocalRole()),
		    DeltaTime, Mode, Punch.IsActive(), Punch.IsReleased(), Punch.GetHandSide(),
		    *Punch.GetDirection().ToCompactString(), CurrentFacingYaw, PunchTwist, GetUprightDot(),
		    bPunchFeetPlanted, bPunchSupportInterrupted, SwingAxe.IsValid(),
		    *GetNameSafe(Mesh->GetSkeletalMeshAsset()), *GetNameSafe(Mesh->GetPhysicsAsset()));
		// 撞击恢复诊断需要覆盖整条关节链，包括身体松软期间
		// 读取实际运行的约束，资源模板不能反映创建后的最终配置
		if (const UPhysicsAsset* Asset = Mesh->GetPhysicsAsset())
		{
			for (const UPhysicsConstraintTemplate* Template : Asset->ConstraintSetup)
			{
				if (!Template)
					continue;
				const FConstraintInstance* Joint =
				    Mesh->FindConstraintInstance(Template->DefaultInstance.JointName);
				if (!Joint)
					continue;
				const FBodyInstance* Child = Mesh->GetBodyInstance(Joint->ConstraintBone1);
				const FBodyInstance* Parent = Mesh->GetBodyInstance(Joint->ConstraintBone2);
				if (!Child || !Parent)
					continue;
				// 把关节两端都换到世界坐标，才能比较是否被拉开或扭转
				const FTransform ChildFrame =
				    Joint->GetRefFrame(EConstraintFrame::Frame1) * Child->GetUnrealWorldTransform();
				const FTransform ParentFrame =
				    Joint->GetRefFrame(EConstraintFrame::Frame2) * Parent->GetUnrealWorldTransform();
				UE_LOG(
				    LogDelivery, Log,
				    TEXT(
				        "CrashDiagJoint actor=%s time=%.3f role=%d mode=%d limp=%d rise=%.2f child=%s parent=%s frameGapCm=%.2f frameAngleDeg=%.1f anglesRad=%.3f,%.3f,%.3f limitsDeg=%.1f,%.1f,%.1f motions=%d,%d,%d mass=%.4f,%.4f collisionDisabled=%d angularV=%s"),
				    *GetNameSafe(GetOwner()), Now, int32(GetOwner()->GetLocalRole()), Mode, bIsLimp,
				    Recovery.GetRiseAlpha(), *Joint->ConstraintBone1.ToString(),
				    *Joint->ConstraintBone2.ToString(),
				    FVector::Distance(ChildFrame.GetLocation(), ParentFrame.GetLocation()),
				    FMath::RadiansToDegrees(
				        ChildFrame.GetRotation().AngularDistance(ParentFrame.GetRotation())),
				    Joint->GetCurrentSwing1(), Joint->GetCurrentSwing2(), Joint->GetCurrentTwist(),
				    Joint->GetAngularSwing1Limit(), Joint->GetAngularSwing2Limit(),
				    Joint->GetAngularTwistLimit(), int32(Joint->GetAngularSwing1Motion()),
				    int32(Joint->GetAngularSwing2Motion()), int32(Joint->GetAngularTwistMotion()),
				    Child->GetBodyMass(), Parent->GetBodyMass(), Joint->IsCollisionDisabled(),
				    *Child->GetUnrealWorldAngularVelocityInRadians().ToCompactString());
			}
		}
		for (const FName Control : PhysicsControl->GetControlNamesInSet(TEXT("All")))
		{
			FPhysicsControlData Data;
			FPhysicsControlMultiplier Multiplier;
			FPhysicsControlTarget Target;
			if (!PhysicsControl->GetControlData(Control, Data) ||
			    !PhysicsControl->GetControlMultiplier(Control, Multiplier) ||
			    !PhysicsControl->GetControlTarget(Control, Target))
				continue;
			UE_LOG(
			    LogDelivery, Log,
			    TEXT(
			        "CrashDiagDrive actor=%s time=%.3f role=%d control=%s enabled=%d useAnim=%d angularStrength=%.2f angularMultiplier=%.2f linearMultiplier=%s targetOffset=%s targetAngularV=%s"),
			    *GetNameSafe(GetOwner()), Now, int32(GetOwner()->GetLocalRole()), *Control.ToString(),
			    PhysicsControl->GetControlEnabled(Control), Data.bUseSkeletalAnimation, Data.AngularStrength,
			    Multiplier.AngularStrengthMultiplier, *Multiplier.LinearStrengthMultiplier.ToCompactString(),
			    *Target.TargetOrientation.ToCompactString(), *Target.TargetAngularVelocity.ToCompactString());
		}
		for (const FDeliveryBoxingPose::FArm& Arm : BoxingPose.Arms)
		{
			if (!Arm.bReady)
				continue;
			UE_LOG(LogDelivery, Log,
			       TEXT("PunchDiagArm actor=%s time=%.3f bone=%s windup=%.3f extension=%.3f aim=%s"),
			       *GetNameSafe(GetOwner()), Now, *Arm.Upper.ToString(), Arm.Windup, Arm.Extension,
			       *Arm.PunchForward.ToCompactString());
			const TPair<FName, FName> Links[] = {
			    {Arm.Upper, Arm.UpperControl}, {Arm.Lower, Arm.LowerControl}, {Arm.Hand, Arm.HandControl}};
			for (const TPair<FName, FName>& Link : Links)
			{
				const FBodyInstance* LinkBody = Mesh->GetBodyInstance(Link.Key);
				FPhysicsControlTarget Target;
				FPhysicsControlData Data;
				FPhysicsControlMultiplier Multiplier;
				if (!LinkBody || !PhysicsControl->GetControlTarget(Link.Value, Target) ||
				    !PhysicsControl->GetControlData(Link.Value, Data) ||
				    !PhysicsControl->GetControlMultiplier(Link.Value, Multiplier))
					continue;
				FQuat Actual = LinkBody->GetUnrealWorldTransform().GetRotation();
				const FName ParentBone = Link.Key == Arm.Lower  ? Arm.Upper
				                         : Link.Key == Arm.Hand ? Arm.Lower
				                                                : NAME_None;
				// 手和前臂的目标使用父骨骼坐标，实际旋转也要换到相同坐标后比较
				if (!ParentBone.IsNone())
				{
					if (const FBodyInstance* ParentBody = Mesh->GetBodyInstance(ParentBone))
					{
						Actual = ParentBody->GetUnrealWorldTransform().GetRotation().Inverse() * Actual;
					}
				}
				const float Error =
				    FMath::RadiansToDegrees(Actual.AngularDistance(Target.TargetOrientation.Quaternion()));
				// 保留正常阶段的采样，便于对比异常出现前后的变化
				UE_LOG(
				    LogDelivery, Log,
				    TEXT(
				        "UpperBodyTrace actor=%s world=%s time=%.3f mode=%d bone=%s enabled=%d errorDeg=%.1f strength=%.2f multiplier=%.2f actual=%s target=%s angularV=%s"),
				    *GetNameSafe(GetOwner()), *GetNameSafe(GetWorld()), Now, Mode, *Link.Key.ToString(),
				    PhysicsControl->GetControlEnabled(Link.Value), Error, Data.AngularStrength,
				    Multiplier.AngularStrengthMultiplier, *Actual.Rotator().ToCompactString(),
				    *Target.TargetOrientation.ToCompactString(),
				    *LinkBody->GetUnrealWorldAngularVelocityInRadians().ToCompactString());
				UE_LOG(
				    LogDelivery, Log,
				    TEXT(
				        "PunchDiagBody actor=%s time=%.3f bone=%s mass=%.4f sim=%d awake=%d parent=%s pos=%s targetAngularV=%s"),
				    *GetNameSafe(GetOwner()), Now, *Link.Key.ToString(), LinkBody->GetBodyMass(),
				    Mesh->IsSimulatingPhysics(Link.Key), LinkBody->IsInstanceAwake(), *ParentBone.ToString(),
				    *LinkBody->GetUnrealWorldTransform().GetLocation().ToCompactString(),
				    *Target.TargetAngularVelocity.ToCompactString());
				if (const FConstraintInstance* Joint = Mesh->FindConstraintInstance(Link.Key))
				{
					UE_LOG(
					    LogDelivery, Log,
					    TEXT(
					        "PunchDiagJoint actor=%s time=%.3f bone=%s child=%s parent=%s anglesRadians=%.3f,%.3f,%.3f limitsDegrees=%.1f,%.1f,%.1f motions=%d,%d,%d collisionDisabled=%d"),
					    *GetNameSafe(GetOwner()), Now, *Link.Key.ToString(),
					    *Joint->ConstraintBone1.ToString(), *Joint->ConstraintBone2.ToString(),
					    Joint->GetCurrentSwing1(), Joint->GetCurrentSwing2(), Joint->GetCurrentTwist(),
					    Joint->GetAngularSwing1Limit(), Joint->GetAngularSwing2Limit(),
					    Joint->GetAngularTwistLimit(), int32(Joint->GetAngularSwing1Motion()),
					    int32(Joint->GetAngularSwing2Motion()), int32(Joint->GetAngularTwistMotion()),
					    Joint->IsCollisionDisabled());
				}
			}
		}
		for (const FDeliveryRagdollFoot* Foot : {&Movement.LeftFoot, &Movement.RightFoot})
		{
			const FName Thigh = Foot == &Movement.LeftFoot ? Bones.LeftUpLeg : Bones.RightUpLeg;
			for (FName ChildName : {Thigh, Foot->Bone})
			{
				FName ParentName = Mesh->GetParentBone(ChildName);
				while (!ParentName.IsNone() && !Mesh->GetBodyInstance(ParentName))
					ParentName = Mesh->GetParentBone(ParentName);
				const FBodyInstance* Child = Mesh->GetBodyInstance(ChildName);
				const FBodyInstance* Parent = Mesh->GetBodyInstance(ParentName);
				const FName Control(*(ParentName.ToString() + TEXT("_") + ChildName.ToString()));
				FPhysicsControlTarget Target;
				if (!Child || !Parent || !PhysicsControl->GetControlTarget(Control, Target))
					continue;
				const FQuat Actual = Parent->GetUnrealWorldTransform().GetRotation().Inverse() *
				                     Child->GetUnrealWorldTransform().GetRotation();
				FPhysicsControlTarget FootTarget;
				PhysicsControl->GetControlTarget(Foot->Control, FootTarget);
				UE_LOG(
				    LogDelivery, Log,
				    TEXT(
				        "LegRecoveryDiag time=%.3f bone=%s targetErrorDeg=%.2f actual=%s target=%s footDrive=%d footGap=%.2f stepAlpha=%.3f stepElapsed=%.3f wish=%s hipSpeed=%.1f"),
				    Now, *ChildName.ToString(),
				    FMath::RadiansToDegrees(Actual.AngularDistance(Target.TargetOrientation.Quaternion())),
				    *Actual.Rotator().ToCompactString(), *Target.TargetOrientation.ToCompactString(),
				    PhysicsControl->GetControlEnabled(Foot->Control),
				    FVector::Dist(Mesh->GetCenterOfMass(Foot->Bone), FootTarget.TargetPosition), Foot->Alpha,
				    Foot->Elapsed, *GetWishDir().ToCompactString(),
				    Mesh->GetPhysicsLinearVelocity(Bones.Hips).Size());
			}
		}
		const FBodyInstance* Chest = Mesh->GetBodyInstance(Bones.Spine);
		FPhysicsControlTarget ChestTarget;
		if (Chest && PhysicsControl->GetControlTarget(ChestControl, ChestTarget))
		{
			const FQuat Relative = Body->GetUnrealWorldTransform().GetRotation().Inverse() *
			                       Chest->GetUnrealWorldTransform().GetRotation();
			const float Error =
			    FMath::RadiansToDegrees(Relative.AngularDistance(ChestTarget.TargetOrientation.Quaternion()));
			if (Error >= 30.0f)
			{
				UE_LOG(
				    LogDelivery, Log,
				    TEXT(
				        "UpperBodyTrace actor=%s world=%s time=%.3f mode=%d bone=%s chestToPelvisErrorDeg=%.1f enabled=%d"),
				    *GetNameSafe(GetOwner()), *GetNameSafe(GetWorld()), Now, Mode, *Bones.Spine.ToString(),
				    Error, PhysicsControl->GetControlEnabled(ChestControl));
			}
		}
	}
	if (!GetOwner()->HasAuthority())
		return;
	// 物理计算前保存位置和速度，计算后对比同一步的变化，定位起身跳动来源
	if (!bAfterPhysics)
	{
		if (Mode != DiagnosticRecoveryMode)
		{
			if (DiagnosticRecoveryMode != -1 || bIsLimp || Recovery.IsRising())
			{
				DiagnosticRecoveryUntil = Now + 8.0f;
				DiagnosticRecoveryNextLog = Now;
				UE_LOG(LogDelivery, Log,
				       TEXT("RecoveryTrace transition actor=%s world=%s time=%.3f mode=%d->%d"),
				       *GetNameSafe(GetOwner()), *GetNameSafe(GetWorld()), Now, DiagnosticRecoveryMode, Mode);
			}
			DiagnosticRecoveryMode = Mode;
		}
		bDiagnosticPrePhysicsValid = Now < DiagnosticRecoveryUntil;
		if (bDiagnosticPrePhysicsValid)
		{
			DiagnosticPrePhysicsPosition = Body->GetUnrealWorldTransform().GetLocation();
			DiagnosticPrePhysicsVelocity = Body->GetUnrealWorldVelocity();
		}
		return;
	}
	if (!bDiagnosticPrePhysicsValid)
		return;
	bDiagnosticPrePhysicsValid = false;
	const FVector Position = Body->GetUnrealWorldTransform().GetLocation();
	const float StepXY = FVector::Dist2D(Position, DiagnosticPrePhysicsPosition);
	if (Now < DiagnosticRecoveryNextLog && StepXY < 20.0f)
		return;
	DiagnosticRecoveryNextLog = Now + 0.1f;
	FPhysicsControlTarget DriveTarget;
	PhysicsControl->GetControlTarget(PelvisControl, DriveTarget);
	UE_LOG(
	    LogDelivery, Log,
	    TEXT(
	        "RecoveryTrace actor=%s world=%s time=%.3f dt=%.4f mode=%d limp=%d rise=%.2f airborne=%d drive=%d pre=%s post=%s mesh=%s actorPos=%s preV=%s postV=%s target=%s targetV=%s stepXY=%.1f feet=%d,%d"),
	    *GetNameSafe(GetOwner()), *GetNameSafe(GetWorld()), Now, DeltaTime, Mode, bIsLimp,
	    Recovery.GetRiseAlpha(), Movement.bPelvisAirborne, PhysicsControl->GetControlEnabled(PelvisControl),
	    *DiagnosticPrePhysicsPosition.ToCompactString(), *Position.ToCompactString(),
	    *Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace).ToCompactString(),
	    *GetOwner()->GetActorLocation().ToCompactString(), *DiagnosticPrePhysicsVelocity.ToCompactString(),
	    *Body->GetUnrealWorldVelocity().ToCompactString(), *DriveTarget.TargetPosition.ToCompactString(),
	    *DriveTarget.TargetVelocity.ToCompactString(), StepXY,
	    PhysicsControl->GetControlEnabled(Movement.LeftFoot.Control),
	    PhysicsControl->GetControlEnabled(Movement.RightFoot.Control));
}
#endif
