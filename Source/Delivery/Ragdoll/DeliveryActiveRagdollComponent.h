// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/NetSerialization.h"
#include "Combat/DeliveryBoxingPose.h"
#include "DeliveryActiveRagdollComponent.generated.h"

class UCapsuleComponent;
class UPhysicsControlComponent;
class USkeletalMeshComponent;

UENUM(BlueprintType)
enum class EDeliveryRagdollControlMode : uint8
{
	Disabled,
	Active,
	Limp
};

USTRUCT()
struct FDeliveryRagdollBodyState
{
	GENERATED_BODY()

	UPROPERTY()
	FName Bone;

	UPROPERTY()
	FVector_NetQuantize10 Position;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY()
	FVector_NetQuantize10 LinearVelocity;

	UPROPERTY()
	FVector_NetQuantize10 AngularVelocity;
};

USTRUCT()
struct FDeliveryRagdollSnapshot
{
	GENERATED_BODY()

	UPROPERTY()
	uint16 Sequence = 0;

	UPROPERTY()
	float ServerTime = 0.0f;

	UPROPERTY()
	TArray<FDeliveryRagdollBodyState> Bodies;
};

USTRUCT(BlueprintType)
struct FDeliveryRagdollBones
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll")
	FName Hips = TEXT("Hips");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll")
	FName Spine = TEXT("Spine");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll")
	FName Head = TEXT("Head");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll")
	FName LeftUpLeg = TEXT("LeftUpLeg");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll")
	FName LeftFoot = TEXT("LeftFoot");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll")
	FName RightUpLeg = TEXT("RightUpLeg");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll")
	FName RightFoot = TEXT("RightFoot");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll")
	FName LeftArm = TEXT("LeftArm");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll")
	FName RightArm = TEXT("RightArm");

};

/**
 * 全身物理角色：网格刚体持续模拟，Physics Control 当作关节肌肉，用目标位置和目标旋转去拉身体。
 *
 * 走路不是动画位移，也不是 IK。髋先走、脚后追。问地的方向沿坡面法线，只处理较缓的斜面。
 * 陡坡、台阶和用手攀爬都不在这套范围内。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryActiveRagdollComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryActiveRagdollComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void RegisterComponentTickFunctions(bool bRegister) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Ragdoll")
	void StartRagdoll();

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Ragdoll")
	void StopRagdoll();

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Ragdoll")
	void SetLimp(bool bLimp);

	UFUNCTION(BlueprintCallable, Category="Ragdoll")
	void SetMoveInput(FVector2D RightForward);

	UFUNCTION(BlueprintCallable, Category="Ragdoll")
	void AddImpulse(FVector Impulse, bool bVelocityChange = true);

	/** 开始直拳：记录攻击方向和手侧。 */
	void BeginBodyDrivenPunch(FVector AimDirection, float HandSide);
	bool CanDrivePunch(float HandSide) const { return bIsActive && !bIsLimp && BoxingPose.IsReady(HandSide > 0 ? 0 : 1); }
	FVector GetBodyForward() const { return FRotator(0, CurrentFacingYaw, 0).Vector(); }

	/** 释放直拳：标记前送阶段。 */
	void ReleaseBodyDrivenPunch();

	/** 清除额外的出拳姿势，回到普通站立/移动控制。 */
	void EndBodyDrivenPunch();

	UFUNCTION(BlueprintPure, Category="Ragdoll")
	bool IsRagdollActive() const { return bIsActive; }

	UFUNCTION(BlueprintPure, Category="Ragdoll")
	EDeliveryRagdollControlMode GetControlMode() const { return ReplicatedControlMode; }

protected:

	UPROPERTY(VisibleAnywhere, Category="Ragdoll")
	TObjectPtr<UPhysicsControlComponent> PhysicsControl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll")
	FDeliveryRagdollBones Bones;

	UPROPERTY(EditAnywhere, Category="Ragdoll")
	bool bStartOnBeginPlay = true;

	UPROPERTY(EditAnywhere, Category="Ragdoll")
	bool bPlaceOnGroundAtStart = true;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0"))
	float DesiredMoveSpeed = 360.0f;

	/** 用速度误差估计髋部沿坡领前目标时，向前看多长时间。时间越长，目标离当前髋越远。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0"))
	float TargetLeadTime = 0.08f;

	/** 髋部领前目标相对当前髋的最大距离，避免电机一次性把身体拉得太远。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0"))
	float MaxTargetLead = 18.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0", ClampMax="20.0"))
	float AccelerationLeanAngle = 6.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.1"))
	float TurnResponsiveness = 5.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0", ClampMax="8.0"))
	float BouncyPelvisWobbleAngle = 3.5f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0", ClampMax="20.0"))
	float LooseTorsoSwingAngle = 14.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0", ClampMax="8.0"))
	float SmoothBounceHeight = 2.5f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.0"))
	float ControlledStrideLength = 108.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.0"))
	float StableComedyStance = 32.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.0"))
	float ControlledStepHeight = 38.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.0"))
	float StableMinimumFootSide = 16.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.0"))
	float MovingCrossingRecoveryMargin = 6.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.05"))
	float ControlledStrideDuration = 0.34f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.0"))
	float StopRecoveryDistance = 34.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.0", ClampMax="1.0"))
	float MinimumStepUprightDot = 0.62f;

	/**
	 * 允许沿坡面行走的最大坡角（度）。坡角是地面法线与世界向上的夹角：
	 * 法线竖直分量 = cos(坡角)。58 度时 cos(58°) ≈ 0.530。
	 * 原先用 0.82，对应 arccos(0.82) ≈ 35 度，较缓斜面也容易被判成走不了。
	 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.0", ClampMax="75.0"))
	float MaxWalkableSlopeDegrees = 58.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float RootLinearStrength = 4.6f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float StabilizedRootAngularStrength = 10.5f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float LooseWaistFollowStrength = 5.8f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float LooseComedyBodyStrength = 3.4f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float UprightHeadStrength = 11.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float StableHeadDampingRatio = 1.6f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|姿势", meta=(ClampMin="0.0", ClampMax="15.0"))
	float StandingHeadCorrectionAngle = 6.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float StableHipStrength = 6.5f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float ArticulatedKneeStrength = 6.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float ComedyArmStrength = 0.2f;

	/**
	 * 蓄力阶段把出拳侧的肩膀向后拧多少度。纯水平旋转，不弯腰。
	 * 这个角度和下面的跟随角度加起来就是身体在一次出拳里横转的总量。给大了，
	 * 伸出去的手臂会被整块躯干横着带过去，看起来是在扇耳光而不是出拳。
	 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|出拳", meta=(ClampMin="0.0", ClampMax="60.0"))
	float PunchSideStanceAngle = 12.0f;

	/** 释放时上身向出拳侧带出多少度。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|出拳", meta=(ClampMin="0.0", ClampMax="45.0"))
	float PunchFollowThroughAngle = 14.0f;

	/** 拧身角度的过渡速度。目标角度是阶跃的，这里决定身体多快跟上去。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|出拳", meta=(ClampMin="1.0"))
	float PunchTwistSpeed = 10.0f;

	/** 释放时全身沿拳路前送多远。直拳的力道主要来自这一下体重前压。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|出拳", meta=(ClampMin="0.0", ClampMax="60.0"))
	float PunchLungeDistance = 41.0f;

	/** 前送和收回的速度。要比拧身快，身体先压出去，拳头才跟着有重量。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|出拳", meta=(ClampMin="1.0"))
	float PunchLungeSpeed = 16.0f;

	/** 出拳期间胸腔和脊柱的强度。肩膀要有支点，上臂才转得动。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|出拳", meta=(ClampMin="0.0"))
	float PunchBraceStrength = 12.0f;

	/** 躯干绷紧和放松的过渡速度。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|出拳", meta=(ClampMin="1.0"))
	float PunchBraceSpeed = 8.0f;

	/** 手臂三个姿势的参数：站立走路的 A 姿势、出拳起手的收拳、直拳终点。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|出拳")
	FDeliveryArmPoseSettings ArmPose;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float LegPullStrength = 3.8f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float FootFacingStrength = 4.5f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float StableMuscleDampingRatio = 1.35f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|稳定", meta=(ClampMin="0.0"))
	float RigidBodyLinearDamping = 1.5f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|稳定", meta=(ClampMin="0.0"))
	float RigidBodyAngularDamping = 4.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|稳定", meta=(ClampMin="0.0", ClampMax="1.0"))
	float CenterOfMassCorrection = 0.8f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|稳定", meta=(ClampMin="0.0"))
	float MaxCenterOfMassCorrection = 14.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|稳定", meta=(ClampMin="0.0"))
	float BalanceResponseSpeed = 3.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|稳定", meta=(ClampMin="0.0"))
	float DriveTargetSmoothingSpeed = 10.0f;

	/** 坡面法线和贴地点的过渡速度。坡顶接到平面时若直接换法线，髋目标和旋转会一帧抽掉。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|稳定", meta=(ClampMin="0.0"))
	float GroundNormalSmoothingSpeed = 8.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|稳定", meta=(ClampMin="0.0", ClampMax="2.0"))
	float StartupFootPlantDuration = 0.3f;

	UPROPERTY(EditAnywhere, Category="Ragdoll")
	float CapsuleHipsZOffset = 0.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|镜头", meta=(ClampMin="0.0"))
	float CameraSmoothingSpeed = 7.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|网络", meta=(ClampMin="5.0", ClampMax="60.0"))
	float NetworkSnapshotRate = 20.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|网络", meta=(ClampMin="0.0"))
	float OwnerCorrectionSpeed = 5.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|网络", meta=(ClampMin="1.0"))
	float OwnerHardCorrectionDistance = 100.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|网络", meta=(ClampMin="0.0", ClampMax="0.5"))
	float MaxSnapshotExtrapolation = 0.1f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|网络", meta=(ClampMin="0.05", ClampMax="1.0"))
	float ServerInputTimeout = 0.25f;

	UPROPERTY(ReplicatedUsing=OnRep_ControlMode)
	EDeliveryRagdollControlMode ReplicatedControlMode = EDeliveryRagdollControlMode::Disabled;

	UPROPERTY(ReplicatedUsing=OnRep_RagdollSnapshot)
	FDeliveryRagdollSnapshot ReplicatedSnapshot;

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<UCapsuleComponent> Capsule;

	/** 地面探测结果：击中点和向上的坡面法线。 */
	struct FGroundHit
	{
		FVector Point = FVector::ZeroVector;
		FVector Normal = FVector::UpVector;
	};

	/** 一只脚的迈步状态。Alpha 小于 1 表示正在摆动，等于 1 表示落在地上做支撑。 */
	struct FFoot
	{
		FName Bone;
		FName Control;
		/** 抬脚瞬间的脚质心，摆动曲线的起点。 */
		FVector Start = FVector::ZeroVector;
		/** 这一步要落到的世界位置。 */
		FVector Target = FVector::ZeroVector;
		FVector SideAxis = FVector::RightVector;
		FQuat ReferenceRotation = FQuat::Identity;
		FQuat TargetRotation = FQuat::Identity;
		/** 启动时脚质心相对地面的高度，落到地面时加回去，避免脚埋进地里。 */
		float GroundOffset = 0.0f;
		/** 左脚为负、右脚为正，或按启动时相对髋的左右决定。 */
		float SideSign = 1.0f;
		float Alpha = 1.0f;
	};

	FActorComponentTickFunction PostPhysicsTickFunction;
	FDeliveryBoxingPose BoxingPose;
	FVector2D MoveInput = FVector2D::ZeroVector;
	FQuat ReferencePelvisRotation = FQuat::Identity;
	FQuat ReferenceSpineRelativeRotation = FQuat::Identity;
	FQuat ReferenceHeadRotation = FQuat::Identity;
	FQuat ReferenceLeftArmRelativeRotation = FQuat::Identity;
	FQuat ReferenceRightArmRelativeRotation = FQuat::Identity;
	FTransform InitialMeshRelativeTransform = FTransform::Identity;
	FVector LastWishDirection = FVector::ForwardVector;
	FVector SmoothedBalanceOffset = FVector::ZeroVector;
	FVector SmoothedMoveLead = FVector::ZeroVector;
	/** 本帧已经算好的髋部目标。脚的落点从这里在切平面上推出，再沿法线投到坡面上。 */
	FVector PlannedPelvisTarget = FVector::ZeroVector;
	FVector CurrentGroundNormal = FVector::UpVector;
	FVector SmoothedGroundPoint = FVector::ZeroVector;
	FVector WishOnSlope = FVector::ZeroVector;
	FVector UprightInPelvisSpace = FVector::UpVector;
	float SmoothedAccelerationAlpha = 0.0f;
	FFoot LeftFoot;
	FFoot RightFoot;
	FName PelvisControl;
	FName ChestControl;
	FName HeadControl;
	/** 胸腔以上的脊柱节。出拳时按名字绷紧，不要假定自定义 Set 名一定存在。 */
	TArray<FName> TorsoControls;
	float BraceAlpha = 0.0f;
	float AppliedBraceAlpha = -1.0f;
	/** 平滑后的拧身角度。发给电机的必须是这个，不是阶跃的目标角度。 */
	float PunchTwist = 0.0f;
	float PunchLunge = 0.0f;
	float StandHeight = 95.0f;
	float ReferenceFacingYaw = 0.0f;
	float CurrentFacingYaw = 0.0f;
	float StartupPlantRemaining = 0.0f;
	float SnapshotAccumulator = 0.0f;
	float SnapshotReceivedAt = 0.0f;
	float LastMoveInputTime = 0.0f;
	FDeliveryRagdollSnapshot PreviousSnapshot;
	FDeliveryRagdollSnapshot TargetSnapshot;
	bool bHasNetworkSnapshot = false;
	bool bWasMoving = false;
	bool bPendingStopRecovery = false;
	bool bStepLeftNext = true;
	bool bIsActive = false;
	bool bIsLimp = false;
	bool bBodyDrivenPunchActive = false;
	bool bBodyDrivenPunchReleased = false;
	float PunchHandSide = 1.0f;
	FVector PunchAimDirection = FVector::ForwardVector;

	UFUNCTION()
	void OnRep_ControlMode();

	UFUNCTION()
	void OnRep_RagdollSnapshot();

	void ResolveOwnerComponents();
	bool ValidateSetup() const;
	void PlaceOnGround();
	void ConfigurePhysics();
	bool CreateControls();
	void DestroyControls();
	void CacheStandingState();
	void UpdateControlTargets(float DeltaTime);
	void BraceTorsoForPunch(float DeltaTime, bool bBrace);
	void UpdatePelvisTarget(float DeltaTime, const FVector& Wish);
	void UpdateFeet(float DeltaTime, const FVector& Wish);
	bool BeginStep(FFoot& Foot, const FVector& Wish);
	void UpdateFootTarget(FFoot& Foot, float DeltaTime);
	bool PlanFootLanding(FFoot& Foot, const FVector& Wish);
	FVector GetSlopeForward(const FVector& Wish) const;
	FVector GetSlopeRight(const FVector& Wish) const;
	FQuat MakeSlopeAlignedFootRotation(const FFoot& Foot, const FVector& SlopeForward) const;
	bool SampleGround(const FVector& Planned, const FVector& Fallback, const FVector& AlongNormal, FGroundHit& OutHit) const;
	FVector GetWholeBodyCenterOfMass() const;
	float GetUprightDot() const;
	bool TraceGround(const FVector& Around, const FVector& AlongNormal, FGroundHit& OutHit) const;
	FVector GetWishDir() const;
	float GetAimYaw() const;
	void SyncOwnerToPelvis(float DeltaTime);
	void CaptureNetworkSnapshot();
	void ApplyNetworkSnapshot(float DeltaTime);
};
