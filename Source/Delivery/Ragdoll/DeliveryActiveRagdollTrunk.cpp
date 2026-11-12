#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/DeliveryTrunkPassengerComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsControlComponent.h"
#include "PhysicsControlData.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/BodyInstance.h"

bool UDeliveryActiveRagdollComponent::BeginTrunkPose()
{
    if (!GetOwner()->HasAuthority() || !bIsActive || !Mesh || !PhysicsControl || !Mesh->GetPhysicsAsset()) return false;
    EndTrunkPose();
    const auto* Character = CastChecked<ADeliveryCharacter>(GetOwner());
    const auto* Settings = Character->TrunkPassenger.Get();
    const auto& Ref = Mesh->GetSkeletalMeshAsset()->GetRefSkeleton();
    TArray<FTransform> Pose;
    const FTransform MeshFrame(FRotator(0, -90, 0), FVector::ZeroVector, Mesh->GetComponentScale());
    for (int32 I=0; I<Ref.GetNum(); ++I)
        Pose.Add(Ref.GetRefBonePose()[I] * (Ref.GetParentIndex(I) >= 0 ? Pose[Ref.GetParentIndex(I)] : MeshFrame));
    const TArray<FTransform> Standing=Pose;
    const int32 Hips = Ref.FindBoneIndex(Bones.Hips);
    if (Hips == INDEX_NONE) return false;
    // 按参考骨段长度旋转整条子链，不独立拉伸手脚；方向在角色前/右/上坐标中配置
    const auto AimChain = [&](FName RootName, FName EndName, const FVector& Direction)
    {
        const int32 Root = Ref.FindBoneIndex(RootName), End = Ref.FindBoneIndex(EndName);
        if (Root == INDEX_NONE || End == INDEX_NONE) return;
        const FVector Pivot = Pose[Root].GetLocation();
        const FQuat Delta = FQuat::FindBetweenNormals((Pose[End].GetLocation()-Pivot).GetSafeNormal(), Direction.GetSafeNormal());
        for (int32 I=Root; I<Ref.GetNum(); ++I)
        {
            int32 Parent=I;
            while (Parent>Root) Parent=Ref.GetParentIndex(Parent);
            if (Parent!=Root) continue;
            Pose[I].SetLocation(Pivot + Delta.RotateVector(Pose[I].GetLocation()-Pivot));
            Pose[I].SetRotation((Delta*Pose[I].GetRotation()).GetNormalized());
        }
    };
    const auto& Fold=Settings->FoldedPose;
    const auto Mirror=[](FVector Direction) { Direction.Y=-Direction.Y; return Direction; };
    AimChain(Bones.Spine, Bones.Head, Fold.Spine);
    AimChain(Bones.LeftUpLeg, TEXT("LeftLeg"), Mirror(Fold.Thigh));
    AimChain(TEXT("LeftLeg"), TEXT("LeftFoot"), Mirror(Fold.Shin));
    AimChain(Bones.RightUpLeg, TEXT("RightLeg"), Fold.Thigh);
    AimChain(TEXT("RightLeg"), TEXT("RightFoot"), Fold.Shin);
    AimChain(Bones.LeftArm, TEXT("LeftForeArm"), Mirror(Fold.UpperArm));
    AimChain(TEXT("LeftForeArm"), TEXT("LeftHand"), Mirror(Fold.Forearm));
    AimChain(Bones.RightArm, TEXT("RightForeArm"), Fold.UpperArm);
    AimChain(TEXT("RightForeArm"), TEXT("RightHand"), Fold.Forearm);
    const FVector Origin = Pose[Hips].GetLocation();
    PhysicsControl->SetControlsInSetEnabled(TEXT("All"), false);
    for (const auto& Setup : Mesh->GetPhysicsAsset()->SkeletalBodySetups)
    {
        if (!Setup) continue;
        auto* Body = Mesh->GetBodyInstance(Setup->BoneName);
        const int32 Index = Ref.FindBoneIndex(Setup->BoneName);
        if (!Body || Index==INDEX_NONE) continue;
        FPhysicsControlData Data;
        Data.LinearStrength = Settings->FollowStrength;
        Data.AngularStrength = Settings->FollowStrength;
        Data.LinearDampingRatio = Data.AngularDampingRatio = 1.f;
        Data.bUseSkeletalAnimation = false;
        Data.bUseAccelerationDriveMode = true;
        Data.MaxForce = FMath::Max(Body->GetBodyMass(), .1f)*Settings->MaxLinearAcceleration;
        Data.MaxTorque = FMath::Max(Body->GetBodyInertiaTensor().GetMax(), 1.0)*FMath::DegreesToRadians(Settings->MaxAngularAcceleration);
        if (Setup->BoneName==Bones.Head || Setup->BoneName.ToString().Contains(TEXT("Foot")))
        { Data.AngularStrength *= .5f; }
        const auto Names = PhysicsControl->CreateControlsFromSkeletalMesh(Mesh, {Setup->BoneName},
            EPhysicsControlType::WorldSpace, Data, TEXT("TrunkPassenger"));
        if (Names.IsEmpty()) continue;
        auto& Entry = TrunkControls.AddDefaulted_GetRef();
        Entry.Bone=Setup->BoneName; Entry.Control=Names[0];
        Entry.Start=Entry.Target=Body->GetUnrealWorldTransform();
        Entry.Folded=Pose[Index]; Entry.Folded.SetLocation(Pose[Index].GetLocation()-Origin);
        Entry.Folded.SetScale3D(FVector::OneVector);
        Entry.Unfolded=Standing[Index];
        Entry.Unfolded.SetLocation(Standing[Index].GetLocation()-Standing[Hips].GetLocation());
        Entry.Unfolded.SetScale3D(FVector::OneVector);
        FPhysicsControlTarget Target;
        Target.TargetPosition=Entry.Start.GetLocation(); Target.TargetOrientation=Entry.Start.Rotator();
        Target.bApplyControlPointToTarget=true;
        PhysicsControl->SetControlTarget(Entry.Control,Target,true,true,false);
    }
    return !TrunkControls.IsEmpty();
}

void UDeliveryActiveRagdollComponent::UpdateTrunkPose(const FTransform& Frame, float Blend, float Unfold,
    const FVector& CarrierVelocity, float Dt)
{
    if (!PhysicsControl || Dt<=0.f) return;
    const auto* Settings = CastChecked<ADeliveryCharacter>(GetOwner())->TrunkPassenger.Get();
    const float Step = FMath::Min(Dt,.1f);
    for (auto& Entry : TrunkControls)
    {
        FTransform Local=Entry.Folded;
        // 与收拢姿势使用同一骨长，在出箱阶段逐渐展开髋、膝和双臂
        Local.Blend(Entry.Folded,Entry.Unfolded,Unfold);
        const FTransform Destination=Local*Frame;
        const FVector Desired=FMath::Lerp(Entry.Start.GetLocation(),Destination.GetLocation(),Blend);
        const FVector Previous=Entry.Target.GetLocation();
        const FVector Feed=CarrierVelocity*Step*Blend;
        const FVector Position=Previous+Feed+(Desired-Previous-Feed).GetClampedToMaxSize(Settings->PoseMoveSpeed*Step);
        const FQuat Rotation=FQuat::Slerp(Entry.Start.GetRotation(),Destination.GetRotation(),Blend).GetNormalized();
        const float Angle=Entry.Target.GetRotation().AngularDistance(Rotation);
        const float Alpha=Angle>SMALL_NUMBER ? FMath::Min(1.f,FMath::DegreesToRadians(Settings->PoseDegreesPerSecond)*Step/Angle):1.f;
        const FQuat Next=FQuat::Slerp(Entry.Target.GetRotation(),Rotation,Alpha).GetNormalized();
        FPhysicsControlTarget Target;
        Target.TargetPosition=Position; Target.TargetOrientation=Next.Rotator();
        Target.TargetVelocity=(Position-Previous)/Step;
        FVector Axis; float Radians;
        (Next*Entry.Target.GetRotation().Inverse()).ToAxisAndAngle(Axis,Radians);
        Target.TargetAngularVelocity=Axis*FMath::UnwindRadians(Radians)/(2.f*PI*Step);
        Target.bApplyControlPointToTarget=true;
        PhysicsControl->SetControlTarget(Entry.Control,Target,true,true,false);
        Entry.Target=FTransform(Next,Position);
    }
}

float UDeliveryActiveRagdollComponent::GetTrunkPoseError() const
{
    float Error=0.f;
    for (const auto& Entry : TrunkControls)
        if (const auto* Body=Mesh->GetBodyInstance(Entry.Bone))
            Error=FMath::Max(Error,static_cast<float>(FVector::Dist(Body->GetUnrealWorldTransform().GetLocation(),Entry.Target.GetLocation())));
    return Error;
}

FVector UDeliveryActiveRagdollComponent::GetPhysicalPelvisLocation() const
{
    const auto* Body=Mesh ? Mesh->GetBodyInstance(Bones.Hips):nullptr;
    return Body ? Body->GetUnrealWorldTransform().GetLocation() : GetOwner()->GetActorLocation();
}

void UDeliveryActiveRagdollComponent::EndTrunkPose()
{
    if (PhysicsControl)
        for (const auto& Entry : TrunkControls) PhysicsControl->DestroyControl(Entry.Control,true,false);
    TrunkControls.Reset();
}

void UDeliveryActiveRagdollComponent::ReleaseTrunkVelocity(float MaxSpeed)
{
    if (!Mesh || !Mesh->GetPhysicsAsset()) return;
    for (const auto& Setup : Mesh->GetPhysicsAsset()->SkeletalBodySetups)
        if (Setup) if (auto* Body=Mesh->GetBodyInstance(Setup->BoneName))
        {
            const FVector V=Body->GetUnrealWorldVelocity();
            const FVector Flat=FVector(V.X,V.Y,0).GetClampedToMaxSize(MaxSpeed);
            Body->SetLinearVelocity(FVector(Flat.X,Flat.Y,FMath::Clamp(V.Z,-200.0,200.0)),false);
        }
}

void UDeliveryActiveRagdollComponent::EmergencyMoveTrunkBody(const FVector& HipsLocation)
{
    // 仅失败/销毁回收路径平移整条链；正常装卸由有限力驱动，不逐帧瞬移
    ApplyOwnerTranslation(HipsLocation-GetPhysicalPelvisLocation());
}
