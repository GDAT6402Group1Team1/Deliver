// Copyright Epic Games, Inc. All Rights Reserved.

/*
每一帧是：

用水平 Wish 和速度差，算出髋接下来要去的水平位置。
在这个位置竖直探测地面，高度 = 地面 + 站立身高。这里探测不到，再退回当前髋下方探测一次，避免把髋吊到半空。
电机把髋拉向这个完整目标 PlannedPelvisTarget。
脚的落点从同一个 PlannedPelvisTarget 推出，再探测地面；摆动脚沿抬脚曲线跟过去。选哪只脚迈步仍看当前身体姿态。
*/

#include "DeliveryActiveRagdollComponent.h"

#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"
#include "PhysicsControlComponent.h"
#include "PhysicsControlData.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "../Delivery.h"

namespace
{
	const FName PelvisSet(TEXT("Ragdoll.Pelvis"));
	const FName ChestSet(TEXT("Ragdoll.Chest"));
	const FName FeetSet(TEXT("Ragdoll.Feet"));
	const FName TorsoSet(TEXT("Ragdoll.Torso"));
	const FName HeadSet(TEXT("Ragdoll.Head"));
	const FName LegsSet(TEXT("Ragdoll.Legs"));
	const FName FootPostureSet(TEXT("Ragdoll.FootPosture"));
	const FName ArmsSet(TEXT("Ragdoll.Arms"));

	FPhysicsControlData MakeAngularControl(float Strength, float Damping)
	{
		FPhysicsControlData Data;
		Data.AngularStrength = Strength;
		Data.AngularDampingRatio = Damping;
		Data.bUseSkeletalAnimation = true;
		Data.bUseAccelerationDriveMode = true;
		Data.bDisableCollision = true;
		return Data;
	}
}

UDeliveryActiveRagdollComponent::UDeliveryActiveRagdollComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	SetIsReplicatedByDefault(true);

	PostPhysicsTickFunction.bCanEverTick = true;
	PostPhysicsTickFunction.bStartWithTickEnabled = false;
	PostPhysicsTickFunction.TickGroup = TG_PostPhysics;

	PhysicsControl = CreateDefaultSubobject<UPhysicsControlComponent>(TEXT("PhysicsControl"));
}

void UDeliveryActiveRagdollComponent::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UDeliveryActiveRagdollComponent, ReplicatedControlMode);
	DOREPLIFETIME(UDeliveryActiveRagdollComponent, ReplicatedSnapshot);
}

void UDeliveryActiveRagdollComponent::RegisterComponentTickFunctions(bool bRegister)
{
	Super::RegisterComponentTickFunctions(bRegister);

	if (bRegister)
	{
		if (SetupActorComponentTickFunction(&PostPhysicsTickFunction))
		{
			PostPhysicsTickFunction.Target = this;
		}
	}
	else if (PostPhysicsTickFunction.IsTickFunctionRegistered())
	{
		PostPhysicsTickFunction.UnRegisterTickFunction();
	}
}

void UDeliveryActiveRagdollComponent::BeginPlay()
{
	Super::BeginPlay();
	ResolveOwnerComponents();

	if (bStartOnBeginPlay)
	{
		if (GetOwner() && GetOwner()->HasAuthority())
		{
			ReplicatedControlMode = EDeliveryRagdollControlMode::Active;
		}
		StartRagdoll();
	}
}

void UDeliveryActiveRagdollComponent::TickComponent(
	float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bIsActive)
	{
		return;
	}

	if (ThisTickFunction == &PostPhysicsTickFunction)
	{
		if (GetOwner()->HasAuthority())
		{
			SnapshotAccumulator += DeltaTime;
			if (SnapshotAccumulator >= 1.0f / FMath::Max(NetworkSnapshotRate, 1.0f))
			{
				SnapshotAccumulator = 0.0f;
				CaptureNetworkSnapshot();
			}
		}

		if (GetOwner()->HasAuthority() || GetOwner()->GetLocalRole() == ROLE_AutonomousProxy)
		{
			SyncOwnerToPelvis(DeltaTime);
		}
		return;
	}

	if (GetOwner()->GetLocalRole() == ROLE_SimulatedProxy)
	{
		ApplyNetworkSnapshot(DeltaTime);
		return;
	}

	if (GetOwner()->GetLocalRole() == ROLE_AutonomousProxy)
	{
		ApplyNetworkSnapshot(DeltaTime);
	}

	if (!bIsLimp)
	{
		// 物理积分之前写入本帧的髋目标和脚目标。
		UpdateControlTargets(DeltaTime);
	}
}

void UDeliveryActiveRagdollComponent::ResolveOwnerComponents()
{
	if (AActor* Owner = GetOwner())
	{
		Mesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
		Capsule = Owner->FindComponentByClass<UCapsuleComponent>();
	}
}

bool UDeliveryActiveRagdollComponent::ValidateSetup() const
{
	if (!Mesh || !Capsule || !PhysicsControl)
	{
		UE_LOG(LogDelivery, Error, TEXT("%s: Active ragdoll requires a skeletal mesh and capsule."), *GetNameSafe(GetOwner()));
		return false;
	}

	if (!Mesh->GetPhysicsAsset())
	{
		UE_LOG(LogDelivery, Error, TEXT("%s: Skeletal mesh has no Physics Asset."), *GetNameSafe(GetOwner()));
		return false;
	}

	const FName Required[] = {
		Bones.Hips, Bones.Spine, Bones.Head, Bones.LeftUpLeg, Bones.LeftFoot,
		Bones.RightUpLeg, Bones.RightFoot, Bones.LeftArm, Bones.RightArm
	};

	for (const FName Bone : Required)
	{
		if (Mesh->GetBoneIndex(Bone) == INDEX_NONE || !Mesh->GetBodyInstance(Bone))
		{
			UE_LOG(LogDelivery, Error, TEXT("%s: Physics body '%s' is missing."), *GetNameSafe(GetOwner()), *Bone.ToString());
			return false;
		}
	}
	return true;
}

void UDeliveryActiveRagdollComponent::PlaceOnGround()
{
	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	if (!World || !Owner || !Capsule)
	{
		return;
	}

	const FVector Start = Owner->GetActorLocation() + FVector::UpVector * Capsule->GetScaledCapsuleHalfHeight();
	const FVector End = Start - FVector::UpVector * 10000.0f;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(RagdollSpawn), false, Owner);
	FHitResult Hit;

	if (World->SweepSingleByChannel(
		Hit, Start, End, FQuat::Identity, ECC_WorldStatic, Capsule->GetCollisionShape(), Params))
	{
		Owner->SetActorLocation(Hit.Location, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void UDeliveryActiveRagdollComponent::ConfigurePhysics()
{
	InitialMeshRelativeTransform = Mesh->GetRelativeTransform();
	Mesh->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);

	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Mesh->SetCollisionObjectType(ECC_PhysicsBody);
	Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	Mesh->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Mesh->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	Mesh->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
	Mesh->SetEnableGravity(true);
	Mesh->SetLinearDamping(RigidBodyLinearDamping);
	Mesh->SetAngularDamping(RigidBodyAngularDamping);
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

	Capsule->SetSimulatePhysics(false);
	Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Capsule->SetEnableGravity(false);

	Mesh->RecreatePhysicsState();
	Mesh->SetAllBodiesSimulatePhysics(true);
	Mesh->SetAllBodiesPhysicsBlendWeight(1.0f, false);
	Mesh->SetSimulatePhysics(true);
	Mesh->WakeAllRigidBodies();
}

bool UDeliveryActiveRagdollComponent::CreateControls()
{
	UPhysicsAsset* PhysicsAsset = Mesh->GetPhysicsAsset();
	if (!PhysicsAsset)
	{
		return false;
	}

	TArray<FName> Torso;
	TArray<FName> HeadChildren;
	TArray<FName> UpperLegs;
	TArray<FName> LowerLegs;
	TArray<FName> Arms;

	for (const TObjectPtr<USkeletalBodySetup>& Setup : PhysicsAsset->SkeletalBodySetups)
	{
		if (!Setup || Setup->BoneName == Bones.Hips || Setup->BoneName == Bones.Spine)
		{
			continue;
		}

		const FName Bone = Setup->BoneName;
		if (Bone == Bones.Head || Mesh->BoneIsChildOf(Bone, Bones.Head))
		{
			if (Bone != Bones.Head)
			{
				HeadChildren.Add(Bone);
			}
		}
		else if (Mesh->BoneIsChildOf(Bone, Bones.LeftUpLeg) || Mesh->BoneIsChildOf(Bone, Bones.RightUpLeg)
			|| Bone == Bones.LeftUpLeg || Bone == Bones.RightUpLeg)
		{
			if (Bone == Bones.LeftUpLeg || Bone == Bones.RightUpLeg)
			{
				UpperLegs.Add(Bone);
			}
			else if (Bone != Bones.LeftFoot && Bone != Bones.RightFoot)
			{
				LowerLegs.Add(Bone);
			}
		}
		else if (Mesh->BoneIsChildOf(Bone, Bones.LeftArm) || Mesh->BoneIsChildOf(Bone, Bones.RightArm)
			|| Bone == Bones.LeftArm || Bone == Bones.RightArm)
		{
			Arms.Add(Bone);
		}
		else
		{
			Torso.Add(Bone);
		}
	}

	// 髋：世界空间位置和旋转。这是走路位移的来源，目标由 UpdatePelvisTarget 每帧写入。
	FPhysicsControlData PelvisData;
	PelvisData.LinearStrength = RootLinearStrength;
	PelvisData.LinearDampingRatio = StableMuscleDampingRatio;
	PelvisData.AngularStrength = StabilizedRootAngularStrength;
	PelvisData.AngularDampingRatio = StableMuscleDampingRatio;
	PelvisData.bUseSkeletalAnimation = false;
	PelvisData.bUseAccelerationDriveMode = true;

	const TArray<FName> PelvisControls = PhysicsControl->CreateControlsFromSkeletalMesh(
		Mesh, { Bones.Hips }, EPhysicsControlType::WorldSpace, PelvisData, PelvisSet);
	if (PelvisControls.IsEmpty())
	{
		return false;
	}
	PelvisControl = PelvisControls[0];

	// 胸：父空间旋转，目标来自启动时记下的胸相对髋姿势，走路时再叠加一点左右摆动。
	FPhysicsControlData ChestData;
	ChestData.AngularStrength = LooseWaistFollowStrength;
	ChestData.AngularDampingRatio = StableMuscleDampingRatio;
	ChestData.bUseSkeletalAnimation = false;
	ChestData.bUseAccelerationDriveMode = true;
	ChestData.bOnlyControlChildObject = true;

	const TArray<FName> ChestControls = PhysicsControl->CreateControlsFromSkeletalMesh(
		Mesh, { Bones.Spine }, EPhysicsControlType::ParentSpace, ChestData, ChestSet);
	if (ChestControls.IsEmpty())
	{
		return false;
	}
	ChestControl = ChestControls[0];

	// 躯干其余节：父空间旋转，目标是当前骨骼动画姿势，不是启动时记下的站立姿势。
	PhysicsControl->CreateControlsFromSkeletalMesh(
		Mesh, Torso, EPhysicsControlType::ParentSpace, MakeAngularControl(LooseComedyBodyStrength, StableMuscleDampingRatio), TorsoSet);
	// 头：世界空间旋转，目标由启动时记下的头部姿势随身体转向得到。
	FPhysicsControlData HeadData = MakeAngularControl(UprightHeadStrength, StableHeadDampingRatio);
	HeadData.bUseSkeletalAnimation = false;
	HeadData.bOnlyControlChildObject = true;
	const TArray<FName> HeadControls = PhysicsControl->CreateControlsFromSkeletalMesh(
		Mesh, { Bones.Head }, EPhysicsControlType::WorldSpace, HeadData, HeadSet);
	if (HeadControls.IsEmpty())
	{
		return false;
	}
	HeadControl = HeadControls[0];
	PhysicsControl->CreateControlsFromSkeletalMesh(
		Mesh, HeadChildren, EPhysicsControlType::ParentSpace,
		MakeAngularControl(UprightHeadStrength, StableHeadDampingRatio), HeadSet);
	// 大腿和小腿：父空间旋转，目标是当前骨骼动画姿势。腿会跟着弯，但迈步位移由脚的世界空间目标决定。
	PhysicsControl->CreateControlsFromSkeletalMesh(
		Mesh, UpperLegs, EPhysicsControlType::ParentSpace, MakeAngularControl(StableHipStrength, 1.3f), LegsSet);
	PhysicsControl->CreateControlsFromSkeletalMesh(
		Mesh, LowerLegs, EPhysicsControlType::ParentSpace, MakeAngularControl(ArticulatedKneeStrength, 1.15f), LegsSet);
	// 脚踝：父空间旋转负责脚尖朝向。世界空间脚控制只拉位置，不写角度，避免两套旋转目标互相拉扯。
	PhysicsControl->CreateControlsFromSkeletalMesh(
		Mesh, { Bones.LeftFoot, Bones.RightFoot }, EPhysicsControlType::ParentSpace,
		MakeAngularControl(FootFacingStrength, StableMuscleDampingRatio), FootPostureSet);
	// 手臂：父空间旋转，目标是当前骨骼动画姿势。强度很低，外力和惯性很容易把胳膊带走。
	PhysicsControl->CreateControlsFromSkeletalMesh(
		Mesh, Arms, EPhysicsControlType::ParentSpace, MakeAngularControl(ComedyArmStrength, 1.0f), ArmsSet);

	// 摆动脚的世界空间位置电机。支撑阶段关掉，避免和地面摩擦较劲。
	FPhysicsControlData FootData;
	FootData.LinearStrength = LegPullStrength;
	FootData.LinearDampingRatio = StableMuscleDampingRatio;
	FootData.AngularStrength = 0.0f;
	FootData.bUseSkeletalAnimation = false;
	FootData.bUseAccelerationDriveMode = true;
	FootData.bOnlyControlChildObject = true;

	const TArray<FName> LeftControls = PhysicsControl->CreateControlsFromSkeletalMesh(
		Mesh, { Bones.LeftFoot }, EPhysicsControlType::WorldSpace, FootData, FeetSet);
	const TArray<FName> RightControls = PhysicsControl->CreateControlsFromSkeletalMesh(
		Mesh, { Bones.RightFoot }, EPhysicsControlType::WorldSpace, FootData, FeetSet);
	if (LeftControls.IsEmpty() || RightControls.IsEmpty())
	{
		return false;
	}

	LeftFoot.Bone = Bones.LeftFoot;
	LeftFoot.Control = LeftControls[0];
	RightFoot.Bone = Bones.RightFoot;
	RightFoot.Control = RightControls[0];
	PhysicsControl->SetControlTargetPositionAndOrientation(
		LeftFoot.Control, LeftFoot.Target, LeftFoot.TargetRotation.Rotator(),
		0.0f, true, true, true, false);
	PhysicsControl->SetControlTargetPositionAndOrientation(
		RightFoot.Control, RightFoot.Target, RightFoot.TargetRotation.Rotator(),
		0.0f, true, true, true, false);
	const bool bEnableFeet = StartupPlantRemaining > 0.0f;
	PhysicsControl->SetControlEnabled(LeftFoot.Control, bEnableFeet, true, false);
	PhysicsControl->SetControlEnabled(RightFoot.Control, bEnableFeet, true, false);
	return true;
}

void UDeliveryActiveRagdollComponent::DestroyControls()
{
	if (PhysicsControl)
	{
		PhysicsControl->DestroyControlsInSet(TEXT("All"));
	}
	PelvisControl = NAME_None;
	ChestControl = NAME_None;
	HeadControl = NAME_None;
	LeftFoot.Control = NAME_None;
	RightFoot.Control = NAME_None;
}

void UDeliveryActiveRagdollComponent::CacheStandingState()
{
	// 记下启动瞬间的站立姿势，供髋、胸、头和脚尖朝向当作参考，而不是每帧重新当成动画目标。
	const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
	ReferencePelvisRotation = Mesh->GetBoneQuaternion(Bones.Hips, EBoneSpaces::WorldSpace);
	const FQuat ReferenceSpineWorld = Mesh->GetBoneQuaternion(Bones.Spine, EBoneSpaces::WorldSpace);
	ReferenceSpineRelativeRotation = ReferencePelvisRotation.Inverse() * ReferenceSpineWorld;
	ReferenceHeadRotation = Mesh->GetBoneQuaternion(Bones.Head, EBoneSpaces::WorldSpace);
	ReferenceFacingYaw = GetAimYaw();
	CurrentFacingYaw = ReferenceFacingYaw;
	UprightInPelvisSpace = ReferencePelvisRotation.UnrotateVector(FVector::UpVector).GetSafeNormal();

	LeftFoot.Target = Mesh->GetCenterOfMass(Bones.LeftFoot);
	RightFoot.Target = Mesh->GetCenterOfMass(Bones.RightFoot);
	LeftFoot.ReferenceRotation = Mesh->GetBoneQuaternion(Bones.LeftFoot, EBoneSpaces::WorldSpace);
	RightFoot.ReferenceRotation = Mesh->GetBoneQuaternion(Bones.RightFoot, EBoneSpaces::WorldSpace);
	LeftFoot.TargetRotation = LeftFoot.ReferenceRotation;
	RightFoot.TargetRotation = RightFoot.ReferenceRotation;

	FVector LeftGround;
	FVector RightGround;
	float LeftGroundZ = LeftFoot.Target.Z;
	float RightGroundZ = RightFoot.Target.Z;
	if (TraceGround(LeftFoot.Target, LeftGround))
	{
		LeftFoot.GroundOffset = FMath::Max(0.0f, LeftFoot.Target.Z - LeftGround.Z);
		LeftGroundZ = LeftGround.Z;
	}
	if (TraceGround(RightFoot.Target, RightGround))
	{
		RightFoot.GroundOffset = FMath::Max(0.0f, RightFoot.Target.Z - RightGround.Z);
		RightGroundZ = RightGround.Z;
	}

	LeftFoot.Start = LeftFoot.Target;
	RightFoot.Start = RightFoot.Target;
	LeftFoot.Alpha = 1.0f;
	RightFoot.Alpha = 1.0f;
	StandHeight = FMath::Clamp(Hips.Z - 0.5f * (LeftGroundZ + RightGroundZ), 80.0f, 140.0f);
	LastWishDirection = GetOwner() ? GetOwner()->GetActorForwardVector() : FVector::ForwardVector;
	SmoothedBalanceOffset = FVector::ZeroVector;
	SmoothedMoveLead = FVector::ZeroVector;
	PlannedPelvisTarget = Hips;
	SmoothedAccelerationAlpha = 0.0f;
	StartupPlantRemaining = StartupFootPlantDuration;
	bWasMoving = false;
	bPendingStopRecovery = false;
	const FVector Right = FVector::CrossProduct(FVector::UpVector, LastWishDirection).GetSafeNormal();
	LeftFoot.SideSign = FVector::DotProduct(LeftFoot.Target - Hips, Right) < 0.0f ? -1.0f : 1.0f;
	RightFoot.SideSign = -LeftFoot.SideSign;
}

void UDeliveryActiveRagdollComponent::StartRagdoll()
{
	AActor* Owner = GetOwner();
	if (Owner)
	{
		if (Owner->HasAuthority())
		{
			ReplicatedControlMode = EDeliveryRagdollControlMode::Active;
			Owner->ForceNetUpdate();
		}
	}

	if (bIsActive)
	{
		bIsLimp = false;
		if (PhysicsControl)
		{
			const bool bRemoteProxy = Owner && Owner->GetLocalRole() == ROLE_SimulatedProxy;
			PhysicsControl->SetControlsInSetEnabled(TEXT("All"), !bRemoteProxy);
			PhysicsControl->SetControlEnabled(LeftFoot.Control, false, true, false);
			PhysicsControl->SetControlEnabled(RightFoot.Control, false, true, false);
		}
		return;
	}

	ResolveOwnerComponents();
	if (!ValidateSetup())
	{
		return;
	}

	if (bPlaceOnGroundAtStart)
	{
		PlaceOnGround();
	}

	ConfigurePhysics();
	CacheStandingState();
	if (!CreateControls())
	{
		UE_LOG(LogDelivery, Error, TEXT("%s: Failed to create Physics Control drives."), *GetNameSafe(GetOwner()));
		StopRagdoll();
		return;
	}

	bIsActive = true;
	bIsLimp = false;
	SetComponentTickEnabled(true);
	PostPhysicsTickFunction.SetTickFunctionEnable(true);
	UpdateControlTargets(0.0f);
	if (Owner && Owner->GetLocalRole() == ROLE_SimulatedProxy)
	{
		// 远端身体由服务端快照驱动，不再运行本地肌肉目标。
		PhysicsControl->SetControlsInSetEnabled(TEXT("All"), false);
	}

	UE_LOG(LogDelivery, Log, TEXT("%s: Active ragdoll started with %d bodies."),
		*GetNameSafe(GetOwner()), Mesh->GetPhysicsAsset()->SkeletalBodySetups.Num());
}

void UDeliveryActiveRagdollComponent::StopRagdoll()
{
	AActor* Owner = GetOwner();
	if (Owner)
	{
		if (Owner->HasAuthority())
		{
			ReplicatedControlMode = EDeliveryRagdollControlMode::Disabled;
			Owner->ForceNetUpdate();
		}
	}

	DestroyControls();
	bIsActive = false;
	bIsLimp = false;
	MoveInput = FVector2D::ZeroVector;
	StartupPlantRemaining = 0.0f;
	bWasMoving = false;
	bPendingStopRecovery = false;
	SnapshotAccumulator = 0.0f;
	bHasNetworkSnapshot = false;
	PreviousSnapshot = FDeliveryRagdollSnapshot();
	TargetSnapshot = FDeliveryRagdollSnapshot();
	SetComponentTickEnabled(false);
	PostPhysicsTickFunction.SetTickFunctionEnable(false);

	if (!Mesh || !Capsule)
	{
		return;
	}

	Mesh->SetSimulatePhysics(false);
	Mesh->SetAllBodiesSimulatePhysics(false);
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Mesh->SetEnableGravity(false);
	Mesh->AttachToComponent(Capsule, FAttachmentTransformRules::KeepWorldTransform);
	Mesh->SetRelativeTransform(InitialMeshRelativeTransform);
	Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
}

void UDeliveryActiveRagdollComponent::SetLimp(bool bLimp)
{
	AActor* Owner = GetOwner();
	const EDeliveryRagdollControlMode NewMode = bLimp
		? EDeliveryRagdollControlMode::Limp
		: EDeliveryRagdollControlMode::Active;
	if (Owner)
	{
		if (Owner->HasAuthority())
		{
			ReplicatedControlMode = NewMode;
			Owner->ForceNetUpdate();
		}
	}

	if (!bIsActive && !bLimp)
	{
		StartRagdoll();
		return;
	}

	bIsLimp = bLimp;
	if (bLimp)
	{
		MoveInput = FVector2D::ZeroVector;
	}
	if (PhysicsControl && bIsActive)
	{
		PhysicsControl->SetControlsInSetEnabled(TEXT("All"), !bLimp);
		if (!bLimp)
		{
			PhysicsControl->SetControlEnabled(LeftFoot.Control, false, true, false);
			PhysicsControl->SetControlEnabled(RightFoot.Control, false, true, false);
			if (Owner && Owner->GetLocalRole() == ROLE_SimulatedProxy)
			{
				PhysicsControl->SetControlsInSetEnabled(TEXT("All"), false);
			}
		}
	}
}

void UDeliveryActiveRagdollComponent::SetMoveInput(FVector2D RightForward)
{
	MoveInput.X = FMath::Clamp(RightForward.X, -1.0f, 1.0f);
	MoveInput.Y = FMath::Clamp(RightForward.Y, -1.0f, 1.0f);
	if (GetOwner() && GetOwner()->HasAuthority() && GetWorld())
	{
		LastMoveInputTime = GetWorld()->GetTimeSeconds();
	}
}

void UDeliveryActiveRagdollComponent::AddImpulse(FVector Impulse, bool bVelocityChange)
{
	if (bIsActive && Mesh)
	{
		Mesh->AddImpulse(Impulse, Bones.Hips, bVelocityChange);
	}
}

FVector UDeliveryActiveRagdollComponent::GetWishDir() const
{
	if (MoveInput.IsNearlyZero())
	{
		return FVector::ZeroVector;
	}

	// 用镜头水平朝向把 WASD 变成世界方向，最后丢掉 Z，所以前进意图始终贴在水平面上。
	const FRotator YawRotation(0.0f, GetAimYaw(), 0.0f);
	return (YawRotation.Vector() * MoveInput.Y
		+ FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y) * MoveInput.X).GetSafeNormal2D();
}

float UDeliveryActiveRagdollComponent::GetAimYaw() const
{
	if (const APawn* Pawn = Cast<APawn>(GetOwner()))
	{
		if (const AController* Controller = Pawn->GetController())
		{
			return Controller->GetControlRotation().Yaw;
		}
	}
	return Capsule ? Capsule->GetComponentRotation().Yaw : 0.0f;
}

void UDeliveryActiveRagdollComponent::UpdateControlTargets(float DeltaTime)
{
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
	const FVector EffectiveWish = StartupPlantRemaining > 0.0f ? FVector::ZeroVector : Wish;
	// 先写出髋部目标 PlannedPelvisTarget，脚的落点再从同一个点推。
	UpdatePelvisTarget(DeltaTime, EffectiveWish);
	UpdateFeet(DeltaTime, EffectiveWish);
}

void UDeliveryActiveRagdollComponent::UpdatePelvisTarget(float DeltaTime, const FVector& Wish)
{
	// 顺序：先算水平目标，再在该点探测地面得到高度，最后把完整目标交给髋部电机。
	const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
	const FVector Velocity = Mesh->GetPhysicsLinearVelocity(Bones.Hips);
	const FVector DesiredVelocity = Wish * DesiredMoveSpeed;
	// 用期望水平速度和当前水平速度的差，估计髋部接下来要去的水平位置。这不是步长，只是让电机有一个略微领前的目标。
	const FVector RawLead = ((DesiredVelocity - FVector(Velocity.X, Velocity.Y, 0.0f)) * TargetLeadTime)
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

	FVector Target = Hips + FVector(SmoothedMoveLead.X, SmoothedMoveLead.Y, 0.0f);

	FVector DesiredBalanceOffset = FVector::ZeroVector;
	if (Wish.IsNearlyZero())
	{
		const FVector SupportCenter = 0.5f * (
			Mesh->GetCenterOfMass(Bones.LeftFoot) + Mesh->GetCenterOfMass(Bones.RightFoot));
		const FVector BodyCenter = GetWholeBodyCenterOfMass();
		DesiredBalanceOffset = FVector(
			SupportCenter.X - BodyCenter.X,
			SupportCenter.Y - BodyCenter.Y,
			0.0f).GetClampedToMaxSize(MaxCenterOfMassCorrection) * CenterOfMassCorrection;
	}
	SmoothedBalanceOffset = FMath::VInterpTo(
		SmoothedBalanceOffset, DesiredBalanceOffset, DeltaTime, BalanceResponseSpeed);
	Target += SmoothedBalanceOffset;

	// 水平目标已经确定，再在这个位置探测地面高度。探测不到时退回当前髋下方，避免把髋吊到半空。
	FVector Ground;
	if (ResolveGroundHeight(Target, Hips, Ground))
	{
		Target.Z = Ground.Z + StandHeight + SmoothBounceHeight * GaitPulse;
	}

	PlannedPelvisTarget = Target;

	const float DesiredYaw = Wish.IsNearlyZero() ? CurrentFacingYaw : Wish.Rotation().Yaw;
	const float YawError = FMath::FindDeltaAngleDegrees(CurrentFacingYaw, DesiredYaw);
	CurrentFacingYaw = FMath::UnwindDegrees(FMath::FInterpTo(
		CurrentFacingYaw, CurrentFacingYaw + YawError, DeltaTime, TurnResponsiveness));
	if (!Wish.IsNearlyZero())
	{
		LastWishDirection = FRotator(0.0f, CurrentFacingYaw, 0.0f).Vector();
	}
	const FQuat YawDelta(FVector::UpVector, FMath::DegreesToRadians(
		FMath::FindDeltaAngleDegrees(ReferenceFacingYaw, CurrentFacingYaw)));
	FQuat Lean = FQuat::Identity;
	if (!Wish.IsNearlyZero())
	{
		const float ForwardSpeed = FVector::DotProduct(
			FVector(Velocity.X, Velocity.Y, 0.0f), Wish);
		const float RawAccelerationAlpha = FMath::Clamp(
			(DesiredMoveSpeed - ForwardSpeed) / FMath::Max(DesiredMoveSpeed, 1.0f), 0.0f, 1.0f);
		SmoothedAccelerationAlpha = FMath::FInterpTo(
			SmoothedAccelerationAlpha, RawAccelerationAlpha, DeltaTime, DriveTargetSmoothingSpeed);
		const float LeanRadians = FMath::DegreesToRadians(
			AccelerationLeanAngle * SmoothedAccelerationAlpha * FMath::Clamp(MoveInput.Size(), 0.0f, 1.0f));
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Wish).GetSafeNormal();
		const float WobbleRadians = FMath::DegreesToRadians(BouncyPelvisWobbleAngle * Wobble);
		const FVector DesiredUp = (FVector::UpVector
			+ Wish * FMath::Tan(LeanRadians)
			+ Right * FMath::Tan(WobbleRadians)).GetSafeNormal();
		Lean = FQuat::FindBetweenNormals(FVector::UpVector, DesiredUp);
	}
	else
	{
		SmoothedAccelerationAlpha = FMath::FInterpTo(
			SmoothedAccelerationAlpha, 0.0f, DeltaTime, DriveTargetSmoothingSpeed);
	}

	const FQuat PelvisRotation = Lean * YawDelta * ReferencePelvisRotation;
	FQuat SpineRelativeRotation = ReferenceSpineRelativeRotation;
	if (!Wish.IsNearlyZero() && !FMath::IsNearlyZero(Wobble))
	{
		const float SwingRadians = FMath::DegreesToRadians(-LooseTorsoSwingAngle * Wobble);
		const float TwistRadians = FMath::DegreesToRadians(LooseTorsoSwingAngle * 0.35f * Wobble);
		const FQuat SpineWorldRotation =
			FQuat(FVector::UpVector, TwistRadians)
			* FQuat(Wish, SwingRadians)
			* PelvisRotation
			* ReferenceSpineRelativeRotation;
		SpineRelativeRotation = PelvisRotation.Inverse() * SpineWorldRotation;
		SpineRelativeRotation.Normalize();
	}

	PhysicsControl->SetControlTargetPositionAndOrientation(
		PelvisControl, Target, PelvisRotation.Rotator(), DeltaTime, true, true, true, false);
	PhysicsControl->SetControlTargetPositionAndOrientation(
		ChestControl, FVector::ZeroVector, SpineRelativeRotation.Rotator(), DeltaTime, true, false, true, false);

	const float StandingAlpha = 1.0f - FMath::Clamp(MoveInput.Size(), 0.0f, 1.0f);
	const FVector Facing = FRotator(0.0f, CurrentFacingYaw, 0.0f).Vector();
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Facing).GetSafeNormal();
	const FQuat BodyRotationDelta = PelvisRotation * ReferencePelvisRotation.Inverse();
	const FQuat HeadCorrection(
		Right, FMath::DegreesToRadians(StandingHeadCorrectionAngle * StandingAlpha));
	const FQuat HeadRotation = HeadCorrection * BodyRotationDelta * ReferenceHeadRotation;
	PhysicsControl->SetControlTargetPositionAndOrientation(
		HeadControl, FVector::ZeroVector, HeadRotation.Rotator(), DeltaTime, true, false, true, false);
}

void UDeliveryActiveRagdollComponent::UpdateFeet(float DeltaTime, const FVector& Wish)
{
	// 先推进正在摆动的脚。若可以开新步，落点从 PlannedPelvisTarget 计算，而不是从当前髋骨骼另算。
	UpdateFootTarget(LeftFoot, DeltaTime);
	UpdateFootTarget(RightFoot, DeltaTime);

	const bool bMoving = !Wish.IsNearlyZero();
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

	// 身体相对世界铅垂线倾得太厉害，或者已经有一只脚在空中，就不要再开新步。
	if (GetUprightDot() < MinimumStepUprightDot
		|| LeftFoot.Alpha < 1.0f || RightFoot.Alpha < 1.0f)
	{
		return;
	}

	FVector Facing = Wish.IsNearlyZero()
		? LastWishDirection
		: FRotator(0.0f, CurrentFacingYaw, 0.0f).Vector();
	Facing = Facing.GetSafeNormal2D();
	if (Facing.IsNearlyZero())
	{
		Facing = GetOwner() ? GetOwner()->GetActorForwardVector() : FVector::ForwardVector;
	}
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Facing).GetSafeNormal();
	// 哪只脚在后面、有没有交叉，看的是当前身体姿态；落点本身用 PlannedPelvisTarget。
	const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
	const float LeftSide = FVector::DotProduct(Mesh->GetCenterOfMass(Bones.LeftFoot) - Hips, Right);
	const float RightSide = FVector::DotProduct(Mesh->GetCenterOfMass(Bones.RightFoot) - Hips, Right);

	if (Wish.IsNearlyZero())
	{
		if (!bPendingStopRecovery)
		{
			return;
		}

		const FVector LeftStand = PlannedPelvisTarget + Right * (LeftFoot.SideSign * StableComedyStance);
		const FVector RightStand = PlannedPelvisTarget + Right * (RightFoot.SideSign * StableComedyStance);
		const float LeftError = FVector::Dist2D(
			Mesh->GetCenterOfMass(Bones.LeftFoot), LeftStand);
		const float RightError = FVector::Dist2D(
			Mesh->GetCenterOfMass(Bones.RightFoot), RightStand);

		// 松开移动后最多收脚一次，把脚收到髋部目标两侧的站宽上，避免站着不停倒脚。
		if (FMath::Max(LeftError, RightError) > StopRecoveryDistance)
		{
			const bool bRecoverLeft = LeftError > RightError;
			if (BeginStep(bRecoverLeft ? LeftFoot : RightFoot, Facing))
			{
				bStepLeftNext = !bRecoverLeft;
			}
		}
		bPendingStopRecovery = false;
		return;
	}

	if (LeftSide * LeftFoot.SideSign < -MovingCrossingRecoveryMargin)
	{
		if (BeginStep(LeftFoot, Facing))
		{
			bStepLeftNext = false;
		}
		return;
	}
	if (RightSide * RightFoot.SideSign < -MovingCrossingRecoveryMargin)
	{
		if (BeginStep(RightFoot, Facing))
		{
			bStepLeftNext = true;
		}
		return;
	}

	const float LeftForward = FVector::DotProduct(
		Mesh->GetCenterOfMass(Bones.LeftFoot) - Hips, Facing);
	const float RightForward = FVector::DotProduct(
		Mesh->GetCenterOfMass(Bones.RightFoot) - Hips, Facing);
	const bool bStepLeft = FMath::Abs(LeftForward - RightForward) > MovingCrossingRecoveryMargin
		? LeftForward < RightForward
		: bStepLeftNext;
	const bool bStarted = BeginStep(bStepLeft ? LeftFoot : RightFoot, Facing);
	if (bStarted)
	{
		bStepLeftNext = !bStepLeft;
	}
}

bool UDeliveryActiveRagdollComponent::BeginStep(FFoot& Foot, const FVector& Wish)
{
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Wish).GetSafeNormal();
	const float ForwardDistance = MoveInput.IsNearlyZero() ? 0.0f : ControlledStrideLength;
	const float ForwardSpeed = FVector::DotProduct(
		Mesh->GetPhysicsLinearVelocity(Bones.Hips), Wish);
	const float VelocityLead = MoveInput.IsNearlyZero()
		? 0.0f
		: FMath::Clamp(
			ForwardSpeed * ControlledStrideDuration * 0.35f,
			0.0f, ControlledStrideLength * 0.5f);
	// 落点从本帧髋部目标推出，再在落点探测地面高度。平滑曲线只负责从当前位置走到这个落点，并不重新决定落点。
	FVector Destination = PlannedPelvisTarget + Wish * (ForwardDistance + VelocityLead)
		+ Right * (Foot.SideSign * StableComedyStance);
	FVector Ground;
	if (!TraceGround(Destination, Ground))
	{
		// 落点下方没有地面就不抬这只脚，否则目标会停在髋部高度的半空中。
		return false;
	}
	Destination.Z = Ground.Z + Foot.GroundOffset;

	Foot.Start = Mesh->GetCenterOfMass(Foot.Bone);
	Foot.Target = Destination;
	Foot.SideAxis = Right;
	const float TargetYaw = Wish.Rotation().Yaw;
	const FQuat FacingDelta(FVector::UpVector, FMath::DegreesToRadians(
		FMath::FindDeltaAngleDegrees(ReferenceFacingYaw, TargetYaw)));
	Foot.TargetRotation = FacingDelta * Foot.ReferenceRotation;
	Foot.Alpha = 0.0f;
	PhysicsControl->SetControlEnabled(Foot.Control, true, true, false);
	return true;
}

void UDeliveryActiveRagdollComponent::UpdateFootTarget(FFoot& Foot, float DeltaTime)
{
	if (Foot.Alpha >= 1.0f)
	{
		return;
	}

	Foot.Alpha = FMath::Min(1.0f, Foot.Alpha + DeltaTime / FMath::Max(ControlledStrideDuration, 0.05f));
	// 落点已经定好。这条曲线只描述这一脚怎么从现在的位置走到落点：水平两端慢起慢停，中间按正弦抬起，避免直线拽进地面。
	const float SmoothAlpha = Foot.Alpha * Foot.Alpha * Foot.Alpha
		* (Foot.Alpha * (Foot.Alpha * 6.0f - 15.0f) + 10.0f);
	FVector Position = FMath::Lerp(Foot.Start, Foot.Target, SmoothAlpha);
	Position.Z += FMath::Square(FMath::Sin(PI * Foot.Alpha)) * ControlledStepHeight;

	const float SignedSide = FVector::DotProduct(Position - PlannedPelvisTarget, Foot.SideAxis) * Foot.SideSign;
	const float RequiredSide = StableMinimumFootSide * SmoothAlpha;
	if (SignedSide < RequiredSide)
	{
		Position += Foot.SideAxis * Foot.SideSign * (RequiredSide - SignedSide);
	}

	PhysicsControl->SetControlTargetPositionAndOrientation(
		Foot.Control, Position, Foot.TargetRotation.Rotator(), DeltaTime, true, true, true, false);

	if (Foot.Alpha >= 1.0f)
	{
		// 落地后关掉世界空间位置电机。这只脚改做支撑，靠摩擦和腿部角度电机留在地上。
		PhysicsControl->SetControlEnabled(Foot.Control, false, true, false);
	}
}

FVector UDeliveryActiveRagdollComponent::GetWholeBodyCenterOfMass() const
{
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

bool UDeliveryActiveRagdollComponent::ResolveGroundHeight(
	const FVector& PlannedHorizontal, const FVector& Fallback, FVector& GroundPoint) const
{
	if (TraceGround(PlannedHorizontal, GroundPoint))
	{
		return true;
	}
	return TraceGround(Fallback, GroundPoint);
}

float UDeliveryActiveRagdollComponent::GetUprightDot() const
{
	if (!Mesh)
	{
		return 0.0f;
	}

	// 把启动时髋部里的头顶方向转到现在，再和世界向上做点积。数值越接近 1 越直立。
	const FQuat PelvisRotation = Mesh->GetBoneQuaternion(Bones.Hips, EBoneSpaces::WorldSpace);
	const FVector Up = PelvisRotation.RotateVector(UprightInPelvisSpace).GetSafeNormal();
	return FVector::DotProduct(Up, FVector::UpVector);
}

bool UDeliveryActiveRagdollComponent::TraceGround(const FVector& Around, FVector& GroundPoint) const
{
	// 从探测点上方 60 打到下方 220，只取撞击点，不用地面法线。
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	const FVector Start = Around + FVector::UpVector * 60.0f;
	const FVector End = Around - FVector::UpVector * 220.0f;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(RagdollGround), false, GetOwner());
	FHitResult Hit;

	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Params)
		|| World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
	{
		GroundPoint = Hit.ImpactPoint;
		return true;
	}
	return false;
}

void UDeliveryActiveRagdollComponent::SyncOwnerToPelvis(float DeltaTime)
{
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
	switch (ReplicatedControlMode)
	{
	case EDeliveryRagdollControlMode::Active:
		StartRagdoll();
		break;
	case EDeliveryRagdollControlMode::Limp:
		if (!bIsActive)
		{
			StartRagdoll();
		}
		SetLimp(true);
		break;
	default:
		StopRagdoll();
		break;
	}
}

void UDeliveryActiveRagdollComponent::CaptureNetworkSnapshot()
{
	if (!Mesh || !GetWorld())
	{
		return;
	}

	++ReplicatedSnapshot.Sequence;
	if (const AGameStateBase* GameState = GetWorld()->GetGameState())
	{
		ReplicatedSnapshot.ServerTime = GameState->GetServerWorldTimeSeconds();
	}
	else
	{
		ReplicatedSnapshot.ServerTime = GetWorld()->GetTimeSeconds();
	}

	const FName SnapshotBones[] = {
		Bones.Hips, Bones.Spine, Bones.Head, Bones.LeftFoot, Bones.RightFoot
	};
	ReplicatedSnapshot.Bodies.Reset(UE_ARRAY_COUNT(SnapshotBones));
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
	PreviousSnapshot = bHasNetworkSnapshot ? TargetSnapshot : ReplicatedSnapshot;
	TargetSnapshot = ReplicatedSnapshot;
	SnapshotReceivedAt = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	bHasNetworkSnapshot = true;
}

void UDeliveryActiveRagdollComponent::ApplyNetworkSnapshot(float DeltaTime)
{
	if (!bHasNetworkSnapshot || !Mesh || TargetSnapshot.Bodies.IsEmpty() || !GetWorld())
	{
		return;
	}

	const bool bOwnerPrediction = GetOwner()->GetLocalRole() == ROLE_AutonomousProxy;
	const float SnapshotInterval = 1.0f / FMath::Max(NetworkSnapshotRate, 1.0f);
	const float TimeSinceReceive = GetWorld()->GetTimeSeconds() - SnapshotReceivedAt;
	const float InterpolationAlpha = bOwnerPrediction
		? 1.0f
		: FMath::Clamp(TimeSinceReceive / SnapshotInterval, 0.0f, 1.0f);

	float ExtrapolationTime = FMath::Max(0.0f, TimeSinceReceive - SnapshotInterval);
	if (bOwnerPrediction)
	{
		if (const AGameStateBase* GameState = GetWorld()->GetGameState())
		{
			ExtrapolationTime = FMath::Max(
				0.0f, GameState->GetServerWorldTimeSeconds() - TargetSnapshot.ServerTime);
		}
	}
	ExtrapolationTime = FMath::Min(ExtrapolationTime, MaxSnapshotExtrapolation);

	bool bHardCorrection = false;
	if (bOwnerPrediction)
	{
		if (const FBodyInstance* PelvisBody = Mesh->GetBodyInstance(TargetSnapshot.Bodies[0].Bone))
		{
			const FVector TargetPelvis = FVector(TargetSnapshot.Bodies[0].Position)
				+ FVector(TargetSnapshot.Bodies[0].LinearVelocity) * ExtrapolationTime;
			bHardCorrection = FVector::Dist(
				PelvisBody->GetUnrealWorldTransform().GetLocation(), TargetPelvis)
				> OwnerHardCorrectionDistance;
		}
	}

	const int32 BodyCount = FMath::Min(
		PreviousSnapshot.Bodies.Num(), TargetSnapshot.Bodies.Num());
	const float CorrectionAlpha = bOwnerPrediction && !bHardCorrection
		? 1.0f - FMath::Exp(-OwnerCorrectionSpeed * DeltaTime)
		: 1.0f;

	for (int32 Index = 0; Index < BodyCount; ++Index)
	{
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
