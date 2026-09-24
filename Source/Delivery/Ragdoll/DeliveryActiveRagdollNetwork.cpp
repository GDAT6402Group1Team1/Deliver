// 服务器采集各刚体快照，客户端据此平滑显示；不重新计算一套独立的角色运动。
#include "DeliveryActiveRagdollComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsControlComponent.h"

void UDeliveryActiveRagdollComponent::SyncOwnerToPelvis(float DeltaTime)
{
	// 网格是物理模拟的主体，角色胶囊只跟随髋部，供镜头和其他角色逻辑定位。
	// 平滑的是胶囊追赶“实际髋位置”的过程，不是改变髋部的物理运动。
	if (!Mesh || !Capsule)
	{
		return;
	}

	FVector Location = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
	Location.Z += CapsuleHipsZOffset;
	const FVector SmoothedLocation = FMath::VInterpTo(
		Capsule->GetComponentLocation(), Location, DeltaTime, CameraSmoothingSpeed);
	Capsule->SetWorldLocationAndRotation(SmoothedLocation, FRotator(0.0f, GetAimYaw(), 0.0f));
}

void UDeliveryActiveRagdollComponent::OnRep_ControlMode()
{
	// 服务器只同步当前是站立控制、晕倒还是停用；客户端按同一状态开关本地控制。
	switch (ReplicatedControlMode)
	{
	case EDeliveryRagdollControlMode::Active:
		if (bIsActive && (bClientRecoveryPlayback || bIsLimp))
		{
			if (GetOwner()->GetLocalRole() == ROLE_SimulatedProxy)
			{
				bClientRecoveryPlayback = false;
				bIsLimp = false;
				PhysicsControl->SetControlsInSetEnabled(TEXT("All"), false);
			}
			else
			{
				// 等 Active 之后的新快照抵达，避免用上一包倒地姿态开启本地电机。
				bClientRecoveryPlayback = true;
				bClientRecoveryAwaitingSnapshot = true;
				ClientRecoveryHandoffSequence = TargetSnapshot.Sequence;
			}
		}
		else
		{
			StartRagdoll();
		}
		break;
	case EDeliveryRagdollControlMode::Limp:
		bClientRecoveryPlayback = false;
		bClientRecoveryAwaitingSnapshot = false;
		if (!bIsActive)
		{
			StartRagdoll();
		}
		ApplyLimpState(true);
		break;
	case EDeliveryRagdollControlMode::Recovering:
		if (!bIsActive)
		{
			StartRagdoll();
		}
		ApplyLimpState(true);
		bClientRecoveryPlayback = true;
		bClientRecoveryAwaitingSnapshot = false;
		break;
	default:
		StopRagdoll();
		break;
	}
}

void UDeliveryActiveRagdollComponent::CaptureNetworkSnapshot()
{
	// 在物理步完成后记录各刚体，而不是发送“想站在哪里”的电机目标。
	// 远端看到的姿态由这些真实刚体位置/旋转重建。
	if (!Mesh || !GetWorld())
	{
		return;
	}

	++ReplicatedSnapshot.Sequence;
	// 时间戳与服务器时间同源，客户端可估计快照已经过去多久。
	if (const AGameStateBase* GameState = GetWorld()->GetGameState())
	{
		ReplicatedSnapshot.ServerTime = GameState->GetServerWorldTimeSeconds();
	}
	else
	{
		ReplicatedSnapshot.ServerTime = GetWorld()->GetTimeSeconds();
	}

	const FName SnapshotBones[] = {
		Bones.Hips, Bones.Spine, Bones.Head, LeftFoot.Bone, RightFoot.Bone,
		TEXT("LeftArm"), TEXT("LeftForeArm"), TEXT("LeftHand"),
		TEXT("RightArm"), TEXT("RightForeArm"), TEXT("RightHand")
	};
	ReplicatedSnapshot.Bodies.Reset(UE_ARRAY_COUNT(SnapshotBones));
	// 每条记录带骨骼名、实际刚体变换及速度；速度用于两包之间短暂预测。
	for (const FName Bone : SnapshotBones)
	{
		const FBodyInstance* Body = Mesh->GetBodyInstance(Bone);
		if (!Body)
		{
			continue;
		}

		const FTransform Transform = Body->GetUnrealWorldTransform();
		FDeliveryRagdollBodyState& State = ReplicatedSnapshot.Bodies.AddDefaulted_GetRef();
		State.Bone = Bone;
		State.Position = FVector_NetQuantize10(Transform.GetLocation());
		State.Rotation = Transform.Rotator();
		State.LinearVelocity = FVector_NetQuantize10(Body->GetUnrealWorldVelocity());
		State.AngularVelocity = FVector_NetQuantize10(
			Body->GetUnrealWorldAngularVelocityInRadians());
	}

	GetOwner()->ForceNetUpdate();
}

void UDeliveryActiveRagdollComponent::OnRep_RagdollSnapshot()
{
	// 留住上一包和新包，下一帧才能在两套身体姿态之间平滑过渡。
	PreviousSnapshot = bHasNetworkSnapshot ? TargetSnapshot : ReplicatedSnapshot;
	TargetSnapshot = ReplicatedSnapshot;
	SnapshotReceivedAt = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	bHasNetworkSnapshot = true;
	if (bClientRecoveryAwaitingSnapshot
		&& TargetSnapshot.Sequence != ClientRecoveryHandoffSequence)
	{
		FinishClientRecoveryPlayback();
	}
}

void UDeliveryActiveRagdollComponent::FinishClientRecoveryPlayback()
{
	if (!bClientRecoveryAwaitingSnapshot || !PhysicsControl || !Mesh
		|| TargetSnapshot.Bodies.IsEmpty()
		|| ReplicatedControlMode != EDeliveryRagdollControlMode::Active)
	{
		return;
	}
	// 电机仍关闭时先应用完整的新姿态；之后以它为起点接管本地行走。
	for (const FDeliveryRagdollBodyState& State : TargetSnapshot.Bodies)
	{
		if (FBodyInstance* Body = Mesh->GetBodyInstance(State.Bone))
		{
			Body->SetBodyTransform(FTransform(State.Rotation.Quaternion(), FVector(State.Position)),
				ETeleportType::TeleportPhysics);
			Body->SetLinearVelocity(FVector(State.LinearVelocity), false);
			Body->SetAngularVelocityInRadians(FVector(State.AngularVelocity), false);
		}
	}
	bClientRecoveryAwaitingSnapshot = false;
	bClientRecoveryPlayback = false;
	bIsLimp = false;
	bRisingFromLimp = false;
	LimpRecoveryRiseAlpha = 1.0f;
	ReseedFromCurrentPose();
	SetLimpRecoveryDriveStrength(1.0f);
	PhysicsControl->SetControlsInSetEnabled(TEXT("All"), true);
	PhysicsControl->SetControlEnabled(LeftFoot.Control, false, true, false);
	PhysicsControl->SetControlEnabled(RightFoot.Control, false, true, false);
	bHasOwnerCorrectionSequence = false;
	UpdateControlTargets(0.0f);
}

void UDeliveryActiveRagdollComponent::ApplyNetworkSnapshot(float DeltaTime)
{
	// 远端玩家：在上/下两包之间插值；包暂时没到时用速度短暂外推。
	// 本机玩家的电机也在模拟，不能把每块刚体分别传送到服务端姿态。
	if (!bHasNetworkSnapshot || !Mesh || TargetSnapshot.Bodies.IsEmpty() || !GetWorld())
	{
		return;
	}

	const bool bAutonomous = GetOwner()->GetLocalRole() == ROLE_AutonomousProxy;
	const bool bRecoveryPlayback = bClientRecoveryPlayback
		|| ReplicatedControlMode == EDeliveryRagdollControlMode::Recovering;
	const bool bOwnerPrediction = bAutonomous && !bIsLimp && !bRecoveryPlayback;
	const float SnapshotInterval = 1.0f / FMath::Max(NetworkSnapshotRate, 1.0f);
	const float TimeSinceReceive = GetWorld()->GetTimeSeconds() - SnapshotReceivedAt;
	const float InterpolationAlpha = bAutonomous && !bRecoveryPlayback
		? 1.0f
		: FMath::Clamp(TimeSinceReceive / SnapshotInterval, 0.0f, 1.0f);

	float ExtrapolationTime = FMath::Max(0.0f, TimeSinceReceive - SnapshotInterval);
	if (bAutonomous)
	{
		if (const AGameStateBase* GameState = GetWorld()->GetGameState())
		{
			ExtrapolationTime = FMath::Max(
				0.0f, GameState->GetServerWorldTimeSeconds() - TargetSnapshot.ServerTime);
		}
	}
	ExtrapolationTime = FMath::Min(ExtrapolationTime, MaxSnapshotExtrapolation);
	// 外推必须有上限，网络暂停时不能让身体无限沿旧速度飞走。
	if (bOwnerPrediction)
	{
		// 每个新快照只校正一次整体位置。逐刚体 TeleportPhysics 会在起身时
		// 不断改变关节两端的相对变换，与本地电机/约束形成高频拉扯。
		if (bHasOwnerCorrectionSequence && LastOwnerCorrectionSequence == TargetSnapshot.Sequence)
		{
			return;
		}
		LastOwnerCorrectionSequence = TargetSnapshot.Sequence;
		bHasOwnerCorrectionSequence = true;
		const FDeliveryRagdollBodyState* PelvisState = TargetSnapshot.Bodies.FindByPredicate(
			[this](const FDeliveryRagdollBodyState& State) { return State.Bone == Bones.Hips; });
		const FBodyInstance* PelvisBody = PelvisState ? Mesh->GetBodyInstance(Bones.Hips) : nullptr;
		const UPhysicsAsset* PhysicsAsset = Mesh->GetPhysicsAsset();
		if (!PelvisState || !PelvisBody || !PhysicsAsset)
		{
			return;
		}
		const FVector TargetPelvis = FVector(PelvisState->Position)
			+ FVector(PelvisState->LinearVelocity) * ExtrapolationTime;
		const FVector Error = TargetPelvis - PelvisBody->GetUnrealWorldTransform().GetLocation();
		if (Error.SizeSquared() <= FMath::Square(5.0f))
		{
			return;
		}
		const bool bHardCorrection = Error.Size() > OwnerHardCorrectionDistance;
		const float Alpha = bHardCorrection ? 1.0f
			: 1.0f - FMath::Exp(-OwnerCorrectionSpeed * SnapshotInterval);
		const FVector Translation = bHardCorrection
			? Error : (Error * Alpha).GetClampedToMaxSize(12.0f);
		for (const TObjectPtr<USkeletalBodySetup>& Setup : PhysicsAsset->SkeletalBodySetups)
		{
			if (Setup)
			{
				if (FBodyInstance* Body = Mesh->GetBodyInstance(Setup->BoneName))
				{
					FTransform Transform = Body->GetUnrealWorldTransform();
					Transform.AddToTranslation(Translation);
					Body->SetBodyTransform(Transform, ETeleportType::TeleportPhysics);
				}
			}
		}
		return;
	}

	const int32 BodyCount = FMath::Min(
		PreviousSnapshot.Bodies.Num(), TargetSnapshot.Bodies.Num());
	const float CorrectionAlpha = bAutonomous && !bRecoveryPlayback
		? 1.0f - FMath::Exp(-OwnerCorrectionSpeed * DeltaTime) : 1.0f;

	for (int32 Index = 0; Index < BodyCount; ++Index)
	{
		// 每块刚体分别更新。只同步髋而让其他骨骼自己模拟，会使整条身体链
		// 与服务器的受击、抓取姿势逐渐分离。
		const FDeliveryRagdollBodyState& Previous = PreviousSnapshot.Bodies[Index];
		const FDeliveryRagdollBodyState& Target = TargetSnapshot.Bodies[Index];
		FBodyInstance* Body = Mesh->GetBodyInstance(Target.Bone);
		if (!Body)
		{
			continue;
		}

		FVector TargetPosition = FMath::Lerp(
			FVector(Previous.Position), FVector(Target.Position), InterpolationAlpha);
		TargetPosition += FVector(Target.LinearVelocity) * ExtrapolationTime;
		const FQuat TargetRotation = FQuat::Slerp(
			Previous.Rotation.Quaternion(), Target.Rotation.Quaternion(), InterpolationAlpha);

		const FTransform Current = Body->GetUnrealWorldTransform();
		const FVector CorrectedPosition = FMath::Lerp(
			Current.GetLocation(), TargetPosition, CorrectionAlpha);
		const FQuat CorrectedRotation = FQuat::Slerp(
			Current.GetRotation(), TargetRotation, CorrectionAlpha).GetNormalized();
		Body->SetBodyTransform(
			FTransform(CorrectedRotation, CorrectedPosition), ETeleportType::TeleportPhysics);

		const FVector TargetLinearVelocity = FMath::Lerp(
			FVector(Previous.LinearVelocity), FVector(Target.LinearVelocity), InterpolationAlpha);
		const FVector TargetAngularVelocity = FMath::Lerp(
			FVector(Previous.AngularVelocity), FVector(Target.AngularVelocity), InterpolationAlpha);
		const FVector LinearVelocity = FMath::Lerp(
			Body->GetUnrealWorldVelocity(), TargetLinearVelocity, CorrectionAlpha);
		const FVector AngularVelocity = FMath::Lerp(
			Body->GetUnrealWorldAngularVelocityInRadians(), TargetAngularVelocity, CorrectionAlpha);
		Body->SetLinearVelocity(LinearVelocity, false);
		Body->SetAngularVelocityInRadians(AngularVelocity, false);
	}
}
