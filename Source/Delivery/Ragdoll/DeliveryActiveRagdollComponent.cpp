// Copyright Epic Games, Inc. All Rights Reserved.

/*
每一帧是：

把水平 Wish 投影到当前坡面的切平面上，再用速度差算出髋接下来要去的位置。
在切平面上定好这一点，沿法线投到坡面上。髋目标等于击中点加上站立高度乘以法线。
如果这条射线没有碰到地面，就回到当前髋的位置，再沿法线探测一次，避免把髋吊到半空。
坡顶接到平面时，法线和贴地点不会立刻换成新值，而是平滑过渡过去，避免髋的位置和旋转在一帧里抽掉。
电机把髋拉向这个完整目标 PlannedPelvisTarget。
脚的落点从同一个 PlannedPelvisTarget 在切平面上推出来，再沿法线投到坡面上。
摆动过程中每一帧按本帧的髋目标重新计算落点，不要在抬脚那一瞬间把落点算死。
脚底板的朝向用切平面上的前进方向和法线来建，让脚底板贴着地面。
选哪只脚迈步，仍然看当前身体姿态。人有没有倾倒，看髋的朝上方向和坡面法线的点积，不用世界竖直向上。

髋先走，脚后追。这套方法只处理比较缓的斜面。陡坡、台阶和用手攀爬都不做。
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
#include "Math/RotationMatrix.h"
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

	FGroundHit LeftGround;
	FGroundHit RightGround;
	FVector LeftGroundPoint = LeftFoot.Target;
	FVector RightGroundPoint = RightFoot.Target;
	FVector LeftNormal = FVector::UpVector;
	FVector RightNormal = FVector::UpVector;
	if (TraceGround(LeftFoot.Target, FVector::UpVector, LeftGround))
	{
		LeftFoot.GroundOffset = FMath::Max(0.0f, FVector::DotProduct(LeftFoot.Target - LeftGround.Point, LeftGround.Normal));
		LeftGroundPoint = LeftGround.Point;
		LeftNormal = LeftGround.Normal;
	}
	if (TraceGround(RightFoot.Target, FVector::UpVector, RightGround))
	{
		RightFoot.GroundOffset = FMath::Max(0.0f, FVector::DotProduct(RightFoot.Target - RightGround.Point, RightGround.Normal));
		RightGroundPoint = RightGround.Point;
		RightNormal = RightGround.Normal;
	}

	LeftFoot.Start = LeftFoot.Target;
	RightFoot.Start = RightFoot.Target;
	LeftFoot.Alpha = 1.0f;
	RightFoot.Alpha = 1.0f;
	const FVector AverageNormal = (LeftNormal + RightNormal).GetSafeNormal();
	CurrentGroundNormal = AverageNormal.IsNearlyZero() ? FVector::UpVector : AverageNormal;
	const FVector MidGround = 0.5f * (LeftGroundPoint + RightGroundPoint);
	SmoothedGroundPoint = MidGround;
	StandHeight = FMath::Clamp(FVector::DotProduct(Hips - MidGround, CurrentGroundNormal), 80.0f, 140.0f);
	LastWishDirection = GetOwner() ? GetOwner()->GetActorForwardVector() : FVector::ForwardVector;
	SmoothedBalanceOffset = FVector::ZeroVector;
	SmoothedMoveLead = FVector::ZeroVector;
	PlannedPelvisTarget = Hips;
	WishOnSlope = FVector::ZeroVector;
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

	// 用镜头水平朝向把 WASD 变成世界方向，最后丢掉 Z。沿坡行走时再投影到切平面。
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
	UpdatePelvisTarget(DeltaTime, EffectiveWish);
	UpdateFeet(DeltaTime, EffectiveWish);
}

void UDeliveryActiveRagdollComponent::UpdatePelvisTarget(float DeltaTime, const FVector& Wish)
{
	const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
	const FVector Velocity = Mesh->GetPhysicsLinearVelocity(Bones.Hips);

	FGroundHit GroundUnderHips;
	bool bHasHipGround = TraceGround(Hips, CurrentGroundNormal, GroundUnderHips);
	if (!bHasHipGround)
	{
		bHasHipGround = TraceGround(Hips, FVector::UpVector, GroundUnderHips);
	}

	// 把水平 Wish 和当前速度都投影到坡面上，用它们的差估计髋要沿坡往前领多远。
	WishOnSlope = FVector::VectorPlaneProject(Wish, CurrentGroundNormal).GetSafeNormal();
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

	FVector DesiredBalanceOffset = FVector::ZeroVector;
	if (Wish.IsNearlyZero())
	{
		const FVector SupportCenter = 0.5f * (
			Mesh->GetCenterOfMass(Bones.LeftFoot) + Mesh->GetCenterOfMass(Bones.RightFoot));
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
	FGroundHit Ground;
	if (SampleGround(Target, Hips, CurrentGroundNormal, Ground))
	{
		DesiredNormal = Ground.Normal;
		DesiredGroundPoint = Ground.Point;
	}
	else if (bHasHipGround)
	{
		DesiredNormal = GroundUnderHips.Normal;
		DesiredGroundPoint = GroundUnderHips.Point;
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
	// 髋目标等于平滑后的贴地点，再加上站立高度沿法线抬起来。
	Target = SmoothedGroundPoint + CurrentGroundNormal * (StandHeight + SmoothBounceHeight * GaitPulse);

	PlannedPelvisTarget = Target;

	const FVector FacingWish = WishOnSlope.IsNearlyZero() ? Wish : WishOnSlope;
	const float DesiredYaw = FacingWish.IsNearlyZero() ? CurrentFacingYaw : FacingWish.GetSafeNormal2D().Rotation().Yaw;
	const float YawError = FMath::FindDeltaAngleDegrees(CurrentFacingYaw, DesiredYaw);
	CurrentFacingYaw = FMath::UnwindDegrees(FMath::FInterpTo(
		CurrentFacingYaw, CurrentFacingYaw + YawError, DeltaTime, TurnResponsiveness));
	if (!Wish.IsNearlyZero())
	{
		LastWishDirection = FRotator(0.0f, CurrentFacingYaw, 0.0f).Vector();
	}
	const FVector StanceUp = FMath::Lerp(FVector::UpVector, CurrentGroundNormal, 0.45f).GetSafeNormal();
	const FQuat SlopeAlign = FQuat::FindBetweenNormals(FVector::UpVector, StanceUp);
	const FQuat YawDelta(CurrentGroundNormal, FMath::DegreesToRadians(
		FMath::FindDeltaAngleDegrees(ReferenceFacingYaw, CurrentFacingYaw)));
	FQuat Lean = FQuat::Identity;
	const FVector SlopeForward = GetSlopeForward(Wish);
	const FVector SlopeRight = GetSlopeRight(Wish);
	if (!Wish.IsNearlyZero())
	{
		const float ForwardSpeed = FVector::DotProduct(VelocityOnSlope, WishOnSlope);
		const float RawAccelerationAlpha = FMath::Clamp(
			(DesiredMoveSpeed - ForwardSpeed) / FMath::Max(DesiredMoveSpeed, 1.0f), 0.0f, 1.0f);
		SmoothedAccelerationAlpha = FMath::FInterpTo(
			SmoothedAccelerationAlpha, RawAccelerationAlpha, DeltaTime, DriveTargetSmoothingSpeed);
		const float LeanRadians = FMath::DegreesToRadians(
			AccelerationLeanAngle * FMath::Clamp(MoveInput.Size(), 0.0f, 1.0f));
		const float WobbleRadians = FMath::DegreesToRadians(BouncyPelvisWobbleAngle * Wobble);
		const FVector DesiredUp = (StanceUp
			+ WishOnSlope * FMath::Tan(LeanRadians)
			+ SlopeRight * FMath::Tan(WobbleRadians)).GetSafeNormal();
		Lean = FQuat::FindBetweenNormals(StanceUp, DesiredUp);
	}
	else
	{
		SmoothedAccelerationAlpha = FMath::FInterpTo(
			SmoothedAccelerationAlpha, 0.0f, DeltaTime, DriveTargetSmoothingSpeed);
	}

	const FQuat PelvisRotation = Lean * YawDelta * SlopeAlign * ReferencePelvisRotation;
	FQuat SpineRelativeRotation = ReferenceSpineRelativeRotation;
	if (!Wish.IsNearlyZero() && !FMath::IsNearlyZero(Wobble) && !SlopeForward.IsNearlyZero())
	{
		const float SwingRadians = FMath::DegreesToRadians(-LooseTorsoSwingAngle * Wobble);
		const float TwistRadians = FMath::DegreesToRadians(LooseTorsoSwingAngle * 0.35f * Wobble);
		const FQuat SpineWorldRotation =
			FQuat(CurrentGroundNormal, TwistRadians)
			* FQuat(SlopeForward, SwingRadians)
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
	const FQuat BodyRotationDelta = PelvisRotation * ReferencePelvisRotation.Inverse();
	const FQuat HeadCorrection(
		SlopeRight, FMath::DegreesToRadians(StandingHeadCorrectionAngle * StandingAlpha));
	const FQuat HeadRotation = HeadCorrection * BodyRotationDelta * ReferenceHeadRotation;
	PhysicsControl->SetControlTargetPositionAndOrientation(
		HeadControl, FVector::ZeroVector, HeadRotation.Rotator(), DeltaTime, true, false, true, false);
}

void UDeliveryActiveRagdollComponent::UpdateFeet(float DeltaTime, const FVector& Wish)
{
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

	if (GetUprightDot() < MinimumStepUprightDot
		|| LeftFoot.Alpha < 1.0f || RightFoot.Alpha < 1.0f)
	{
		return;
	}

	// 哪只脚在后面、有没有交叉到身体另一侧，看现在的身体姿态。落点本身从 PlannedPelvisTarget 推。
	const FVector Forward = GetSlopeForward(Wish);
	const FVector Right = GetSlopeRight(Wish);
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
		const FVector LeftDelta = FVector::VectorPlaneProject(
			Mesh->GetCenterOfMass(Bones.LeftFoot) - LeftStand, CurrentGroundNormal);
		const FVector RightDelta = FVector::VectorPlaneProject(
			Mesh->GetCenterOfMass(Bones.RightFoot) - RightStand, CurrentGroundNormal);
		const float LeftError = LeftDelta.Size();
		const float RightError = RightDelta.Size();

		// 刚松开移动时最多收一次脚，把脚收到髋目标两侧的站宽上，免得站着不停倒脚。
		if (FMath::Max(LeftError, RightError) > StopRecoveryDistance)
		{
			const bool bRecoverLeft = LeftError > RightError;
			if (BeginStep(bRecoverLeft ? LeftFoot : RightFoot, Wish))
			{
				bStepLeftNext = !bRecoverLeft;
			}
		}
		bPendingStopRecovery = false;
		return;
	}

	if (LeftSide * LeftFoot.SideSign < -MovingCrossingRecoveryMargin)
	{
		if (BeginStep(LeftFoot, Wish))
		{
			bStepLeftNext = false;
		}
		return;
	}
	if (RightSide * RightFoot.SideSign < -MovingCrossingRecoveryMargin)
	{
		if (BeginStep(RightFoot, Wish))
		{
			bStepLeftNext = true;
		}
		return;
	}

	const float LeftForward = FVector::DotProduct(
		Mesh->GetCenterOfMass(Bones.LeftFoot) - Hips, Forward);
	const float RightForward = FVector::DotProduct(
		Mesh->GetCenterOfMass(Bones.RightFoot) - Hips, Forward);
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
	PlanFootLanding(Foot, WishOnSlope.IsNearlyZero() ? GetWishDir() : WishOnSlope);

	// 落点已经在上面重算过。这条曲线只描述这一脚怎么从现在的位置走到落点：两端慢起慢停，中间沿法线抬起。
	const float SmoothAlpha = Foot.Alpha * Foot.Alpha * Foot.Alpha
		* (Foot.Alpha * (Foot.Alpha * 6.0f - 15.0f) + 10.0f);
	FVector Position = FMath::Lerp(Foot.Start, Foot.Target, SmoothAlpha);
	Position += CurrentGroundNormal * (FMath::Square(FMath::Sin(PI * Foot.Alpha)) * ControlledStepHeight);

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
		// 脚已经落到目标上。关掉位置电机，这只脚改做支撑，靠摩擦和腿部角度电机留在地上。
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

	const float ForwardDistance = MoveInput.IsNearlyZero() ? 0.0f : ControlledStrideLength;
	const float ForwardSpeed = FVector::DotProduct(
		FVector::VectorPlaneProject(Mesh->GetPhysicsLinearVelocity(Bones.Hips), CurrentGroundNormal),
		Forward);
	const float VelocityLead = MoveInput.IsNearlyZero()
		? 0.0f
		: FMath::Clamp(
			ForwardSpeed * ControlledStrideDuration * 0.35f,
			0.0f, ControlledStrideLength * 0.5f);
	const FVector DestinationOnPlane = PlannedPelvisTarget
		+ Forward * (ForwardDistance + VelocityLead)
		+ Right * (Foot.SideSign * StableComedyStance);

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

FVector UDeliveryActiveRagdollComponent::GetSlopeRight(const FVector& Wish) const
{
	return FVector::CrossProduct(CurrentGroundNormal, GetSlopeForward(Wish)).GetSafeNormal();
}

FQuat UDeliveryActiveRagdollComponent::MakeSlopeAlignedFootRotation(
	const FFoot& Foot, const FVector& SlopeForward) const
{
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

bool UDeliveryActiveRagdollComponent::SampleGround(
	const FVector& Planned, const FVector& Fallback, const FVector& AlongNormal, FGroundHit& OutHit) const
{
	if (TraceGround(Planned, AlongNormal, OutHit))
	{
		return true;
	}
	return TraceGround(Fallback, AlongNormal, OutHit);
}

float UDeliveryActiveRagdollComponent::GetUprightDot() const
{
	if (!Mesh)
	{
		return 0.0f;
	}

	// 把启动时髋里的头顶方向转到现在，再和坡面法线做点积。不要和世界竖直向上比。
	const FQuat PelvisRotation = Mesh->GetBoneQuaternion(Bones.Hips, EBoneSpaces::WorldSpace);
	const FVector Up = PelvisRotation.RotateVector(UprightInPelvisSpace).GetSafeNormal();
	return FVector::DotProduct(Up, CurrentGroundNormal);
}

bool UDeliveryActiveRagdollComponent::TraceGround(
	const FVector& Around, const FVector& AlongNormal, FGroundHit& OutHit) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FVector Normal = AlongNormal.GetSafeNormal();
	if (Normal.IsNearlyZero() || Normal.Z < 0.0f)
	{
		Normal = FVector::UpVector;
	}

	const FVector Start = Around + Normal * 60.0f;
	const FVector End = Around - Normal * 220.0f;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(RagdollGround), false, GetOwner());
	FHitResult Hit;

	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Params)
		|| World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
	{
		FVector HitNormal = Hit.ImpactNormal.GetSafeNormal();
		if (HitNormal.Z < 0.0f)
		{
			HitNormal = -HitNormal;
		}
		const float MinimumWalkableNormalZ = FMath::Cos(FMath::DegreesToRadians(MaxWalkableSlopeDegrees));
		if (HitNormal.Z < MinimumWalkableNormalZ)
		{
			// 比 MaxWalkableSlopeDegrees 更陡的面不按坡面走，例如立面和台阶踢面。
			return false;
		}

		OutHit.Point = Hit.ImpactPoint;
		OutHit.Normal = HitNormal;
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
