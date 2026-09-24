// Copyright Epic Games, Inc. All Rights Reserved.

/*
阅读入口：TickComponent 决定什么时候运行控制；UpdateControlTargets 决定本帧先更新身体、
再更新双脚和手臂。各阶段的具体计算见同目录 Stance/Gait/Ground/Combat/Network 实现文件。
所有文件实现的仍是同一个组件，蓝图参数和服务器复制状态只在头文件中保存一份。

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

髋先走，脚后追。这套步态只处理比较缓的斜面；陡坡切成全物理翻滚。
*/

#include "DeliveryActiveRagdollComponent.h"
#include "DeliveryCharacter.h"
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
#include "Math/RotationMatrix.h"
#include "../Delivery.h"


UDeliveryActiveRagdollComponent::UDeliveryActiveRagdollComponent()
{
	// 一帧调用两次：物理计算前给电机目标，物理计算后读取真正被模拟出的身体位置。
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

	// 后物理阶段：服务器把已经模拟完的骨骼状态打包；本机再让碰撞胶囊跟随髋部，
	// 这样镜头和交互位置跟着真实身体走，而不是跟着电机的目标点走。
	if (ThisTickFunction == &PostPhysicsTickFunction)
	{
#if !UE_BUILD_SHIPPING
		TraceRecoveryPhysics(true, DeltaTime);
#endif
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

	// 旁观其他玩家的客户端不驱动这具布娃娃，只平滑播放服务器发来的刚体快照。
	if (GetOwner()->GetLocalRole() == ROLE_SimulatedProxy || bClientRecoveryPlayback)
	{
		ApplyNetworkSnapshot(DeltaTime);
		return;
	}

	if (GetOwner()->GetLocalRole() == ROLE_AutonomousProxy)
	{
		// 本机玩家也接受权威快照，用来修正本地模拟与服务器之间的偏差。
		ApplyNetworkSnapshot(DeltaTime);
	}
	if (GetOwner()->HasAuthority())
	{
		UpdateSlopeTumble(DeltaTime);
		UpdateLimpRecovery(DeltaTime);
	}

	if (!bIsLimp)
	{
		// 只给仍有意识的角色写入站立、脚步和手臂目标；晕倒时让刚体自由运动。
		UpdateControlTargets(DeltaTime);
	}
#if !UE_BUILD_SHIPPING
	TraceRecoveryPhysics(false, DeltaTime);
#endif
}

#if !UE_BUILD_SHIPPING
void UDeliveryActiveRagdollComponent::TraceRecoveryPhysics(bool bAfterPhysics, float DeltaTime)
{
	if (!GetOwner()->HasAuthority() || !Mesh || !PhysicsControl || !GetWorld()) return;
	const FBodyInstance* Body = Mesh->GetBodyInstance(Bones.Hips);
	if (!Body) return;
	const float Now = GetWorld()->GetTimeSeconds();
	const int32 Mode = static_cast<int32>(ReplicatedControlMode);
	if (!bAfterPhysics)
	{
		if (Mode != DiagnosticRecoveryMode)
		{
			if (DiagnosticRecoveryMode != -1 || bIsLimp || bRisingFromLimp)
			{
				DiagnosticRecoveryUntil = Now + 8.0f;
				DiagnosticRecoveryNextLog = Now;
				UE_LOG(LogDelivery, Log, TEXT("RecoveryTrace transition actor=%s world=%s time=%.3f mode=%d->%d"),
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
	if (!bDiagnosticPrePhysicsValid) return;
	bDiagnosticPrePhysicsValid = false;
	const FVector Position = Body->GetUnrealWorldTransform().GetLocation();
	const float StepXY = FVector::Dist2D(Position, DiagnosticPrePhysicsPosition);
	if (Now < DiagnosticRecoveryNextLog && StepXY < 20.0f) return;
	DiagnosticRecoveryNextLog = Now + 0.1f;
	FPhysicsControlTarget DriveTarget;
	PhysicsControl->GetControlTarget(PelvisControl, DriveTarget);
	UE_LOG(LogDelivery, Log, TEXT("RecoveryTrace actor=%s world=%s time=%.3f dt=%.4f mode=%d limp=%d rise=%.2f airborne=%d drive=%d pre=%s post=%s mesh=%s actorPos=%s preV=%s postV=%s target=%s targetV=%s stepXY=%.1f feet=%d,%d"),
		*GetNameSafe(GetOwner()), *GetNameSafe(GetWorld()), Now, DeltaTime, Mode,
		bIsLimp, LimpRecoveryRiseAlpha, bPelvisAirborne, PhysicsControl->GetControlEnabled(PelvisControl),
		*DiagnosticPrePhysicsPosition.ToCompactString(), *Position.ToCompactString(),
		*Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace).ToCompactString(),
		*GetOwner()->GetActorLocation().ToCompactString(),
		*DiagnosticPrePhysicsVelocity.ToCompactString(), *Body->GetUnrealWorldVelocity().ToCompactString(),
		*DriveTarget.TargetPosition.ToCompactString(), *DriveTarget.TargetVelocity.ToCompactString(), StepXY,
		PhysicsControl->GetControlEnabled(LeftFoot.Control), PhysicsControl->GetControlEnabled(RightFoot.Control));
}
#endif


void UDeliveryActiveRagdollComponent::StartRagdoll()
{
	// 初始化顺序很重要：先找到物理骨骼并确认配置，再启动刚体、缓存当前站姿，
	// 最后创建电机并写入第一帧目标，避免电机一启动就追旧位置。
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
		if (bIsLimp)
		{
			// 客户端收到 Active 复制状态时也走同一条渐进起身路径。
			ApplyLimpState(false);
			return;
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
	bExternalLimpRequested = false;
	LimpRecoveryStableTime = 0.0f;
	LimpRecoveryStandingTime = 0.0f;
	bRisingFromLimp = false;
	bClientRecoveryPlayback = false;
	bClientRecoveryAwaitingSnapshot = false;
	LimpRecoveryRiseAlpha = 1.0f;
	bSlopeTumbling = false;
	TumbleEntryTime = TumbleElapsed = TumbleRecoveryTime = TumbleCooldownTime = 0.0f;
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
	// 撤销这套物理控制并把角色恢复到非布娃娃状态；不沿用旧的脚步/输入缓存。
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
	bExternalLimpRequested = false;
	LimpRecoveryStableTime = 0.0f;
	LimpRecoveryStandingTime = 0.0f;
	bRisingFromLimp = false;
	bClientRecoveryPlayback = false;
	bClientRecoveryAwaitingSnapshot = false;
	LimpRecoveryRiseAlpha = 1.0f;
	bSlopeTumbling = false;
	TumbleEntryTime = TumbleElapsed = TumbleRecoveryTime = TumbleCooldownTime = 0.0f;
	MoveInput = FVector2D::ZeroVector;
	StartupPlantRemaining = 0.0f;
	bWasMoving = false;
	bPendingStopRecovery = false;
	SnapshotAccumulator = 0.0f;
	bHasNetworkSnapshot = false;
	bHasOwnerCorrectionSequence = false;
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
	bExternalLimpRequested = bLimp;
	if (bLimp)
	{
		ApplyLimpState(true);
	}
	else if (!bIsActive)
	{
		StartRagdoll();
	}
	// HP 恢复可能发生在空中或仍高速翻滚时；由 Tick 的落地稳定判定开启电机。
}

void UDeliveryActiveRagdollComponent::ApplyLimpState(bool bLimp)
{
	// 晕倒不是关闭物理模拟，而是放开站立电机，让整条刚体链继续受重力/碰撞驱动。
	// 恢复时重新播种当前身体位置，避免追向晕倒前缓存的世界位置。
	if (bIsActive && bIsLimp == bLimp)
	{
		return;
	}
	AActor* Owner = GetOwner();
	const EDeliveryRagdollControlMode NewMode = bLimp
		? EDeliveryRagdollControlMode::Limp
		: EDeliveryRagdollControlMode::Recovering;
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
		LimpRecoveryStableTime = 0.0f;
		LimpRecoveryStandingTime = 0.0f;
		LimpRecoveryRiseAlpha = 1.0f;
		bRisingFromLimp = false;
		// 晕倒前若正在受击，不能把锁脚和削弱电机的限时状态带进下一次起身。
		HitReactionEndTime = 0.0f;
		SetHitFeetPlanted(false);
		SetHitReactionStrength(1.0f);
		SetPunchFeetPlanted(false);
		bPunchSupportInterrupted = true;
		MoveInput = FVector2D::ZeroVector;
	}
	else if (bIsActive)
	{
		// 必须在开电机之前：控制器里存的目标还是倒下前的，
		// 先按现在的姿势重新播种，人才会就地站起来而不是弹回原位。
		ReseedFromCurrentPose();
		const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
		LimpRecoveryStartHeight = FMath::Clamp(
			FVector::DotProduct(Hips - SmoothedGroundPoint, CurrentGroundNormal),
			20.0f, StandHeight);
		LimpRecoveryRiseAlpha = 0.0f;
		LimpRecoveryStandingTime = 0.0f;
		bRisingFromLimp = true;
	}
	if (PhysicsControl && bIsActive)
	{
		if (!bLimp)
		{
			// All 控制组即将重新启用。先把力度降下来，避免第一物理帧全身电机同时猛拉。
			SetLimpRecoveryDriveStrength(0.0f);
		}
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

void UDeliveryActiveRagdollComponent::SetLimpRecoveryDriveStrength(float Alpha)
{
	if (!PhysicsControl)
	{
		return;
	}
	const float Strength = FMath::Lerp(
		FMath::Clamp(LimpRecoveryInitialDriveStrength, 0.0f, 1.0f), 1.0f,
		FMath::InterpEaseInOut(0.0f, 1.0f, FMath::Clamp(Alpha, 0.0f, 1.0f), 2.0f));
	FPhysicsControlMultiplier Multiplier;
	Multiplier.LinearStrengthMultiplier = FVector(Strength);
	Multiplier.AngularStrengthMultiplier = Strength;
	PhysicsControl->SetControlMultipliersInSet(TEXT("All"), Multiplier, false);
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
	// 仅在可站立时启动一次跳跃；后续抬高目标的时间曲线由 UpdateJumpHeight 推进。
	if (ReplicatedControlMode != EDeliveryRagdollControlMode::Active || bIsLimp || !IsGrounded())
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
	// 起身或重新落地时，以“现在真实的髋位置和脚位置”重新初始化平滑值。
	// 缓存的站立骨骼相对姿势仍是姿态参考，但旧的世界站立点不能继续使用。
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
	// 取消尚未完成的脚步目标；跳跃、晕倒或失去地面时脚不应继续追旧落点。
	SetPunchFeetPlanted(false);
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


FVector UDeliveryActiveRagdollComponent::GetWishDir() const
{
	// 把玩家的“左右/前后”二维输入转换为世界平面方向；坡面投影在 Stance 中完成。
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
