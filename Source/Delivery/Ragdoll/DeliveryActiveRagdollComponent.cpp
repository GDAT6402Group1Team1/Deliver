// Copyright Epic Games, Inc. All Rights Reserved.

/*
每一帧是：

把水平 Wish 投影到当前坡面的切平面上，再用速度差算出髋接下来要去的位置。
在切平面上定好这一点，沿法线投到坡面上。髋目标等于击中点加上站立高度乘以法线。
如果这条射线没有碰到地面，就回到当前髋的位置，再沿法线探测一次，避免把髋吊到半空。
坡顶接到平面时，法线和贴地点不会立刻换成新值，而是平滑过渡过去，避免髋的位置和旋转在一帧里抽掉。
电机把髋拉向这个完整目标 PlannedPelvisTarget。
脚的落点从同一个 PlannedPelvisTarget 在切平面上推出来，再沿法线投到坡面上。
摆动前 85% 按本帧髋目标重新计算落点，末段固定落点并留出有上限的物理到位时间。
脚底板的朝向用切平面上的前进方向和法线来建，让脚底板贴着地面。
选哪只脚迈步，仍然看当前身体姿态。人有没有倾倒，看髋的朝上方向和坡面法线的点积，不用世界竖直向上。

髋先走，脚后追。这套方法只处理比较缓的斜面。陡坡、台阶和用手攀爬都不做。
*/

#include "DeliveryActiveRagdollComponent.h"
#include "Ragdoll/DeliveryFootPlacement.h"

#include "CollisionQueryParams.h"
#include "Combat/DeliveryHandPose.h"
#include "DeliveryCharacter.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Controller.h"
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

	bool HasPhysicsBody(const USkeletalMeshComponent* Mesh, const FName Bone)
	{
		return Mesh
			&& Bone != NAME_None
			&& Mesh->GetBoneIndex(Bone) != INDEX_NONE
			&& Mesh->GetBodyInstance(Bone) != nullptr;
	}

	int32 BoneDepth(const USkeletalMeshComponent* Mesh, FName Bone)
	{
		int32 Depth = 0;
		while (Bone != NAME_None)
		{
			++Depth;
			Bone = Mesh->GetParentBone(Bone);
		}
		return Depth;
	}

	/** 从某节往末梢找，用该枝上最远端、且真有刚体的那一节。没有脚刚体时就是小腿。 */
	FName FindMostDistalPhysicsBody(const USkeletalMeshComponent* Mesh, const FName Root)
	{
		if (!Mesh || Root == NAME_None || Mesh->GetBoneIndex(Root) == INDEX_NONE)
		{
			return NAME_None;
		}

		FName Best = NAME_None;
		int32 BestDepth = -1;
		const int32 NumBones = Mesh->GetNumBones();
		for (int32 Index = 0; Index < NumBones; ++Index)
		{
			const FName Bone = Mesh->GetBoneName(Index);
			if (Bone != Root && !Mesh->BoneIsChildOf(Bone, Root))
			{
				continue;
			}
			if (!HasPhysicsBody(Mesh, Bone))
			{
				continue;
			}

			const int32 Depth = BoneDepth(Mesh, Bone);
			if (Depth >= BestDepth)
			{
				Best = Bone;
				BestDepth = Depth;
			}
		}
		return Best;
	}

	FName WalkToAncestorPhysicsBody(const USkeletalMeshComponent* Mesh, FName Bone, const FName StopAt)
	{
		if (!Mesh)
		{
			return NAME_None;
		}

		FName Current = Mesh->GetBoneIndex(Bone) != INDEX_NONE ? Mesh->GetParentBone(Bone) : StopAt;
		while (Current != NAME_None)
		{
			if (HasPhysicsBody(Mesh, Current))
			{
				return Current;
			}
			if (Current == StopAt)
			{
				break;
			}
			Current = Mesh->GetParentBone(Current);
		}
		return NAME_None;
	}

	/** 在一条腿上取组件空间最低的骨骼当脚底，通常是脚尖。 */
	FName FindVisualSole(const USkeletalMeshComponent* Mesh, const FName LimbRoot, const FName Fallback)
	{
		if (!Mesh || LimbRoot == NAME_None || Mesh->GetBoneIndex(LimbRoot) == INDEX_NONE)
		{
			return Fallback;
		}

		FName Best = Fallback;
		float BestZ = MAX_flt;
		const int32 NumBones = Mesh->GetNumBones();
		for (int32 Index = 0; Index < NumBones; ++Index)
		{
			const FName Bone = Mesh->GetBoneName(Index);
			if (Bone != LimbRoot && !Mesh->BoneIsChildOf(Bone, LimbRoot))
			{
				continue;
			}

			const float Z = Mesh->GetBoneLocation(Bone, EBoneSpaces::ComponentSpace).Z;
			if (Z < BestZ)
			{
				Best = Bone;
				BestZ = Z;
			}
		}
		return Best;
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
	DOREPLIFETIME_CONDITION(UDeliveryActiveRagdollComponent, CurrentFacingYaw, COND_SimulatedOnly);
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
	if (GetOwner()->HasAuthority() && HitReactionEndTime > 0.0f && GetWorld()
		&& GetWorld()->GetTimeSeconds() >= HitReactionEndTime)
	{
		HitReactionEndTime = 0.0f;
		SetHitReactionStrength(1.0f);
		SetHitFeetPlanted(false);
		bHasHitFacing = false;
		HitPushDirection = FVector::ZeroVector;
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

void UDeliveryActiveRagdollComponent::ResolveConfiguredPhysicsBones()
{
	if (!Mesh)
	{
		return;
	}

	const auto Remap = [this](FName& Bone, const FName Resolved)
	{
		if (Resolved == NAME_None || Resolved == Bone)
		{
			return;
		}

		UE_LOG(LogDelivery, Warning,
			TEXT("%s: '%s' has no physics body, using '%s'."),
			*GetNameSafe(GetOwner()), *Bone.ToString(), *Resolved.ToString());
		Bone = Resolved;
	};

	if (!HasPhysicsBody(Mesh, Bones.Spine))
	{
		FName Chest = WalkToAncestorPhysicsBody(Mesh, Bones.Spine, Bones.Hips);
		if (Chest == NAME_None)
		{
			Chest = HasPhysicsBody(Mesh, TEXT("Spine")) ? FName(TEXT("Spine")) : FName(TEXT("Spine1"));
			if (!HasPhysicsBody(Mesh, Chest))
			{
				Chest = NAME_None;
			}
		}
		Remap(Bones.Spine, Chest);
	}

	// 迈步电机必须挂在有刚体的那一节；脚底高度另用脚尖骨骼量，不要把 Bones.LeftFoot 改成小腿。
	LeftFoot.Bone = HasPhysicsBody(Mesh, Bones.LeftFoot)
		? Bones.LeftFoot
		: FindMostDistalPhysicsBody(Mesh, Bones.LeftUpLeg);
	RightFoot.Bone = HasPhysicsBody(Mesh, Bones.RightFoot)
		? Bones.RightFoot
		: FindMostDistalPhysicsBody(Mesh, Bones.RightUpLeg);
	LeftFoot.SoleBone = FindVisualSole(Mesh, Bones.LeftUpLeg, LeftFoot.Bone);
	RightFoot.SoleBone = FindVisualSole(Mesh, Bones.RightUpLeg, RightFoot.Bone);
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
		Bones.Hips, Bones.Spine, Bones.Head, Bones.LeftUpLeg, LeftFoot.Bone,
		Bones.RightUpLeg, RightFoot.Bone, Bones.LeftArm, Bones.RightArm
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

	// 胶囊半高和网格相对偏移对不齐时，脚会先悬空。再用脚底骨骼把整个人往地面收。
	if (!Mesh)
	{
		return;
	}

	const FName LeftSole = LeftFoot.SoleBone != NAME_None ? LeftFoot.SoleBone : Bones.LeftFoot;
	const FName RightSole = RightFoot.SoleBone != NAME_None ? RightFoot.SoleBone : Bones.RightFoot;
	if (Mesh->GetBoneIndex(LeftSole) == INDEX_NONE || Mesh->GetBoneIndex(RightSole) == INDEX_NONE)
	{
		return;
	}

	FGroundHit SoleGround;
	const FVector MidSole = 0.5f * (
		Mesh->GetBoneLocation(LeftSole, EBoneSpaces::WorldSpace)
		+ Mesh->GetBoneLocation(RightSole, EBoneSpaces::WorldSpace));
	if (TraceGround(MidSole, FVector::UpVector, SoleGround))
	{
		const float Gap = FVector::DotProduct(MidSole - SoleGround.Point, SoleGround.Normal);
		if (FMath::Abs(Gap) > 0.5f)
		{
			Owner->SetActorLocation(
				Owner->GetActorLocation() - SoleGround.Normal * Gap,
				false, nullptr, ETeleportType::TeleportPhysics);
		}
	}
}

void UDeliveryActiveRagdollComponent::ConfigurePhysics()
{
	FDeliveryBoxingPose::CompletePhysicsAsset(Mesh, ArmPose);
	InitialMeshRelativeTransform = Mesh->GetRelativeTransform();
	Mesh->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);

	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Mesh->SetCollisionObjectType(ECC_PhysicsBody);
	Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	Mesh->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Mesh->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	Mesh->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
	// 镜头射线要能选中晕倒角色用于抓取；Visibility 只影响查询，不增加物理碰撞。
	Mesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	Mesh->SetEnableGravity(true);
	Mesh->SetLinearDamping(RigidBodyLinearDamping);
	Mesh->SetAngularDamping(RigidBodyAngularDamping);
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

	// 手指没有刚体，物理姿势管不到，它们只跟动画姿势走。不接管的话就是参考姿势的五指张开，
	// 出拳打出去的是巴掌而不是拳头。这里换成只负责握拳的动画实例，有刚体的骨骼照旧由物理覆盖。
	Mesh->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	Mesh->SetAnimInstanceClass(UDeliveryHandPoseAnimInstance::StaticClass());
	if (UDeliveryHandPoseAnimInstance* Hands = Cast<UDeliveryHandPoseAnimInstance>(Mesh->GetAnimInstance()))
	{
		Hands->FingerCurlAngle = ArmPose.FingerCurlAngle;
		Hands->ThumbCurlAngle = ArmPose.ThumbCurlAngle;
		Hands->RebuildFist();
	}

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
	TArray<FName> LeftArmBones;
	TArray<FName> RightArmBones;
	BoxingPose.Create(Mesh, PhysicsControl, ArmPose);

	for (const TObjectPtr<USkeletalBodySetup>& Setup : PhysicsAsset->SkeletalBodySetups)
	{
		if (!Setup || Setup->BoneName == Bones.Hips || Setup->BoneName == Bones.Spine)
		{
			continue;
		}

		const FName Bone = Setup->BoneName;
		// Each arm body has exactly one controller. Never also add an upper arm to Torso.
		const int32 BoxingSide = (Bone == Bones.LeftArm || Mesh->BoneIsChildOf(Bone, Bones.LeftArm)) ? 0
			: ((Bone == Bones.RightArm || Mesh->BoneIsChildOf(Bone, Bones.RightArm)) ? 1 : -1);
		if (BoxingSide >= 0 && BoxingPose.IsReady(BoxingSide)) continue;
		if (Bone == Bones.LeftArm || Bone == Bones.RightArm) continue;
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
			else if (Bone != LeftFoot.Bone && Bone != RightFoot.Bone)
			{
				LowerLegs.Add(Bone);
			}
		}
		else if (Mesh->BoneIsChildOf(Bone, Bones.LeftArm))
		{
			LeftArmBones.Add(Bone);
		}
		else if (Mesh->BoneIsChildOf(Bone, Bones.RightArm))
		{
			RightArmBones.Add(Bone);
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
	// 上臂就挂在这几节上，出拳时要按名字把它们绷紧，所以留着控制名。
	TorsoControls = PhysicsControl->CreateControlsFromSkeletalMesh(
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
		Mesh, { LeftFoot.Bone, RightFoot.Bone }, EPhysicsControlType::ParentSpace,
		MakeAngularControl(FootFacingStrength, StableMuscleDampingRatio), FootPostureSet);
	// 手臂正常由 BoxingPose 驱动。只有它建不起来时才退回这套跟随骨架动画的软控制，
	// 避免猜测局部轴导致关节持续扭动。
	TArray<FName> FallbackArmBones = LeftArmBones;
	FallbackArmBones.Append(RightArmBones);
	if (!BoxingPose.IsReady(0)) FallbackArmBones.Add(Bones.LeftArm);
	if (!BoxingPose.IsReady(1)) FallbackArmBones.Add(Bones.RightArm);
	const TArray<FName> FallbackArmControls = PhysicsControl->CreateControlsFromSkeletalMesh(
		Mesh, FallbackArmBones, EPhysicsControlType::ParentSpace, MakeAngularControl(ComedyArmStrength, 1.0f), ArmsSet);
	if (FallbackArmControls.IsEmpty() && !BoxingPose.IsReady(0) && !BoxingPose.IsReady(1))
	{
		// 这会直接导致出拳只能动躯干。带出骨骼配置问题，但不再在每次攻击时刷屏。
		UE_LOG(LogDelivery, Warning, TEXT("%s: No arm Physics Controls were created; check the arm bodies in the Physics Asset."),
			*GetNameSafe(GetOwner()));
	}

	// 摆动脚的世界空间位置电机。支撑阶段关掉，避免和地面摩擦较劲。
	FPhysicsControlData FootData;
	FootData.LinearStrength = LegPullStrength;
	FootData.LinearDampingRatio = StableMuscleDampingRatio;
	FootData.AngularStrength = 0.0f;
	FootData.bUseSkeletalAnimation = false;
	FootData.bUseAccelerationDriveMode = true;
	FootData.bOnlyControlChildObject = true;

	const TArray<FName> LeftControls = PhysicsControl->CreateControlsFromSkeletalMesh(
		Mesh, { LeftFoot.Bone }, EPhysicsControlType::WorldSpace, FootData, FeetSet);
	const TArray<FName> RightControls = PhysicsControl->CreateControlsFromSkeletalMesh(
		Mesh, { RightFoot.Bone }, EPhysicsControlType::WorldSpace, FootData, FeetSet);
	if (LeftControls.IsEmpty() || RightControls.IsEmpty())
	{
		return false;
	}

	LeftFoot.Control = LeftControls[0];
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
	BoxingPose = FDeliveryBoxingPose();
	bBodyDrivenPunchActive = false;
	bBodyDrivenPunchReleased = false;
	if (PhysicsControl)
	{
		PhysicsControl->DestroyControlsInSet(TEXT("All"));
	}
	PelvisControl = NAME_None;
	ChestControl = NAME_None;
	HeadControl = NAME_None;
	TorsoControls.Reset();
	BraceAlpha = 0.0f;
	AppliedBraceAlpha = -1.0f;
	AppliedBraceTargetStrength = -1.0f;
	PunchTwist = 0.0f;
	PunchLunge = 0.0f;
	HitPushAlpha = 0.0f;
	HitReactionEndTime = 0.0f;
	HitPushDirection = FVector::ZeroVector;
	bHasHitFacing = false;
	bHitFeetPlanted = false;
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
	const auto GetParentRelativeRotation = [this](const FName Bone)
	{
		const int32 BoneIndex = Mesh->GetBoneIndex(Bone);
		const USkeletalMesh* SkeletalMesh = Mesh->GetSkeletalMeshAsset();
		const int32 ParentIndex = BoneIndex == INDEX_NONE || !SkeletalMesh
			? INDEX_NONE
			: SkeletalMesh->GetRefSkeleton().GetParentIndex(BoneIndex);
		if (ParentIndex == INDEX_NONE)
		{
			return FQuat::Identity;
		}
		const FName ParentBone = Mesh->GetBoneName(ParentIndex);
		return Mesh->GetBoneQuaternion(ParentBone, EBoneSpaces::WorldSpace).Inverse()
			* Mesh->GetBoneQuaternion(Bone, EBoneSpaces::WorldSpace);
	};
	ReferenceLeftArmRelativeRotation = GetParentRelativeRotation(Bones.LeftArm);
	ReferenceRightArmRelativeRotation = GetParentRelativeRotation(Bones.RightArm);
	ReferenceFacingYaw = GetAimYaw();
	CurrentFacingYaw = ReferenceFacingYaw;
	UprightInPelvisSpace = ReferencePelvisRotation.UnrotateVector(FVector::UpVector).GetSafeNormal();

	const FName LeftSole = LeftFoot.SoleBone != NAME_None ? LeftFoot.SoleBone : Bones.LeftFoot;
	const FName RightSole = RightFoot.SoleBone != NAME_None ? RightFoot.SoleBone : Bones.RightFoot;
	const FVector LeftSoleLocation = Mesh->GetBoneLocation(LeftSole, EBoneSpaces::WorldSpace);
	const FVector RightSoleLocation = Mesh->GetBoneLocation(RightSole, EBoneSpaces::WorldSpace);
	LeftFoot.Target = Mesh->GetCenterOfMass(LeftFoot.Bone);
	RightFoot.Target = Mesh->GetCenterOfMass(RightFoot.Bone);
	LeftFoot.ReferenceRotation = Mesh->GetBoneQuaternion(LeftFoot.Bone, EBoneSpaces::WorldSpace);
	RightFoot.ReferenceRotation = Mesh->GetBoneQuaternion(RightFoot.Bone, EBoneSpaces::WorldSpace);
	LeftFoot.TargetRotation = LeftFoot.ReferenceRotation;
	RightFoot.TargetRotation = RightFoot.ReferenceRotation;

	FGroundHit LeftGround;
	FGroundHit RightGround;
	FVector LeftGroundPoint = LeftSoleLocation;
	FVector RightGroundPoint = RightSoleLocation;
	FVector LeftNormal = FVector::UpVector;
	FVector RightNormal = FVector::UpVector;
	if (TraceGround(LeftSoleLocation, FVector::UpVector, LeftGround))
	{
		LeftGroundPoint = LeftGround.Point;
		LeftNormal = LeftGround.Normal;
	}
	if (TraceGround(RightSoleLocation, FVector::UpVector, RightGround))
	{
		RightGroundPoint = RightGround.Point;
		RightNormal = RightGround.Normal;
	}

	// 没有脚刚体时质心在小腿。偏移只记质心到脚底，不把出生时的悬空写进站立高度。
	LeftFoot.GroundOffset = FMath::Max(0.0f, FVector::DotProduct(LeftFoot.Target - LeftSoleLocation, LeftNormal));
	RightFoot.GroundOffset = FMath::Max(0.0f, FVector::DotProduct(RightFoot.Target - RightSoleLocation, RightNormal));

	LeftFoot.Start = LeftFoot.Target;
	RightFoot.Start = RightFoot.Target;
	LeftFoot.Alpha = 1.0f;
	RightFoot.Alpha = 1.0f;
	const FVector AverageNormal = (LeftNormal + RightNormal).GetSafeNormal();
	CurrentGroundNormal = AverageNormal.IsNearlyZero() ? FVector::UpVector : AverageNormal;
	const FVector MidGround = 0.5f * (LeftGroundPoint + RightGroundPoint);
	const FVector MidSole = 0.5f * (LeftSoleLocation + RightSoleLocation);
	SmoothedGroundPoint = MidGround;
	bPelvisAirborne = false;
	// 髋目标从地面命中点抬起，但抬起量应是髋到视觉脚底的距离。
	// 若用髋到地面的距离，出生时脚底的离地间隙会被算进站高，停步后两脚就悬空。
	StandHeight = FMath::Clamp(FVector::DotProduct(Hips - MidSole, CurrentGroundNormal), 40.0f, 160.0f);
	LastWishDirection = GetOwner() ? GetOwner()->GetActorForwardVector() : FVector::ForwardVector;
	SmoothedBalanceOffset = FVector::ZeroVector;
	SmoothedMoveLead = FVector::ZeroVector;
	PlannedPelvisTarget = Hips;
	WishOnSlope = FVector::ZeroVector;
	SmoothedAccelerationAlpha = 0.0f;
	AccelerationLeanRemaining = 0.0f;
	bHadMoveWishLastTick = false;
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
		const bool bWasLimp = bIsLimp;
		bIsLimp = false;
		if (bWasLimp)
		{
			ReseedFromCurrentPose();
		}
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
	ResolveConfiguredPhysicsBones();
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
	else if (bIsActive)
	{
		// 必须在开电机之前：控制器里存的目标还是倒下前的，
		// 先按现在的姿势重新播种，人才会就地站起来而不是弹回原位。
		ReseedFromCurrentPose();
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
			else
			{
				// 把刚播种好的目标立刻写进电机，别让第一帧继续用旧值。
				UpdateControlTargets(0.0f);
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

bool UDeliveryActiveRagdollComponent::IsGrounded() const
{
	// 跳跃期间直接判定为离地：这一条就是「无二段跳」的实现，
	// 不依赖射线在起跳瞬间是否已经脱离地面。
	if (!bIsActive || bJumping || bPelvisAirborne || !Mesh)
	{
		return false;
	}

	// SmoothedGroundPoint 和 CurrentGroundNormal 由 UpdatePelvisTarget 每帧维护，
	// 这里复用它们，不再单独打一次射线。
	const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
	const float HeightAboveGround = FVector::DotProduct(Hips - SmoothedGroundPoint, CurrentGroundNormal);
	return HeightAboveGround <= StandHeight * (1.0f + GroundedHeightTolerance);
}

bool UDeliveryActiveRagdollComponent::TryStartJump()
{
	if (bIsLimp || !IsGrounded())
	{
		return false;
	}

	bJumping = true;
	JumpElapsed = 0.0f;
	// 摆动中的那只脚，它的世界空间位置电机正把脚拉向地面上的落点。
	// 人已经升空、脚却还在追地面，视觉上就是一条腿往后伸直。起跳瞬间先把迈步作废。
	CancelFootSteps();
	return true;
}

void UDeliveryActiveRagdollComponent::ReseedFromCurrentPose()
{
	if (!Mesh)
	{
		return;
	}

	const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);

	// 贴地点和法线按人现在躺的地方重新探一次，不要沿用倒下前的。
	FGroundHit Ground;
	if (TraceGround(Hips, FVector::UpVector, Ground))
	{
		bPelvisAirborne = false;
		CurrentGroundNormal = Ground.Normal;
		SmoothedGroundPoint = Ground.Point;
	}
	else
	{
		bPelvisAirborne = true;
		CurrentGroundNormal = FVector::UpVector;
		SmoothedGroundPoint = Hips - FVector::UpVector * StandHeight;
	}

	// StandHeight 保持不变：它是站立时髋到脚底的距离，躺着量出来只有几十厘米。
	PlannedPelvisTarget = SmoothedGroundPoint + CurrentGroundNormal * StandHeight;

	SmoothedBalanceOffset = FVector::ZeroVector;
	SmoothedMoveLead = FVector::ZeroVector;
	WishOnSlope = FVector::ZeroVector;
	SmoothedAccelerationAlpha = 0.0f;
	AccelerationLeanRemaining = 0.0f;
	bHadMoveWishLastTick = false;
	MoveInput = FVector2D::ZeroVector;
	LastWishDirection = FRotator(0.0f, CurrentFacingYaw, 0.0f).Vector();

	// 受击后退和跳跃的残留状态一并清掉，否则起身第一帧会带着它们跑。
	bJumping = false;
	JumpElapsed = 0.0f;
	JumpOffset = 0.0f;
	PunchLunge = 0.0f;
	HitPushAlpha = 0.0f;
	HitPushDirection = FVector::ZeroVector;
	bHasHitFacing = false;

	// 脚回到"站着、不迈步"的状态，并给一小段站定时间再开始走。
	CancelFootSteps();
	LeftFoot.Start = Mesh->GetCenterOfMass(LeftFoot.Bone);
	RightFoot.Start = Mesh->GetCenterOfMass(RightFoot.Bone);
	LeftFoot.Target = LeftFoot.Start;
	RightFoot.Target = RightFoot.Start;
	StartupPlantRemaining = StartupFootPlantDuration;
	bWasMoving = false;
	bPendingStopRecovery = false;
}

void UDeliveryActiveRagdollComponent::CancelFootSteps()
{
	LeftFoot.Alpha = 1.0f;
	RightFoot.Alpha = 1.0f;
	if (!PhysicsControl)
	{
		return;
	}
	if (!LeftFoot.Control.IsNone())
	{
		PhysicsControl->SetControlEnabled(LeftFoot.Control, false, true, false);
	}
	if (!RightFoot.Control.IsNone())
	{
		PhysicsControl->SetControlEnabled(RightFoot.Control, false, true, false);
	}
}

void UDeliveryActiveRagdollComponent::SetHitReactionStrength(float Multiplier)
{
	if (!PhysicsControl)
	{
		return;
	}

	FPhysicsControlMultiplier ControlMultiplier;
	ControlMultiplier.LinearStrengthMultiplier = FVector(Multiplier);
	ControlMultiplier.AngularStrengthMultiplier = Multiplier;
	FPhysicsControlMultiplier PelvisMultiplier;
	const float PelvisStrength = FMath::IsNearlyEqual(Multiplier, 1.0f)
		? 1.0f : FMath::Clamp(HitReactionPelvisStrengthMultiplier, 0.0f, 1.0f);
	PelvisMultiplier.LinearStrengthMultiplier = FVector(PelvisStrength);
	PelvisMultiplier.AngularStrengthMultiplier = PelvisStrength;
	// 控制名是创建时返回的真实名字。按名字更新，避免依赖可能未注册的自定义 Set。
	// 不重新启用已经因晕倒/远端代理而关闭的控制。
	if (!PelvisControl.IsNone())
	{
		PhysicsControl->SetControlMultiplier(PelvisControl, PelvisMultiplier, false, true, false);
	}
	if (!ChestControl.IsNone())
	{
		PhysicsControl->SetControlMultiplier(ChestControl, ControlMultiplier, false, true, false);
	}
	for (const FName& Control : TorsoControls)
	{
		PhysicsControl->SetControlMultiplier(Control, ControlMultiplier, false, true, false);
	}
}

void UDeliveryActiveRagdollComponent::SetHitFeetPlanted(bool bPlant)
{
	if (!PhysicsControl || !Mesh || bHitFeetPlanted == bPlant
		|| LeftFoot.Control.IsNone() || RightFoot.Control.IsNone())
	{
		return;
	}

	auto SetFootPlant = [this, bPlant](FFoot& Foot)
	{
		FPhysicsControlMultiplier FootMultiplier;
		FootMultiplier.LinearStrengthMultiplier = FVector(bPlant
			? FMath::Clamp(HitReactionFootStrengthMultiplier, 0.0f, 1.0f) : 1.0f);
		PhysicsControl->SetControlMultiplier(Foot.Control, FootMultiplier, false, true, false);
		if (bPlant)
		{
			// 用当前刚体质心作世界空间位置目标，不把脚拉去步态规划的下一落点；
			// 再沿被打飞的方向推出一段，脚被拖着挪出一小步，而不是钉死在原地。
			Foot.Start = Mesh->GetCenterOfMass(Foot.Bone);
			Foot.Target = Foot.Start
				+ HitPushDirection * FMath::Max(HitReactionFootSlideDistance, 0.0f);
			Foot.Alpha = 1.0f;
			PhysicsControl->SetControlTargetPositionAndOrientation(
				Foot.Control, Foot.Target, Foot.TargetRotation.Rotator(),
				0.0f, true, true, true, false);
		}
		PhysicsControl->SetControlEnabled(Foot.Control, bPlant, true, false);
	};

	SetFootPlant(LeftFoot);
	SetFootPlant(RightFoot);
	bHitFeetPlanted = bPlant;
	if (bPlant)
	{
		bWasMoving = false;
		bPendingStopRecovery = false;
	}
}

void UDeliveryActiveRagdollComponent::ApplyMeleeImpact(const FVector& Impulse, const FVector& ImpactPoint)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !bIsActive || !Mesh
		|| !HasPhysicsBody(Mesh, Bones.Spine))
	{
		return;
	}

	// 大部分冲量留在胸部；髋只分到很小一部分，两脚仍围绕原位置支撑。
	Mesh->AddImpulseAtLocation(Impulse, ImpactPoint, Bones.Spine);
	if (HasPhysicsBody(Mesh, Bones.Hips) && HitReactionPelvisImpulseFraction > 0.0f)
	{
		Mesh->AddImpulse(
			Impulse * FMath::Clamp(HitReactionPelvisImpulseFraction, 0.0f, 1.0f), Bones.Hips);
	}
	const FVector HorizontalDirection = FVector(Impulse.X, Impulse.Y, 0.0f).GetSafeNormal();
	if (!HorizontalDirection.IsNearlyZero() && HitReactionAngularVelocity > 0.0f)
	{
		// 绕横轴给胸部一个短促后仰，速度变化与刚体质量无关；方向随来拳方向变化。
		const FVector TiltAxis = FVector::CrossProduct(FVector::UpVector, HorizontalDirection);
		Mesh->AddAngularImpulseInRadians(
			TiltAxis * HitReactionAngularVelocity, Bones.Spine, true);
	}
	if (HitReactionUpwardImpulseFraction > 0.0f)
	{
		// 纯向上的一下，跟水平冲量分开算。上半身电机这段时间很松，弹起来之后自由落体，
		// 视觉上就是整个上身带着一跳，而不只是往后仰。
		Mesh->AddImpulseAtLocation(
			FVector::UpVector * Impulse.Size() * HitReactionUpwardImpulseFraction,
			ImpactPoint, Bones.Spine);
	}
	HitPushDirection = HorizontalDirection;
	if (!bIsLimp && HitReactionDuration > 0.0f && GetWorld())
	{
		if (!HorizontalDirection.IsNearlyZero())
		{
			// 冲量把人往外推，出拳的人就在它的反向。被前面打，就转过去面向前。
			HitFacingYaw = (-HorizontalDirection).Rotation().Yaw;
			bHasHitFacing = true;
		}
		HitReactionEndTime = GetWorld()->GetTimeSeconds() + HitReactionDuration;
		SetHitReactionStrength(FMath::Clamp(HitReactionStrengthMultiplier, 0.0f, 1.0f));
		SetHitFeetPlanted(true);
	}
}

void UDeliveryActiveRagdollComponent::BraceTorsoForAction(float DeltaTime, bool bPunch, bool bCarry)
{
	// 托举和出拳都要给肩膀稳定支点，否则两只手的电机反作用力会把软躯干来回拧。
	// 强度要渐变：一帧之内把整段脊柱从软切到硬，上半身会当场僵住。
	const float BraceTargetStrength = bCarry ? CarryBraceStrength : PunchBraceStrength;
	BraceAlpha = FMath::FInterpTo(BraceAlpha, bPunch || bCarry ? 1.0f : 0.0f, DeltaTime, PunchBraceSpeed);
	if (FMath::IsNearlyEqual(AppliedBraceAlpha, BraceAlpha, 0.02f)
		&& FMath::IsNearlyEqual(AppliedBraceTargetStrength, BraceTargetStrength, 0.1f)) return;
	AppliedBraceAlpha = BraceAlpha;
	AppliedBraceTargetStrength = BraceTargetStrength;
	PhysicsControl->SetControlAngularData(
		ChestControl, FMath::Lerp(LooseWaistFollowStrength, BraceTargetStrength, BraceAlpha),
		StableMuscleDampingRatio, 0.0f, 0.0f, true, true, false);
	const float Spine = FMath::Lerp(LooseComedyBodyStrength, BraceTargetStrength, BraceAlpha);
	for (const FName& Control : TorsoControls)
	{
		PhysicsControl->SetControlAngularData(
			Control, Spine, StableMuscleDampingRatio, 0.0f, 0.0f, true, true, false);
	}
}

void UDeliveryActiveRagdollComponent::BeginBodyDrivenPunch(FVector AimDirection, float HandSide)
{
	PunchAimDirection = AimDirection.GetSafeNormal2D();
	PunchHandSide = FMath::Sign(HandSide);
	bBodyDrivenPunchActive = !PunchAimDirection.IsNearlyZero();
	bBodyDrivenPunchReleased = false;
}

void UDeliveryActiveRagdollComponent::ReleaseBodyDrivenPunch()
{
	bBodyDrivenPunchReleased = bBodyDrivenPunchActive;
}

void UDeliveryActiveRagdollComponent::EndBodyDrivenPunch()
{
	bBodyDrivenPunchActive = false;
	bBodyDrivenPunchReleased = false;
}

FVector UDeliveryActiveRagdollComponent::GetWishDir() const
{
	if (MoveInput.IsNearlyZero())
	{
		return FVector::ZeroVector;
	}

	// 用镜头水平朝向把 WASD 变成世界方向，最后丢掉 Z。沿坡行走时再投影到切平面。
	const FRotator YawRotation(0.0f, GetAimYaw(), 0.0f);
	FVector Wish = (YawRotation.Vector() * MoveInput.Y
		+ FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y) * MoveInput.X).GetClampedToMaxSize(1.0f);
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner()))
	{
		if (const UDeliveryGrabComponent* Grab = Character->GetGrabComponent())
		{
			Wish = Grab->FilterApproachWish(Wish);
		}
	}
	return Wish;
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
	const FVector EffectiveWish = StartupPlantRemaining > 0.0f || bHitFeetPlanted
		? FVector::ZeroVector : Wish;
	UpdatePelvisTarget(DeltaTime, EffectiveWish);
	// 空中不跑步态：髋已经被抬高，这时规划落点会让脚去追够不到的地面点。
	// 腿改由各自的父空间角度电机拉回站立姿势，落地后再恢复迈步。
	if (!bHitFeetPlanted && !bJumping && !bPelvisAirborne)
	{
		UpdateFeet(DeltaTime, EffectiveWish);
	}
	const ADeliveryCharacter* GrabCharacter = Cast<ADeliveryCharacter>(GetOwner());
	const bool bCarryingProp = GrabCharacter && GrabCharacter->GetGrabComponent()
		&& GrabCharacter->GetGrabComponent()->IsCarryingProp();
	const bool bDraggingCharacter = GrabCharacter && GrabCharacter->GetGrabComponent()
		&& GrabCharacter->GetGrabComponent()->IsDraggingCharacter();
	DragReachAlpha = FMath::FInterpTo(DragReachAlpha,
		bDraggingCharacter && !bJumping ? 1.0f : 0.0f, DeltaTime, 7.0f);
	BraceTorsoForAction(DeltaTime, bBodyDrivenPunchActive, bCarryingProp);
	FVector GrabGoals[2] = { FVector::ZeroVector, FVector::ZeroVector };
	uint8 GrabMask = 0;
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner()))
	{
		if (const UDeliveryGrabComponent* Grab = Character->GetGrabComponent())
		{
			if (Grab->GetHandGoal(true, GrabGoals[0])) GrabMask |= 1;
			if (Grab->GetHandGoal(false, GrabGoals[1])) GrabMask |= 2;
		}
	}
	BoxingPose.Update(Mesh, PhysicsControl, CurrentFacingYaw, DeltaTime,
		bBodyDrivenPunchActive ? (PunchHandSide > 0 ? 0 : 1) : -1, bBodyDrivenPunchReleased,
		bBodyDrivenPunchActive ? PunchAimDirection : FVector::ZeroVector, GrabGoals, GrabMask);
}

void UDeliveryActiveRagdollComponent::UpdatePelvisTarget(float DeltaTime, const FVector& Wish)
{
	const ADeliveryCharacter* GrabCharacter = Cast<ADeliveryCharacter>(GetOwner());
	const bool bCarryingProp = GrabCharacter && GrabCharacter->GetGrabComponent()
		&& GrabCharacter->GetGrabComponent()->IsCarryingProp();
	const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
	const FVector Velocity = Mesh->GetPhysicsLinearVelocity(Bones.Hips);

	FGroundHit GroundUnderHips;
	bool bHasHipGround = TraceGround(Hips, CurrentGroundNormal, GroundUnderHips);
	if (!bHasHipGround)
	{
		bHasHipGround = TraceGround(Hips, FVector::UpVector, GroundUnderHips);
	}

	// 把水平 Wish 和当前速度都投影到坡面上，用它们的差估计髋要沿坡往前领多远。
	WishOnSlope = FVector::VectorPlaneProject(Wish, CurrentGroundNormal).GetSafeNormal() * Wish.Size();
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
			Mesh->GetCenterOfMass(LeftFoot.Bone) + Mesh->GetCenterOfMass(RightFoot.Bone));
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
	const bool bHasPlannedGround = SampleGround(Target, Hips, CurrentGroundNormal, Ground);
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
	// 上一帧的站立点把人吊在空中；落地时重新播种目标再启用。
	if (!bJumping && !bHasPlannedGround && !bHasHipGround)
	{
		if (!bPelvisAirborne)
		{
			bPelvisAirborne = true;
			CancelFootSteps();
		}
		// 从 Limp 恢复时 SetControlsInSetEnabled 会重新打开所有电机，
		// 即使 bPelvisAirborne 早已为 true，这里也必须每次明确关闭髋电机。
		PhysicsControl->SetControlEnabled(PelvisControl, false, true, false);
		return;
	}
	if (bPelvisAirborne)
	{
		bPelvisAirborne = false;
		CurrentGroundNormal = DesiredNormal;
		SmoothedGroundPoint = DesiredGroundPoint;
		PhysicsControl->SetControlEnabled(PelvisControl, true, true, false);
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
	// 跳跃只抬高髋部目标，不动冲量：抛物线在 JumpDuration 内从 0 升到 JumpHeight 再回到 0。
	// 腿和脚各自的电机会滞后地跟上来，连跳时来不及收腿，滑稽感就是从这个滞后里出来的。
	if (bJumping)
	{
		JumpElapsed += DeltaTime;
		const float Alpha = JumpDuration > KINDA_SMALL_NUMBER
			? FMath::Clamp(JumpElapsed / JumpDuration, 0.0f, 1.0f)
			: 1.0f;
		// 4*h*a*(1-a) 在 a=0.5 处取到 JumpHeight，两端为 0。
		JumpOffset = 4.0f * JumpHeight * Alpha * (1.0f - Alpha);
		if (Alpha >= 1.0f)
		{
			bJumping = false;
			JumpOffset = 0.0f;
			// 落地这一帧允许收一次脚，把两脚归到髋目标两侧的站宽上，
			// 否则原地连跳几次之后两脚会越站越开。
			CancelFootSteps();
			bWasMoving = false;
			bPendingStopRecovery = true;
		}
	}
	else
	{
		JumpOffset = 0.0f;
	}

	// 髋目标等于平滑后的贴地点，再加上站立高度沿法线抬起来。
	Target = SmoothedGroundPoint + CurrentGroundNormal
		* (StandHeight + SmoothBounceHeight * GaitPulse + JumpOffset);
	// 目标倒地时仅靠站立高度，肩到地面的距离超过手臂长度；平滑屈膝，
	// 让单手真的够到附近的身体表面，而不是永远差一截、建不了约束。
	Target -= CurrentGroundNormal * (DragReachCrouchHeight * DragReachAlpha);

	// 直拳的力道来自体重压上去，不是手臂伸得远。手臂本身只有三十几厘米行程，
	// 全身沿拳路前送这一下才是"打"和"推"的区别。脚会跟着这个目标上步。
	PunchLunge = FMath::FInterpTo(PunchLunge,
		bBodyDrivenPunchActive && bBodyDrivenPunchReleased ? 1.0f : 0.0f, DeltaTime, PunchLungeSpeed);
	if (PunchLunge > KINDA_SMALL_NUMBER && !PunchAimDirection.IsNearlyZero())
	{
		Target += FVector::VectorPlaneProject(PunchAimDirection, CurrentGroundNormal).GetSafeNormal()
			* (PunchLungeDistance * PunchLunge);
	}

	// 挨打不只是上身晃一下，髋部目标本身也顺着来拳方向往后挪一段再弹回来，
	// 整个人才像真的被打退了一步，而不是脚踩死原地光看上身在倒。
	HitPushAlpha = FMath::FInterpTo(
		HitPushAlpha, bHitFeetPlanted ? 1.0f : 0.0f, DeltaTime, HitReactionPelvisSlideSpeed);
	if (HitPushAlpha > KINDA_SMALL_NUMBER && !HitPushDirection.IsNearlyZero())
	{
		Target += FVector::VectorPlaneProject(HitPushDirection, CurrentGroundNormal).GetSafeNormal()
			* (HitReactionPelvisSlideDistance * HitPushAlpha);
	}

	PlannedPelvisTarget = Target;

	const FVector FacingWish = WishOnSlope.IsNearlyZero() ? Wish : WishOnSlope;
	float DesiredYaw = FacingWish.IsNearlyZero() ? CurrentFacingYaw : FacingWish.GetSafeNormal2D().Rotation().Yaw;
	if (bBodyDrivenPunchActive && !PunchAimDirection.IsNearlyZero())
	{
		// 出拳时身体转向瞄准方向。不转身的话拳头目标会落在肩膀活动范围之外，看起来就只有小臂在动。
		DesiredYaw = PunchAimDirection.Rotation().Yaw;
	}
	else if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
		Character && Character->GetGrabComponent() && Character->GetGrabComponent()->IsGrabbing())
	{
		// 手臂追物体世界抓点时，身体也必须面向抓点；只跟镜头转会让手从背后够箱子。
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
		// 挨打之后转过去面对来拳方向，而不是保持原朝向挨第二拳。
		DesiredYaw = HitFacingYaw;
	}
	const bool bTurningToHit = bHasHitFacing && !bBodyDrivenPunchActive;
	const float YawError = FMath::FindDeltaAngleDegrees(CurrentFacingYaw, DesiredYaw);
	CurrentFacingYaw = FMath::UnwindDegrees(bCarryingProp
		? CurrentFacingYaw + FMath::Clamp(YawError, -CarryTurnRate * DeltaTime, CarryTurnRate * DeltaTime)
		: FMath::FInterpTo(CurrentFacingYaw, CurrentFacingYaw + YawError, DeltaTime,
			bTurningToHit ? HitReactionFacingTurnSpeed : TurnResponsiveness));
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
	const bool bHasMoveWish = !Wish.IsNearlyZero();
	if (bHasMoveWish && !bHadMoveWishLastTick)
	{
		AccelerationLeanRemaining = AccelerationLeanDuration;
	}
	else if (!bHasMoveWish)
	{
		AccelerationLeanRemaining = 0.0f;
	}
	bHadMoveWishLastTick = bHasMoveWish;
	const float StartupLeanAlpha = AccelerationLeanDuration > KINDA_SMALL_NUMBER
		? AccelerationLeanRemaining / AccelerationLeanDuration : 0.0f;
	SmoothedAccelerationAlpha = FMath::FInterpTo(
		SmoothedAccelerationAlpha, StartupLeanAlpha, DeltaTime, DriveTargetSmoothingSpeed);
	AccelerationLeanRemaining = FMath::Max(0.0f, AccelerationLeanRemaining - DeltaTime);
	if (!Wish.IsNearlyZero())
	{
		const float LeanRadians = FMath::DegreesToRadians(
			AccelerationLeanAngle * SmoothedAccelerationAlpha * FMath::Clamp(Wish.Size(), 0.0f, 1.0f));
		const float WobbleRadians = FMath::DegreesToRadians(
			BouncyPelvisWobbleAngle * Wobble * (bCarryingProp ? 0.2f : 1.0f));
		const FVector DesiredUp = (StanceUp
			+ WishOnSlope * FMath::Tan(LeanRadians)
			+ SlopeRight * FMath::Tan(WobbleRadians)).GetSafeNormal();
		Lean = FQuat::FindBetweenNormals(StanceUp, DesiredUp);
	}
	if (DragReachAlpha > KINDA_SMALL_NUMBER)
	{
		FVector DragForward;
		if (GrabCharacter && GrabCharacter->GetGrabComponent()
			&& GrabCharacter->GetGrabComponent()->GetGrabFacingDirection(DragForward))
		{
			const FVector LeanUp = (StanceUp + DragForward.GetSafeNormal2D()
				* FMath::Tan(FMath::DegreesToRadians(DragReachLeanAngle * DragReachAlpha))).GetSafeNormal();
			Lean = FQuat::FindBetweenNormals(StanceUp, LeanUp) * Lean;
		}
	}

	// 纯水平拧身，不弯腰也不侧倾：蓄力时出拳侧的肩膀往后拧，释放后拧回来再带出去一点。
	// 正的 Yaw 会把左肩转向前方，所以左拳（PunchHandSide 为正）蓄力要给负角度。
	const float DesiredPunchTwist = !bBodyDrivenPunchActive ? 0.0f
		: PunchHandSide * (bBodyDrivenPunchReleased ? PunchFollowThroughAngle : -PunchSideStanceAngle);
	// 这个目标角度是阶跃的：出拳、释放、收拳各跳一次。骨盆电机很硬，把阶跃直接喂进去
	// 会把整个上半身连着伸出去的手臂横甩过去，看着就是在扇耳光。必须平滑。
	PunchTwist = FMath::FInterpTo(PunchTwist, DesiredPunchTwist, DeltaTime, PunchTwistSpeed);
	const FQuat PunchSideRotation = FMath::IsNearlyZero(PunchTwist)
		? FQuat::Identity
		: FQuat(CurrentGroundNormal, FMath::DegreesToRadians(PunchTwist));
	const FQuat PelvisRotation = PunchSideRotation * Lean * YawDelta * SlopeAlign * ReferencePelvisRotation;
	FQuat SpineRelativeRotation = ReferenceSpineRelativeRotation;
	if (!bCarryingProp && !Wish.IsNearlyZero() && !FMath::IsNearlyZero(Wobble)
		&& !SlopeForward.IsNearlyZero())
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

	const float StandingAlpha = 1.0f - FMath::Clamp(Wish.Size(), 0.0f, 1.0f);
	const FQuat BodyRotationDelta = PelvisRotation * ReferencePelvisRotation.Inverse();
	const FQuat HeadCorrection(
		SlopeRight, FMath::DegreesToRadians(StandingHeadCorrectionAngle * StandingAlpha));
	const FQuat HeadRotation = HeadCorrection * BodyRotationDelta * ReferenceHeadRotation;
	PhysicsControl->SetControlTargetPositionAndOrientation(
		HeadControl, FVector::ZeroVector, HeadRotation.Rotator(), DeltaTime, true, false, true, false);
}

void UDeliveryActiveRagdollComponent::UpdateFeet(float DeltaTime, const FVector& Wish)
{
	// Only runs while grounded and controlled; never counters a jump or limp ragdoll.
	if (GetUprightDot() >= MinimumStepUprightDot) KeepFeetOnOwnSide();
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
	const float LeftSide = FVector::DotProduct(Mesh->GetCenterOfMass(LeftFoot.Bone) - Hips, Right);
	const float RightSide = FVector::DotProduct(Mesh->GetCenterOfMass(RightFoot.Bone) - Hips, Right);
	// Repair a collapsed stance before normal stepping, including after releasing WASD.
	// Waiting until a foot is already on the opposite side lets knees become entangled.
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
	PhysicsControl->SetControlEnabled(Foot.Control, true, true, false);
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
	// Finish the landing at a fixed point: chasing a moving downhill target until
	// the last frame and immediately disabling the motor leaves the foot in midair.
	if (Foot.Alpha < 0.85f)
	{
		PlanFootLanding(Foot, WishOnSlope.IsNearlyZero() ? GetWishDir() : WishOnSlope);
	}

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
		const float Gap = FVector::Dist(Mesh->GetCenterOfMass(Foot.Bone), Position);
		if (Gap > 18.0f && Foot.Elapsed < ControlledStrideDuration + 0.18f)
		{
			Foot.Alpha = 0.999f;
			return;
		}
		// A bounded settling interval gives physics time to arrive without pinning a
		// blocked leg forever. The side guard below also protects the support leg.
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
	const float ForwardSpeed = FVector::DotProduct(
		FVector::VectorPlaneProject(Mesh->GetPhysicsLinearVelocity(Bones.Hips), CurrentGroundNormal),
		Forward);
	const float VelocityLead = Wish.IsNearlyZero()
		? 0.0f
		: FMath::Clamp(
			ForwardSpeed * ControlledStrideDuration * 0.35f,
			0.0f, ControlledStrideLength * 0.5f);
	FVector DestinationOnPlane = PlannedPelvisTarget
		+ Forward * (ForwardDistance + VelocityLead)
		+ Right * (Foot.SideSign * StableComedyStance);
	// Travel lead can point sideways while the body turns. Keep the landing on
	// this leg's anatomical side before tracing, so height is sampled at the corrected point.
	const FVector Hips = Mesh->GetCenterOfMass(Bones.Hips);
	const float SignedSide = FVector::DotProduct(DestinationOnPlane - Hips, Right) * Foot.SideSign;
	if (SignedSide < StableMinimumFootSide)
	{
		DestinationOnPlane += Right * Foot.SideSign * (StableMinimumFootSide - SignedSide);
	}

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

FVector UDeliveryActiveRagdollComponent::GetSlopeRight(const FVector& /*Wish*/) const
{
	// Left/right belongs to the body's smoothed facing, not the new movement input.
	// The latter can flip instantly while the pelvis is still turning on a slope.
	return DeliveryFootPlacement::SideAxis(CurrentGroundNormal, CurrentFacingYaw);
}

void UDeliveryActiveRagdollComponent::KeepFeetOnOwnSide()
{
	// Correct actual physics bodies, not just the swing target. The planted foot has
	// no position motor, so downhill sliding can otherwise cross the centreline.
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
	FCollisionObjectQueryParams GroundObjects;
	GroundObjects.AddObjectTypesToQuery(ECC_WorldStatic);
	GroundObjects.AddObjectTypesToQuery(ECC_WorldDynamic);
	// 电瓶车是 Vehicle；只认静态/动态地面，不把车顶误当成可站立坡面。
	if (World->LineTraceSingleByObjectType(Hit, Start, End, GroundObjects, Params))
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
		Bones.Hips, Bones.Spine, Bones.Head, LeftFoot.Bone, RightFoot.Bone,
		TEXT("LeftArm"), TEXT("LeftForeArm"), TEXT("LeftHand"),
		TEXT("RightArm"), TEXT("RightForeArm"), TEXT("RightHand")
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
