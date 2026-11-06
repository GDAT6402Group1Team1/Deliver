#include "Ragdoll/Debug/DeliveryPoseDebug.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"

#if WITH_EDITOR
#include "Components/SkeletalMeshComponent.h"
#include "PhysicsControlComponent.h"
#include "PhysicsEngine/BodyInstance.h"

bool UDeliveryActiveRagdollComponent::ReadPoseDebug(FDeliveryPoseDebug& Out) const
{
    Out = FDeliveryPoseDebug();
    if (!bIsActive || !Mesh || !PhysicsControl || !Mesh->GetBodyInstance(Bones.Hips)) return false;
    Out.Hips = Mesh->GetCenterOfMass(Bones.Hips);
    Out.HipTarget = Movement.PlannedPelvisTarget;
    Out.Right = GetSlopeRight(FVector::ZeroVector);
    Out.Upright = GetUprightDot();
    Out.MinimumUpright = MinimumStepUprightDot;
    Out.bGrounded = !Movement.bPelvisAirborne && !Movement.bJumping;
    Out.bLimp = bIsLimp;
    Out.bRising = Recovery.IsRising() || ReplicatedControlMode == EDeliveryRagdollControlMode::Recovering;
    Out.bFootRecovery = Recovery.ShouldPauseWalking();
    Out.bArmRecovery = Recovery.IsRecoveringArms();
    Out.bPunching = Punch.IsActive();
    Out.bReleased = Punch.IsReleased();
    Out.bReturning = BoxingPose.HasPendingPunchRecovery() && !Punch.IsActive();

    const FDeliveryRagdollFoot* Feet[] = {&Movement.LeftFoot, &Movement.RightFoot};
    for (int32 Index = 0; Index < 2; ++Index)
    {
        const FDeliveryRagdollFoot& Foot = *Feet[Index];
        Out.Feet[Index] = Mesh->GetCenterOfMass(Foot.Bone);
        FPhysicsControlTarget Target;
        Out.FootDriven[Index] = PhysicsControl->GetControlEnabled(Foot.Control) &&
            PhysicsControl->GetControlTarget(Foot.Control, Target);
        // 显示本帧电机目标，而不是尚未到达的最终落点
        Out.FootTargets[Index] = Out.FootDriven[Index] ? Target.TargetPosition : Out.Feet[Index];
        Out.FootErrors[Index] = FVector::Distance(Out.Feet[Index], Out.FootTargets[Index]);
        Out.bFootLate |= Foot.Alpha < 1.f && Foot.Elapsed > ControlledStrideDuration &&
            Out.FootErrors[Index] > 18.f;

        const FDeliveryBoxingPose::FArm& Arm = BoxingPose.Arms[Index];
        const FBodyInstance* Upper = Mesh->GetBodyInstance(Arm.Upper);
        const FBodyInstance* Lower = Mesh->GetBodyInstance(Arm.Lower);
        const FBodyInstance* Hand = Mesh->GetBodyInstance(Arm.Hand);
        FPhysicsControlTarget UpperTarget, LowerTarget;
        Out.ArmValid[Index] = Arm.bReady && Upper && Lower && Hand &&
            PhysicsControl->GetControlTarget(Arm.UpperControl, UpperTarget) &&
            PhysicsControl->GetControlTarget(Arm.LowerControl, LowerTarget);
        if (!Out.ArmValid[Index]) continue;
        const FTransform UpperPose = Upper->GetUnrealWorldTransform();
        const FTransform LowerPose = Lower->GetUnrealWorldTransform();
        Out.Shoulders[Index] = UpperPose.GetLocation();
        Out.Elbows[Index] = LowerPose.GetLocation();
        Out.Hands[Index] = Hand->GetUnrealWorldTransform().GetLocation();
        // 上臂目标是世界旋转，小臂目标是相对上臂旋转，不能直接混用坐标系
        const float UpperError = UpperPose.GetRotation().AngularDistance(UpperTarget.TargetOrientation.Quaternion());
        const FQuat RelativeLower = UpperPose.GetRotation().Inverse() * LowerPose.GetRotation();
        const float LowerError = RelativeLower.AngularDistance(LowerTarget.TargetOrientation.Quaternion());
        Out.ArmErrors[Index] = FMath::RadiansToDegrees(FMath::Max(UpperError, LowerError));
    }
    Out.Separation = FVector::DotProduct(Out.Feet[1] - Out.Feet[0], Out.Right);
    Out.bCrossed = Out.Separation < -FootCrossDistance;
    return true;
}
#endif
