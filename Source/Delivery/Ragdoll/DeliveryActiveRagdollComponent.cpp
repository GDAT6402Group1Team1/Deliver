// 管理布娃娃状态，并按物理阶段调度控制更新与网络采样
// Copyright Epic Games, Inc. All Rights Reserved.

// 物理计算前更新身体、双脚和手臂目标，计算后采集姿态并同步角色位置
// 身体目标沿地面调整，双脚轮流寻找支撑；进入陡坡翻滚时暂停主动控制

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
#include "../Delivery.h"
#include "Animation/AnimSequence.h"
#include "HAL/IConsoleManager.h"


#include "UObject/ConstructorHelpers.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryHandheldItem.h"

UDeliveryActiveRagdollComponent::UDeliveryActiveRagdollComponent()
{
	// 一帧调用两次：物理计算前给电机目标，物理计算后读取真正被模拟出的身体位置
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	SetIsReplicatedByDefault(true);

	PostPhysicsTickFunction.bCanEverTick = true;
	PostPhysicsTickFunction.bStartWithTickEnabled = false;
	PostPhysicsTickFunction.TickGroup = TG_PostPhysics;

	PhysicsControl = CreateDefaultSubobject<UPhysicsControlComponent>(TEXT("PhysicsControl"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> AxeAnimation(
	    TEXT("/Game/Characters/Animations/Axe/A_AxeDownward.A_AxeDownward"));
	AxeSwingAnimation = AxeAnimation.Object;
}

// 登记身体控制模式和物理快照的同步字段
void UDeliveryActiveRagdollComponent::GetLifetimeReplicatedProps(
    TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UDeliveryActiveRagdollComponent, ReplicatedControlMode);
	DOREPLIFETIME(UDeliveryActiveRagdollComponent, ReplicatedSnapshot);
	DOREPLIFETIME_CONDITION(UDeliveryActiveRagdollComponent, CurrentFacingYaw, COND_SimulatedOnly);
}

// 将身体更新安排在合适的物理阶段
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

// 查找角色组件并准备身体控制
void UDeliveryActiveRagdollComponent::BeginPlay()
{
	Super::BeginPlay();
	ResolveOwnerComponents();
	// 先计算本帧目标，再让物理控制器应用，避免使用上一帧的脚部目标
	PhysicsControl->AddTickPrerequisiteComponent(this);
	if (Mesh)
		AddTickPrerequisiteComponent(Mesh);

	if (bStartOnBeginPlay)
	{
		if (GetOwner() && GetOwner()->HasAuthority())
		{
			ReplicatedControlMode = EDeliveryRagdollControlMode::Active;
		}
		StartRagdoll();
	}
}

// 根据物理阶段和网络角色执行控制更新、快照播放或服务器采样
// 物理计算前更新目标，计算后采集姿态；远端角色仅播放服务器快照
void UDeliveryActiveRagdollComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    UpdateActionState();
    if (!bIsActive) return;

    if (ThisTickFunction == &PostPhysicsTickFunction)
        UpdateAfterPhysics(DeltaTime);
    else
        UpdateBeforePhysics(DeltaTime);
}

// 保留原有检查时机，前后物理阶段都处理失效动作和到期受击
void UDeliveryActiveRagdollComponent::UpdateActionState()
{
	if (SwingAxe.IsValid())
	{
		const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner());
		bool bCanContinueSwing = bIsActive && !bIsLimp && Character && !Character->IsIncapacitated();

		// 动作期间必须始终持有同一把斧头
		if (bCanContinueSwing)
		{
			const UDeliveryInventoryComponent* Inventory = Character->GetInventoryComponent();
			bCanContinueSwing = Inventory && Inventory->GetHeldItem() == SwingAxe.Get();
		}

		// 抓取占用双手，开始抓取后立即取消下劈
		if (bCanContinueSwing)
		{
			const UDeliveryGrabComponent* Grab = Character->GetGrabComponent();
			bCanContinueSwing = !Grab || !Grab->IsGrabbing();
		}

		if (!bCanContinueSwing)
		{
			// 清除动作使用的物品引用，重新装备同一把斧头也不会续播旧动作
			SwingAxe.Reset();
		}
	}

	if (!bIsActive)
	{
		return;
	}
	if (GetOwner()->HasAuthority() && HitReactionEndTime > 0.0f && GetWorld() &&
	    GetWorld()->GetTimeSeconds() >= HitReactionEndTime)
	{
		HitReactionEndTime = 0.0f;
		SetHitReactionStrength(1.0f);
		SetHitFeetPlanted(false);
		bHasHitFacing = false;
		HitPushDirection = FVector::ZeroVector;
	}

}

// 先处理网络姿态和身体状态，再计算本帧控制目标
void UDeliveryActiveRagdollComponent::UpdateBeforePhysics(float DeltaTime)
{
	// 其他玩家的身体姿态使用服务器发来的结果
	if (GetOwner()->GetLocalRole() == ROLE_SimulatedProxy || Network.IsPlayingRecovery())
	{
		ApplyNetworkSnapshot(DeltaTime);
		return;
	}

	if (GetOwner()->GetLocalRole() == ROLE_AutonomousProxy)
	{
		// 用服务器发来的姿态修正本机身体的位置
		ApplyNetworkSnapshot(DeltaTime);
	}
	if (GetOwner()->HasAuthority())
	{
		UpdateSlopeTumble(DeltaTime);
		UpdateLimpRecovery(DeltaTime);
	}

	if (!bIsLimp)
	{
		// 晕倒时不更新主动控制目标，让身体继续按物理规则运动
		UpdateControlTargets(DeltaTime);
	}
#if !UE_BUILD_SHIPPING
	TraceRecoveryPhysics(false, DeltaTime);
#endif
}

// 读取已经模拟完成的身体，最后更新角色根位置
void UDeliveryActiveRagdollComponent::UpdateAfterPhysics(float DeltaTime)
{
#if !UE_BUILD_SHIPPING
	TraceRecoveryPhysics(true, DeltaTime);
#endif
	if (GetOwner()->HasAuthority())
	{
		if (Network.ShouldCapture(DeltaTime, NetworkSnapshotRate))
		{
			CaptureNetworkSnapshot();
		}
	}

	if (GetOwner()->HasAuthority() || GetOwner()->GetLocalRole() == ROLE_AutonomousProxy)
	{
		SyncOwnerToPelvis(DeltaTime);
	}
}



// 准备物理身体、控制目标和同步状态，开始主动布娃娃运行
void UDeliveryActiveRagdollComponent::StartRagdoll()
{
	// 初始化顺序很重要：先找到物理骨骼并确认配置，再启动刚体、缓存当前站姿，
	// 最后创建电机并写入第一帧目标，避免电机一启动就追旧位置
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
			// 客户端收到 Active 复制状态时也走同一条渐进起身路径
			ApplyLimpState(false);
			return;
		}
		if (PhysicsControl)
		{
			const bool bRemoteProxy = Owner && Owner->GetLocalRole() == ROLE_SimulatedProxy;
			PhysicsControl->SetControlsInSetEnabled(TEXT("All"), !bRemoteProxy);
			PhysicsControl->SetControlEnabled(Movement.LeftFoot.Control, false, true, false);
			PhysicsControl->SetControlEnabled(Movement.RightFoot.Control, false, true, false);
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
		UE_LOG(LogDelivery, Error, TEXT("%s: Failed to create Physics Control drives."),
		       *GetNameSafe(GetOwner()));
		StopRagdoll();
		return;
	}

	bIsActive = true;
	bIsLimp = false;
	bExternalLimpRequested = false;
	Recovery.Reset();
	Network.ResetRecovery();
	bSlopeTumbling = false;
	TumbleEntryTime = TumbleElapsed = TumbleRecoveryTime = TumbleCooldownTime = 0.0f;
	SetComponentTickEnabled(true);
	PostPhysicsTickFunction.SetTickFunctionEnable(true);
	UpdateControlTargets(0.0f);
	if (Owner && Owner->GetLocalRole() == ROLE_SimulatedProxy)
	{
		// 远端身体由服务端快照驱动，不再运行本地肌肉目标
		PhysicsControl->SetControlsInSetEnabled(TEXT("All"), false);
	}

	UE_LOG(LogDelivery, Log, TEXT("%s: Active ragdoll started with %d bodies."), *GetNameSafe(GetOwner()),
	       Mesh->GetPhysicsAsset()->SkeletalBodySetups.Num());
}

// 停止身体控制并清理约束和运行状态
void UDeliveryActiveRagdollComponent::StopRagdoll()
{
	SwingAxe.Reset();
	// 撤销这套物理控制并把角色恢复到非布娃娃状态；不沿用旧的脚步/输入缓存
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
	Recovery.Reset();
	Network.ResetRecovery();
	bSlopeTumbling = false;
	TumbleEntryTime = TumbleElapsed = TumbleRecoveryTime = TumbleCooldownTime = 0.0f;
	Movement.MoveInput = FVector2D::ZeroVector;
	Movement.StartupPlantRemaining = 0.0f;
	Movement.bWasMoving = false;
	Movement.bPendingStopRecovery = false;
	Network.Reset();
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

// 由服务器切换身体是否失去主动支撑
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
	// HP 恢复可能发生在空中或仍高速翻滚时；由 Tick 的落地稳定判定开启电机
}

// 根据倒地状态启停控制，并准备恢复站立
void UDeliveryActiveRagdollComponent::ApplyLimpState(bool bLimp)
{
	if (bLimp)
		SwingAxe.Reset();
	// 晕倒不是关闭物理模拟，而是放开站立电机，让整条刚体链继续受重力/碰撞驱动
	// 恢复时重新播种当前身体位置，避免追向晕倒前缓存的世界位置
	if (bIsActive && bIsLimp == bLimp)
	{
		return;
	}
	AActor* Owner = GetOwner();
	const EDeliveryRagdollControlMode NewMode =
	    bLimp ? EDeliveryRagdollControlMode::Limp : EDeliveryRagdollControlMode::Recovering;
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
		Recovery.Reset();
		// 晕倒前若正在受击，不能把锁脚和削弱电机的限时状态带进下一次起身
		HitReactionEndTime = 0.0f;
		SetHitFeetPlanted(false);
		SetHitReactionStrength(1.0f);
		SetPunchFeetPlanted(false);
		bPunchSupportInterrupted = true;
		Movement.MoveInput = FVector2D::ZeroVector;
	}
	else if (bIsActive)
	{
		// 必须在开电机之前：控制器里存的目标还是倒下前的，
		// 先按现在的姿势重新播种，人才会就地站起来而不是弹回原位
		ReseedFromCurrentPose();
		const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
		Recovery.BeginRise(FMath::Clamp(
		    FVector::DotProduct(Hips - Movement.SmoothedGroundPoint, Movement.CurrentGroundNormal), 20.0f, StandHeight));
	}
	if (PhysicsControl && bIsActive)
	{
		if (!bLimp)
		{
			// All 控制组即将重新启用；先把力度降下来，避免第一物理帧全身电机同时猛拉
			SetLimpRecoveryDriveStrength(0.0f);
		}
		PhysicsControl->SetControlsInSetEnabled(TEXT("All"), !bLimp);
		if (!bLimp)
		{
			PhysicsControl->SetControlEnabled(Movement.LeftFoot.Control, false, true, false);
			PhysicsControl->SetControlEnabled(Movement.RightFoot.Control, false, true, false);
			if (Owner && Owner->GetLocalRole() == ROLE_SimulatedProxy)
			{
				PhysicsControl->SetControlsInSetEnabled(TEXT("All"), false);
			}
			else
			{
				// 把刚播种好的目标立刻写进电机，别让第一帧继续用旧值
				UpdateControlTargets(0.0f);
			}
		}
	}
}

// 在起身期间逐渐恢复身体控制强度
void UDeliveryActiveRagdollComponent::SetLimpRecoveryDriveStrength(float Alpha)
{
	if (!PhysicsControl)
	{
		return;
	}
	const float Strength =
	    FMath::Lerp(FMath::Clamp(LimpRecoveryInitialDriveStrength, 0.0f, 1.0f), 1.0f,
		            FMath::InterpEaseInOut(0.0f, 1.0f, FMath::Clamp(Alpha, 0.0f, 1.0f), 2.0f));
	FPhysicsControlMultiplier Multiplier;
	Multiplier.LinearStrengthMultiplier = FVector(Strength);
	Multiplier.AngularStrengthMultiplier = Strength;
	PhysicsControl->SetControlMultipliersInSet(TEXT("All"), Multiplier, false);
}

// 保存经过限制的移动意图，供后续身体和步态更新使用
void UDeliveryActiveRagdollComponent::SetMoveInput(FVector2D RightForward)
{
	Movement.MoveInput.X = FMath::Clamp(RightForward.X, -1.0f, 1.0f);
	Movement.MoveInput.Y = FMath::Clamp(RightForward.Y, -1.0f, 1.0f);
	if (GetOwner() && GetOwner()->HasAuthority() && GetWorld())
	{
		Movement.LastMoveInputTime = GetWorld()->GetTimeSeconds();
	}
}

// 向指定身体部位施加外部冲量
void UDeliveryActiveRagdollComponent::AddImpulse(FVector Impulse, bool bVelocityChange)
{
	if (bIsActive && Mesh)
	{
		Mesh->AddImpulse(Impulse, Bones.Hips, bVelocityChange);
	}
}

// 检查当前支撑面是否允许按站立状态处理
bool UDeliveryActiveRagdollComponent::IsGrounded() const
{
	// 跳跃期间直接判定为离地：这一条就是「无二段跳」的实现，
	// 不依赖射线在起跳瞬间是否已经脱离地面
	if (!bIsActive || Movement.bJumping || Movement.bPelvisAirborne || !Mesh)
	{
		return false;
	}

	// SmoothedGroundPoint 和 CurrentGroundNormal 由 UpdatePelvisTarget 每帧维护，
	// 这里复用它们，不再单独打一次射线
	const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);
	const float HeightAboveGround = FVector::DotProduct(Hips - Movement.SmoothedGroundPoint, Movement.CurrentGroundNormal);
	return HeightAboveGround <= StandHeight * (1.0f + GroundedHeightTolerance);
}

// 确认身体和地面条件后开始跳跃
bool UDeliveryActiveRagdollComponent::TryStartJump()
{
	// 仅在可站立时启动一次跳跃；后续抬高目标的时间曲线由 UpdateJumpHeight 推进
	if (ReplicatedControlMode != EDeliveryRagdollControlMode::Active || bIsLimp || !IsGrounded())
	{
		return false;
	}

	Movement.bJumping = true;
	Movement.JumpElapsed = 0.0f;
	// 摆动中的那只脚，它的世界空间位置电机正把脚拉向地面上的落点
	// 人已经升空、脚却还在追地面，视觉上就是一条腿往后伸直；起跳瞬间先把迈步作废
	CancelFootSteps();
	return true;
}

// 用身体当前姿势重建控制目标，避免恢复时拉回旧位置
void UDeliveryActiveRagdollComponent::ReseedFromCurrentPose()
{
	// 起身或重新落地时，以“现在真实的髋位置和脚位置”重新初始化平滑值
	// 缓存的站立骨骼相对姿势仍是姿态参考，但旧的世界站立点不能继续使用
	if (!Mesh)
	{
		return;
	}

	const FVector Hips = Mesh->GetBoneLocation(Bones.Hips, EBoneSpaces::WorldSpace);

	// 贴地点和法线按人现在躺的地方重新探一次，不要沿用倒下前的
	FGroundHit Ground;
	if (TraceGround(Hips, FVector::UpVector, Ground))
	{
		Movement.bPelvisAirborne = false;
		Movement.CurrentGroundNormal = Ground.Normal;
		Movement.SmoothedGroundPoint = Ground.Point;
	}
	else
	{
		Movement.bPelvisAirborne = true;
		Movement.CurrentGroundNormal = FVector::UpVector;
		Movement.SmoothedGroundPoint = Hips - FVector::UpVector * StandHeight;
	}

	// StandHeight 保持不变：它是站立时髋到脚底的距离，躺着量出来只有几十厘米
	Movement.PlannedPelvisTarget = Movement.SmoothedGroundPoint + Movement.CurrentGroundNormal * StandHeight;

	Movement.SmoothedBalanceOffset = FVector::ZeroVector;
	Movement.SmoothedMoveLead = FVector::ZeroVector;
	Movement.WishOnSlope = FVector::ZeroVector;
	Movement.SmoothedAccelerationAlpha = 0.0f;
	Movement.AccelerationLeanRemaining = 0.0f;
	Movement.bStandAnchored = false;
	Movement.bHadMoveWishLastTick = false;
	Movement.MoveInput = FVector2D::ZeroVector;
	Movement.LastWishDirection = FRotator(0.0f, CurrentFacingYaw, 0.0f).Vector();

	// 受击后退和跳跃的残留状态一并清掉，否则起身第一帧会带着它们跑
	Movement.bJumping = false;
	Movement.JumpElapsed = 0.0f;
	Movement.JumpOffset = 0.0f;
	PunchLunge = 0.0f;
	HitPushAlpha = 0.0f;
	HitPushDirection = FVector::ZeroVector;
	bHasHitFacing = false;

	// 脚回到"站着、不迈步"的状态，并给一小段站定时间再开始走
	Movement.BlockedStepRecoveryRemaining = 0.0f;
	CancelFootSteps();
	Movement.LeftFoot.Start = Mesh->GetCenterOfMass(Movement.LeftFoot.Bone);
	Movement.RightFoot.Start = Mesh->GetCenterOfMass(Movement.RightFoot.Bone);
	Movement.LeftFoot.Target = Movement.LeftFoot.Start;
	Movement.RightFoot.Target = Movement.RightFoot.Start;
	Movement.StartupPlantRemaining = StartupFootPlantDuration;
	Movement.bWasMoving = false;
	Movement.bPendingStopRecovery = false;
}

// 终止当前迈步并清理双脚的摆动状态
void UDeliveryActiveRagdollComponent::CancelFootSteps()
{
	// 取消尚未完成的脚步目标；跳跃、晕倒或失去地面时脚不应继续追旧落点
	SetPunchFeetPlanted(false);
	Movement.LeftFoot.Alpha = 1.0f;
	Movement.RightFoot.Alpha = 1.0f;
	if (!PhysicsControl)
	{
		return;
	}
	if (!Movement.LeftFoot.Control.IsNone())
	{
		PhysicsControl->SetControlEnabled(Movement.LeftFoot.Control, false, true, false);
	}
	if (!Movement.RightFoot.Control.IsNone())
	{
		PhysicsControl->SetControlEnabled(Movement.RightFoot.Control, false, true, false);
	}
}

// 将输入转换成世界中的移动方向
FVector UDeliveryActiveRagdollComponent::GetWishDir() const
{
	// 把玩家的“左右/前后”二维输入转换为世界平面方向；坡面投影在 Stance 中完成
	if (Movement.MoveInput.IsNearlyZero())
	{
		return FVector::ZeroVector;
	}

	// 用镜头水平朝向把 WASD 变成世界方向，最后丢掉 Z；沿坡行走时再投影到切平面
	const FRotator YawRotation(0.0f, GetAimYaw(), 0.0f);
	FVector Wish = (YawRotation.Vector() * Movement.MoveInput.Y +
	                FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y) * Movement.MoveInput.X)
	                   .GetClampedToMaxSize(1.0f);
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(GetOwner()))
	{
		if (const UDeliveryGrabComponent* Grab = Character->GetGrabComponent())
		{
			Wish = Grab->FilterApproachWish(Wish);
		}
	}
	return Wish;
}

// 取得角色希望面对的水平角度
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
