// 启动与物理电机的建立。这里先读取骨骼和物理资产里的站立参考姿势，再给各身体部位
// 建立物理控制。参考姿势描述“正常站立时各骨骼如何相对排列”，不是固定在世界中的站立点；
// 行走时每帧变化的目标位置和朝向在 Stance/Gait 文件计算。
#include "DeliveryActiveRagdollComponent.h"
#include "DeliveryRagdollPhysicsHelpers.h"
#include "Combat/DeliveryHandPose.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "PhysicsControlComponent.h"
#include "PhysicsControlData.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Math/RotationMatrix.h"
#include "../Delivery.h"

namespace
{
	using DeliveryRagdollPhysicsHelpers::HasPhysicsBody;
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
		// 关节电机只纠正朝向，实际角度仍受碰撞和刚体惯性影响。
		FPhysicsControlData Data;
		Data.AngularStrength = Strength;
		Data.AngularDampingRatio = Damping;
		Data.bUseSkeletalAnimation = true;
		Data.bUseAccelerationDriveMode = true;
		Data.bDisableCollision = true;
		return Data;
	}

	int32 BoneDepth(const USkeletalMeshComponent* Mesh, FName Bone)
	{
		// 沿父骨骼向上数，用于按骨架层级处理身体部位。
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

void UDeliveryActiveRagdollComponent::ResolveOwnerComponents()
{
	// 从角色身上找到已有的网格/胶囊；本组件不另建一套身体。
	if (AActor* Owner = GetOwner())
	{
		Mesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
		Capsule = Owner->FindComponentByClass<UCapsuleComponent>();
	}
}

void UDeliveryActiveRagdollComponent::ResolveConfiguredPhysicsBones()
{
	// 蓝图填写的是期望使用的骨骼名；这里核对物理资产并得到运行时实际可控制的刚体。
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
	// 启动前集中检查必需组件和骨骼，缺失时拒绝开启，避免每帧写入不存在的电机。
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
	// 开始模拟前先把初始姿势放到可站立表面，避免第一帧髋目标与地面相差很大。
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
	// 从这里开始由骨骼刚体承担运动和碰撞；胶囊之后只跟随髋部作角色定位。
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
	// 一次性建立髋、胸、脊柱、头、腿、脚等控制器。控制器描述力度和约束；
	// 每帧要追的具体位置/旋转仍由 Stance、Gait 和 BoxingPose 计算后写入。
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
	// 退出布娃娃或重新配置时清理本组件建立的控制，防止旧电机继续施力。
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
	bPunchFeetPlanted = false;
	bPunchSupportInterrupted = false;
	LeftFoot.Control = NAME_None;
	RightFoot.Control = NAME_None;
}

void UDeliveryActiveRagdollComponent::CacheStandingState()
{
	// 只缓存当前站姿的相对朝向、骨骼尺寸和脚底偏移，作为之后生成目标的参考。
	// 这里不是每帧保持不变的“世界目标”：真正目标会随输入、坡面和身体状态移动。
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
