// 把服务器的实际刚体姿态同步给客户端，并按拥有关系采用不同显示方式
// 远端角色播放快照；本机角色保持输入响应，并接受服务器的位置校正
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsControlComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/DeliveryTrunkPassengerComponent.h"
#include "Vehicle/DeliveryMotorbike.h"

// 角色根位置与控制状态

// 让角色根位置跟随物理髋部，保持镜头和交互位置一致
void UDeliveryActiveRagdollComponent::SyncOwnerToPelvis(float DeltaTime)
{
	// 网格是物理模拟的主体，角色胶囊只跟随髋部，供镜头和其他角色逻辑定位
	// 平滑的是胶囊追赶“实际髋位置”的过程，不是改变髋部的物理运动
	if (!Mesh || !Capsule)
	{
		return;
	}

	FVector Location = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
	Location.Z += CapsuleHipsZOffset;
	const FVector SmoothedLocation =
	    FMath::VInterpTo(Capsule->GetComponentLocation(), Location, DeltaTime, CameraSmoothingSpeed);
	Capsule->SetWorldLocationAndRotation(SmoothedLocation, FRotator(0.0f, GetAimYaw(), 0.0f));
}

// 收到服务器身体模式后更新客户端控制状态
void UDeliveryActiveRagdollComponent::OnRep_ControlMode()
{
	// 服务器只同步当前是站立控制、晕倒还是停用；客户端按同一状态开关本地控制
	switch (ReplicatedControlMode)
	{
	case EDeliveryRagdollControlMode::Active:
		if (bIsActive && (Network.IsPlayingRecovery() || bIsLimp))
		{
			if (GetOwner()->GetLocalRole() == ROLE_SimulatedProxy)
			{
				Network.SetRecoveryPlayback(false);
				bIsLimp = false;
				PhysicsControl->SetControlsInSetEnabled(TEXT("All"), false);
			}
			else
			{
				// 等 Active 之后的新快照抵达，避免用上一包倒地姿态开启本地电机
				Network.WaitForActiveSnapshot();
			}
		}
		else
		{
			StartRagdoll();
		}
		break;
	case EDeliveryRagdollControlMode::Limp:
		Network.ResetRecovery();
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
		Network.ResetRecovery();
		Network.SetRecoveryPlayback(true);
		break;
	default:
		StopRagdoll();
		break;
	}
}

// 服务器姿态采样

// 在物理更新后记录身体各部位的位置、旋转和速度
void UDeliveryActiveRagdollComponent::CaptureNetworkSnapshot()
{
	// 在物理步完成后记录各刚体，而不是发送“想站在哪里”的电机目标
	// 远端看到的姿态由这些真实刚体位置/旋转重建
	if (!Mesh || !Mesh->GetPhysicsAsset() || !GetWorld())
	{
		return;
	}

	++ReplicatedSnapshot.Sequence;
    const auto* Character=Cast<ADeliveryCharacter>(GetOwner());
    const auto* Passenger=Character ? Character->TrunkPassenger.Get():nullptr;
    ReplicatedSnapshot.Carrier=Passenger && Passenger->IsPassenger() ? Passenger->GetVehicle():nullptr;
    if (IsValid(ReplicatedSnapshot.Carrier))
    {
        ReplicatedSnapshot.CarrierFrame=ReplicatedSnapshot.Carrier->GetPassengerFrame();
        ReplicatedSnapshot.CarrierVelocity=Passenger->GetCarrierVelocity();
    }
	// 时间戳与服务器时间同源，客户端可估计快照已经过去多久
	if (const AGameStateBase* GameState = GetWorld()->GetGameState())
	{
		ReplicatedSnapshot.ServerTime = GameState->GetServerWorldTimeSeconds();
	}
	else
	{
		ReplicatedSnapshot.ServerTime = GetWorld()->GetTimeSeconds();
	}

	const UPhysicsAsset* PhysicsAsset = Mesh->GetPhysicsAsset();
	ReplicatedSnapshot.Bodies.Reset(PhysicsAsset->SkeletalBodySetups.Num());
	// 腿和躯干也必须同步，否则远端脚已移动，膝盖却仍由本地物理猜测
	for (const TObjectPtr<USkeletalBodySetup>& Setup : PhysicsAsset->SkeletalBodySetups)
	{
		if (!Setup)
			continue;
		const FName Bone = Setup->BoneName;
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
		State.AngularVelocity = FVector_NetQuantize10(Body->GetUnrealWorldAngularVelocityInRadians());
	}

	GetOwner()->ForceNetUpdate();
}

// 客户端接收姿态

// 保存新收到的物理快照，供远端平滑播放和本机校正
void UDeliveryActiveRagdollComponent::OnRep_RagdollSnapshot()
{
	// 留住上一包和新包，下一帧才能在两套身体姿态之间平滑过渡
	if (Network.Receive(ReplicatedSnapshot, GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f))
	{
		FinishClientRecoveryPlayback();
	}
}

// 用恢复后的服务器姿态重新建立控制目标，再交回本机操作
void UDeliveryActiveRagdollComponent::FinishClientRecoveryPlayback()
{
	if (!Network.IsAwaitingResume() || !PhysicsControl || !Mesh || Network.GetTarget().Bodies.IsEmpty() ||
	    ReplicatedControlMode != EDeliveryRagdollControlMode::Active)
	{
		return;
	}
	// 电机仍关闭时先应用完整的新姿态；之后以它为起点接管本地行走
	for (const FDeliveryRagdollBodyState& State : Network.GetTarget().Bodies)
	{
		if (FBodyInstance* Body = Mesh->GetBodyInstance(State.Bone))
		{
			Body->SetBodyTransform(FTransform(State.Rotation.Quaternion(), FVector(State.Position)),
			                       ETeleportType::TeleportPhysics);
			Body->SetLinearVelocity(FVector(State.LinearVelocity), false);
			Body->SetAngularVelocityInRadians(FVector(State.AngularVelocity), false);
		}
	}
	Network.ResetRecovery();
	bIsLimp = false;
	Recovery.FinishRise();
	ReseedFromCurrentPose();
	SetLimpRecoveryDriveStrength(1.0f);
	PhysicsControl->SetControlsInSetEnabled(TEXT("All"), true);
	PhysicsControl->SetControlEnabled(Movement.LeftFoot.Control, false, true, false);
	PhysicsControl->SetControlEnabled(Movement.RightFoot.Control, false, true, false);
	Network.ResetOwnerCorrection();
	UpdateControlTargets(0.0f);
}

// 客户端物理校正

// 远端播放服务器姿态，本机保留物理运动并修正位置偏差
// 位置使用速度作短时推算；旋转在快照之间平滑过渡
void UDeliveryActiveRagdollComponent::ApplyNetworkSnapshot(float DeltaTime)
{
	// 远端玩家：在上/下两包之间插值；包暂时没到时用速度短暂外推
	// 本机玩家的电机也在模拟，不能把每块刚体分别传送到服务端姿态
	if (!Network.HasSnapshot() || !Mesh || Network.GetTarget().Bodies.IsEmpty() || !GetWorld())
	{
		return;
	}

	const bool bAutonomous = GetOwner()->GetLocalRole() == ROLE_AutonomousProxy;
	const auto* PassengerOwner = Cast<ADeliveryCharacter>(GetOwner());
    const bool bPassenger = PassengerOwner && PassengerOwner->TrunkPassenger && PassengerOwner->TrunkPassenger->IsPassenger();
	const bool bRecoveryPlayback = bPassenger ||
	    Network.IsPlayingRecovery() || ReplicatedControlMode == EDeliveryRagdollControlMode::Recovering;
	// 仅正常行动的本机角色使用预测校正；倒地和恢复阶段进入快照姿态处理
	const bool bOwnerPrediction = bAutonomous && !bIsLimp && !bRecoveryPlayback;
	// 快照间隔和接收后经过时间均以秒为单位，用于计算两包之间的播放进度
	const float SnapshotInterval = 1.0f / FMath::Max(NetworkSnapshotRate, 1.0f);
	const float TimeSinceReceive = GetWorld()->GetTimeSeconds() - Network.GetReceivedAt();
	const float InterpolationAlpha = bAutonomous && !bRecoveryPlayback
	                                     ? 1.0f
	                                     : FMath::Clamp(TimeSinceReceive / SnapshotInterval, 0.0f, 1.0f);

	// 远端在插值区间结束后才推算位置；本机可用服务器时间估计快照实际延迟
	float ExtrapolationTime = FMath::Max(0.0f, TimeSinceReceive - SnapshotInterval);
	if (bAutonomous)
	{
		if (const AGameStateBase* GameState = GetWorld()->GetGameState())
		{
			ExtrapolationTime =
			    FMath::Max(0.0f, GameState->GetServerWorldTimeSeconds() - Network.GetTarget().ServerTime);
		}
	}
	ExtrapolationTime = FMath::Min(ExtrapolationTime, MaxSnapshotExtrapolation);
	// 外推必须有上限，网络暂停时不能让身体无限沿旧速度飞走
	if (bOwnerPrediction)
	{
		// 每个快照只处理一次，避免重复叠加同一份校正速度
		if (!Network.ConsumeOwnerCorrection())
			return;
		const FDeliveryRagdollBodyState* PelvisState = Network.GetTarget().Bodies.FindByPredicate(
		    [this](const FDeliveryRagdollBodyState& State)
		    {
			    return State.Bone == Bones.Hips;
		    });
		const FBodyInstance* PelvisBody = PelvisState ? Mesh->GetBodyInstance(Bones.Hips) : nullptr;
		const UPhysicsAsset* PhysicsAsset = Mesh->GetPhysicsAsset();
		if (!PelvisState || !PelvisBody || !PhysicsAsset)
		{
			return;
		}
		const FVector TargetPelvis =
		    FVector(PelvisState->Position) + FVector(PelvisState->LinearVelocity) * ExtrapolationTime;
		// 比较服务器与本机的髋部位置，小幅偏差不处理，避免身体反复抖动
		const FVector Error = TargetPelvis - PelvisBody->GetUnrealWorldTransform().GetLocation();
		if (Error.SizeSquared() <= FMath::Square(5.0f))
		{
			return;
		}
		if (Error.Size() > OwnerHardCorrectionDistance)
		{
			// 严重偏离时才直接对齐，同时移动迈步目标
			ApplyOwnerTranslation(Error);
		}
		else
		{
			// 小偏差通过速度追上，避免每包快照都传送刚体、打断本机物理运动
			const float Alpha = 1.0f - FMath::Exp(-OwnerCorrectionSpeed * SnapshotInterval);
			// 停步目标也逐渐对齐服务器，避免位置控制抵消网络校正
			Movement.CorrectStandAnchor(Error, Alpha);
			const FVector DesiredVelocity = FVector(PelvisState->LinearVelocity) + Error * OwnerCorrectionSpeed;
			const FVector VelocityChange =
			    ((DesiredVelocity - PelvisBody->GetUnrealWorldVelocity()) * Alpha).GetClampedToMaxSize(50.0f);
			if (VelocityChange.IsNearlyZero())
				return;
			// 把修正交给物理模拟，不直接覆盖每帧回传的速度
			// 全身增加相同速度，保留手脚相对身体的运动
			for (const TObjectPtr<USkeletalBodySetup>& Setup : PhysicsAsset->SkeletalBodySetups)
			{
				if (Setup)
				{
					if (FBodyInstance* Body = Mesh->GetBodyInstance(Setup->BoneName))
						Body->AddImpulse(VelocityChange, true);
				}
			}
		}
		return;
	}

	// 只处理两份快照共同拥有的索引范围；此处按数组顺序配对刚体状态
	const int32 BodyCount = FMath::Min(Network.GetPrevious().Bodies.Num(), Network.GetTarget().Bodies.Num());
	const float CorrectionAlpha =
	    bAutonomous && !bRecoveryPlayback ? 1.0f - FMath::Exp(-OwnerCorrectionSpeed * DeltaTime) : 1.0f;
    auto* Carrier=bPassenger ? PassengerOwner->TrunkPassenger->GetVehicle():nullptr;
    const bool bCarrierSpace=IsValid(Carrier) && Network.GetTarget().Carrier==Carrier;
    const FTransform CurrentCarrier=bCarrierSpace ? Carrier->GetPassengerFrame():FTransform::Identity;

	for (int32 Index = 0; Index < BodyCount; ++Index)
	{
		// 每块刚体分别更新；只同步髋而让其他骨骼自己模拟，会使整条身体链
		// 与服务器的受击、抓取姿势逐渐分离
		const FDeliveryRagdollBodyState& Previous = Network.GetPrevious().Bodies[Index];
		const FDeliveryRagdollBodyState& Target = Network.GetTarget().Bodies[Index];
		FBodyInstance* Body = Mesh->GetBodyInstance(Target.Bone);
		if (!Body)
		{
			continue;
		}

		FVector TargetPosition =
		    FMath::Lerp(FVector(Previous.Position), FVector(Target.Position), InterpolationAlpha);
		TargetPosition += FVector(Target.LinearVelocity) * ExtrapolationTime;
		FQuat TargetRotation =
		    FQuat::Slerp(Previous.Rotation.Quaternion(), Target.Rotation.Quaternion(), InterpolationAlpha);
        FVector TargetLinearVelocity=FMath::Lerp(FVector(Previous.LinearVelocity),FVector(Target.LinearVelocity),InterpolationAlpha);
        if (bCarrierSpace)
        {
            // 分别还原两包的车体局部姿态，再放到本机车辆当前参考中
            const auto& Snapshot=Network.GetTarget();
            const FTransform LocalTarget=FTransform(Target.Rotation,FVector(Target.Position)).GetRelativeTransform(Snapshot.CarrierFrame);
            FTransform Local=LocalTarget;
            if (Network.GetPrevious().Carrier==Carrier)
            {
                const FTransform LocalPrevious=FTransform(Previous.Rotation,FVector(Previous.Position)).GetRelativeTransform(Network.GetPrevious().CarrierFrame);
                Local.Blend(LocalPrevious,LocalTarget,InterpolationAlpha);
            }
            const FVector RelativeVelocity=Snapshot.CarrierFrame.InverseTransformVectorNoScale(FVector(Target.LinearVelocity)-FVector(Snapshot.CarrierVelocity));
            Local.AddToTranslation(RelativeVelocity*ExtrapolationTime);
            const FTransform WorldPose=Local*CurrentCarrier;
            TargetPosition=WorldPose.GetLocation(); TargetRotation=WorldPose.GetRotation();
            TargetLinearVelocity=CurrentCarrier.TransformVectorNoScale(RelativeVelocity)+PassengerOwner->TrunkPassenger->GetCarrierVelocity();
        }

		const FTransform Current = Body->GetUnrealWorldTransform();
		const FVector CorrectedPosition = FMath::Lerp(Current.GetLocation(), TargetPosition, CorrectionAlpha);
		const FQuat CorrectedRotation =
		    FQuat::Slerp(Current.GetRotation(), TargetRotation, CorrectionAlpha).GetNormalized();
		Body->SetBodyTransform(FTransform(CorrectedRotation, CorrectedPosition),
		                       ETeleportType::TeleportPhysics);

		const FVector TargetAngularVelocity = FMath::Lerp(
		    FVector(Previous.AngularVelocity), FVector(Target.AngularVelocity), InterpolationAlpha);
		const FVector LinearVelocity =
		    FMath::Lerp(Body->GetUnrealWorldVelocity(), TargetLinearVelocity, CorrectionAlpha);
		const FVector AngularVelocity = FMath::Lerp(Body->GetUnrealWorldAngularVelocityInRadians(),
		                                            TargetAngularVelocity, CorrectionAlpha);
		Body->SetLinearVelocity(LinearVelocity, false);
		Body->SetAngularVelocityInRadians(AngularVelocity, false);
	}
}

// 网络修正只改变位置，不重启动作，也不改变身体速度
void UDeliveryActiveRagdollComponent::ApplyOwnerTranslation(const FVector& Translation)
{
	if (!Mesh || !Mesh->GetPhysicsAsset() || !PhysicsControl)
	{
		return;
	}

	for (const TObjectPtr<USkeletalBodySetup>& Setup : Mesh->GetPhysicsAsset()->SkeletalBodySetups)
	{
		if (!Setup)
			continue;
		if (FBodyInstance* Body = Mesh->GetBodyInstance(Setup->BoneName))
		{
			FTransform Transform = Body->GetUnrealWorldTransform();
			Transform.AddToTranslation(Translation);
			Body->SetBodyTransform(Transform, ETeleportType::TeleportPhysics);
		}
	}

	// 身体移动后，迈步起点和落点也要一起移动，避免脚被拉回旧位置
	Movement.Translate(Translation);
	PunchSupportLocation += Translation;

	// 保留目标速度，否则下一帧会把网络位移误算成脚的运动速度
	// 胸部和头部使用相对坐标，不参与世界位置修正
	for (const FName Control : {PelvisControl, Movement.LeftFoot.Control, Movement.RightFoot.Control})
	{
		FPhysicsControlTarget Target;
		if (PhysicsControl->GetControlTarget(Control, Target))
		{
			Target.TargetPosition += Translation;
			PhysicsControl->SetControlTarget(Control, Target, false, true, false);
		}
	}
}
