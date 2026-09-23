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

	/** 迈步落点刚体。物理资产没有脚时，启动会改成该腿最远端的现有刚体（通常是小腿）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll")
	FName LeftFoot = TEXT("LeftFoot");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ragdoll")
	FName RightUpLeg = TEXT("RightUpLeg");

	/** 迈步落点刚体。物理资产没有脚时，启动会改成该腿最远端的现有刚体（通常是小腿）。 */
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
 * 陡坡改由服务器触发短暂的全物理翻滚；台阶和用手攀爬不在步态范围内。
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

	/** 仅服务器在确认受到一拳后调用；受击者自己承受冲量并短暂放松直立控制。 */
	void ApplyMeleeImpact(const FVector& Impulse, const FVector& ImpactPoint);

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

	/** 盆骨是否贴在站立高度附近。跳跃过程中恒为 false，二段跳就是被这里挡住的。 */
	UFUNCTION(BlueprintPure, Category="Ragdoll|跳跃")
	bool IsGrounded() const;

	/** 起跳。不在地上、正在跳、或处于瘫软状态时返回 false，不做任何事。 */
	UFUNCTION(BlueprintCallable, Category="Ragdoll|跳跃")
	bool TryStartJump();

	UFUNCTION(BlueprintPure, Category="Ragdoll|跳跃")
	bool IsJumping() const { return bJumping; }

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
	float AccelerationLeanAngle = 4.5f;

	/** 起步时短暂前倾；不要按当前速度误差一直前倾，否则下坡失速会形成前扑的正反馈。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0"))
	float AccelerationLeanDuration = 0.4f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.1"))
	float TurnResponsiveness = 5.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0", ClampMax="8.0"))
	float BouncyPelvisWobbleAngle = 2.2f;

	/** 拖拽倒地角色时稍微屈膝、前倾，让单手能接近地面的身体；不影响普通物品托举。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|抓取", meta=(ClampMin="0.0", ClampMax="45.0"))
	float DragReachCrouchHeight = 25.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|抓取", meta=(ClampMin="0.0", ClampMax="20.0"))
	float DragReachLeanAngle = 8.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0", ClampMax="20.0"))
	float LooseTorsoSwingAngle = 7.5f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0", ClampMax="8.0"))
	float SmoothBounceHeight = 1.8f;

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
	float MinimumStepUprightDot = 0.35f;

	/**
	 * 允许沿坡面行走的最大坡角（度）。坡角是地面法线与世界向上的夹角：
	 * 法线竖直分量 = cos(坡角)。58 度时 cos(58°) ≈ 0.530。
	 * 原先用 0.82，对应 arccos(0.82) ≈ 35 度，较缓斜面也容易被判成走不了。
	 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.0", ClampMax="75.0"))
	float MaxWalkableSlopeDegrees = 58.0f;

	/** 顺坡移动超过此坡角时，不再强迫脚迈步，而是进入物理翻滚。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|陡坡翻滚", meta=(ClampMin="20.0", ClampMax="75.0"))
	float TumbleSlopeDegrees = 48.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|陡坡翻滚", meta=(ClampMin="0.0"))
	float TumbleDownhillSpeed = 120.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|陡坡翻滚", meta=(ClampMin="0.0"))
	float TumbleEntryDelay = 0.18f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|陡坡翻滚", meta=(ClampMin="0.0", ClampMax="60.0"))
	float TumbleRecoverySlopeDegrees = 30.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|陡坡翻滚", meta=(ClampMin="0.0"))
	float TumbleRecoverySpeed = 140.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|陡坡翻滚", meta=(ClampMin="0.0"))
	float TumbleMinimumDuration = 0.9f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|陡坡翻滚", meta=(ClampMin="0.0"))
	float TumbleRecoveryDelay = 0.45f;

	/** 起滚时给胸部一次轻微角速度；之后只靠重力和碰撞滚动。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|陡坡翻滚", meta=(ClampMin="0.0"))
	float TumbleStartAngularSpeed = 2.0f;

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
	float ComedyArmStrength = 3.0f;

	/** 受击后重点减弱胸和躯干电机；髋、腿只轻微让位。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|受击", meta=(ClampMin="0.0", ClampMax="1.0"))
	float HitReactionStrengthMultiplier = 0.2f;

	/** 受击时髋部电机保留的强度；高于上半身，避免整个人飞走。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|受击", meta=(ClampMin="0.0", ClampMax="1.0"))
	float HitReactionPelvisStrengthMultiplier = 0.85f;

	/** 两脚仍盯住落点，但位置电机可稍微让位。给小一点，脚会被拖着走得更多。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|受击", meta=(ClampMin="0.0", ClampMax="1.0"))
	float HitReactionFootStrengthMultiplier = 0.45f;

	/** 受击时两脚沿被打飞的方向挪多远，做出踉跄一步。0 就是原地钉住。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|受击", meta=(ClampMin="0.0", ClampMax="200.0"))
	float HitReactionFootSlideDistance = 34.0f;

	/** 受击时髋部目标沿被打方向额外挪多远，让整个人真的被打退一步，不只是上身晃。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|受击", meta=(ClampMin="0.0", ClampMax="200.0"))
	float HitReactionPelvisSlideDistance = 30.0f;

	/** 髋部挪出去/收回来的过渡速度。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|受击", meta=(ClampMin="1.0"))
	float HitReactionPelvisSlideSpeed = 10.0f;

	/** 胸部冲量中额外传给髋的比例，制造很短的下身后移。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|受击", meta=(ClampMin="0.0", ClampMax="1.0"))
	float HitReactionPelvisImpulseFraction = 0.12f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|受击", meta=(ClampMin="0.0"))
	float HitReactionDuration = 0.4f;

	/** 胸部绕髋部后仰的瞬时角速度变化（弧度/秒）。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|受击", meta=(ClampMin="0.0"))
	float HitReactionAngularVelocity = 5.5f;

	/**
	 * 额外给胸部一个纯向上的冲量，大小是水平冲量的这个倍数。挨打这段时间上半身电机很松，
	 * 这一下会把上身弹起来，靠重力自己落回去，看起来是上半身带着一跳，而不只是往后倒。
	 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|受击", meta=(ClampMin="0.0", ClampMax="3.0"))
	float HitReactionUpwardImpulseFraction = 0.5f;

	/** 受击后转身面向来拳方向的速度。比平时转身快一点，挨打是被动的反射。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|受击", meta=(ClampMin="0.0"))
	float HitReactionFacingTurnSpeed = 9.0f;

	/**
	 * 蓄力阶段把出拳侧的肩膀向后拧多少度。纯水平旋转，不弯腰。
	 * 这个角度和下面的跟随角度加起来就是身体在一次出拳里横转的总量。给大了，
	 * 伸出去的手臂会被整块躯干横着带过去，看起来是在扇耳光而不是出拳。
	 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|出拳", meta=(ClampMin="0.0", ClampMax="60.0"))
	float PunchSideStanceAngle = 8.0f;

	/** 释放时上身向出拳侧带出多少度。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|出拳", meta=(ClampMin="0.0", ClampMax="45.0"))
	float PunchFollowThroughAngle = 9.0f;

	/** 拧身角度的过渡速度。目标角度是阶跃的，这里决定身体多快跟上去。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|出拳", meta=(ClampMin="1.0"))
	float PunchTwistSpeed = 10.0f;

	/** 释放时全身沿拳路前送多远。直拳的力道主要来自这一下体重前压。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|出拳", meta=(ClampMin="0.0", ClampMax="60.0"))
	float PunchLungeDistance = 24.0f;

	/** 前送和收回的速度。要比拧身快，身体先压出去，拳头才跟着有重量。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|出拳", meta=(ClampMin="1.0"))
	float PunchLungeSpeed = 12.0f;

	/** 出拳期间胸腔和脊柱的强度。肩膀要有支点，上臂才转得动。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|出拳", meta=(ClampMin="0.0"))
	float PunchBraceStrength = 12.0f;

	/** 躯干绷紧和放松的过渡速度。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|出拳", meta=(ClampMin="1.0"))
	float PunchBraceSpeed = 8.0f;

	/** 托举时胸和脊柱提供肩膀支点；只在持物期间渐进提高，不锁住腿。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|抓取", meta=(ClampMin="0.0"))
	float CarryBraceStrength = 20.0f;

	/** 抱箱转身时限制目标身体角速度，避免手臂被镜头瞬间转向扭到背后。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|抓取", meta=(ClampMin="1.0"))
	float CarryTurnRate = 160.0f;

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

	/** 跳跃最高点相对站立高度的抬升量。默认约为角色身高的一半。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|跳跃", meta=(ClampMin="0.0"))
	float JumpHeight = 90.0f;

	/** 一次跳跃从离地到髋目标回到站立高度的总时长。没有前摇，按下即开始。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|跳跃", meta=(ClampMin="0.05"))
	float JumpDuration = 0.55f;

	/** 落地判定容差，按站立高度的比例算。髋部低于 StandHeight*(1+该值) 就算站在地上。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|跳跃", meta=(ClampMin="0.0", ClampMax="1.0"))
	float GroundedHeightTolerance = 0.35f;

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
		/** 位置电机拉的刚体。没有脚刚体时是小腿。 */
		FName Bone;
		/** 用来量贴地的骨骼，通常是脚尖；可以没有刚体。 */
		FName SoleBone;
		FName Control;
		/** 抬脚瞬间的脚质心，摆动曲线的起点。 */
		FVector Start = FVector::ZeroVector;
		/** 这一步要落到的世界位置。 */
		FVector Target = FVector::ZeroVector;
		FVector SideAxis = FVector::RightVector;
		FQuat ReferenceRotation = FQuat::Identity;
		FQuat TargetRotation = FQuat::Identity;
		/** 刚体质心比脚底高出多少。落点按这个抬，让脚底着地，而不是把小腿质心按到地面。 */
		float GroundOffset = 0.0f;
		/** 左脚为负、右脚为正，或按启动时相对髋的左右决定。 */
		float SideSign = 1.0f;
		float Alpha = 1.0f;
		float Elapsed = 0.0f;
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
	bool bPelvisAirborne = false;
	FVector WishOnSlope = FVector::ZeroVector;
	FVector UprightInPelvisSpace = FVector::UpVector;
	float SmoothedAccelerationAlpha = 0.0f;
	float DragReachAlpha = 0.0f;
	float AccelerationLeanRemaining = 0.0f;
	bool bHadMoveWishLastTick = false;
	FFoot LeftFoot;
	FFoot RightFoot;
	FName PelvisControl;
	FName ChestControl;
	FName HeadControl;
	/** 胸腔以上的脊柱节。出拳时按名字绷紧，不要假定自定义 Set 名一定存在。 */
	TArray<FName> TorsoControls;
	float BraceAlpha = 0.0f;
	float AppliedBraceAlpha = -1.0f;
	float AppliedBraceTargetStrength = -1.0f;
	/** 平滑后的拧身角度。发给电机的必须是这个，不是阶跃的目标角度。 */
	float PunchTwist = 0.0f;
	float PunchLunge = 0.0f;
	/** 受击后退平滑值，0 到 1，驱动髋部目标沿 HitPushDirection 的偏移量。 */
	float HitPushAlpha = 0.0f;
	float StandHeight = 95.0f;
	/** 本次跳跃已经过去的时间，超过 JumpDuration 即落地。 */
	float JumpElapsed = 0.0f;
	/** 本帧要叠加到髋部目标上的抬升量，沿地面法线方向。 */
	float JumpOffset = 0.0f;
	float ReferenceFacingYaw = 0.0f;
	/** Stable physical-body facing. Simulated proxies need this for held-item presentation. */
	UPROPERTY(Replicated)
	float CurrentFacingYaw = 0.0f;
	float StartupPlantRemaining = 0.0f;
	float SnapshotAccumulator = 0.0f;
	float SnapshotReceivedAt = 0.0f;
	float LastMoveInputTime = 0.0f;
	float HitReactionEndTime = 0.0f;
	/** 被打飞的水平方向（单位向量）。脚沿它挪，朝向取它的反向。 */
	FVector HitPushDirection = FVector::ZeroVector;
	/** 受击期间身体要转过去的朝向：面对出拳的人。 */
	float HitFacingYaw = 0.0f;
	bool bHasHitFacing = false;
	bool bHitFeetPlanted = false;
	FDeliveryRagdollSnapshot PreviousSnapshot;
	FDeliveryRagdollSnapshot TargetSnapshot;
	bool bHasNetworkSnapshot = false;
	bool bWasMoving = false;
	bool bPendingStopRecovery = false;
	bool bStepLeftNext = true;
	bool bIsActive = false;
	bool bIsLimp = false;
	bool bExternalLimpRequested = false;
	bool bSlopeTumbling = false;
	float TumbleEntryTime = 0.0f;
	float TumbleElapsed = 0.0f;
	float TumbleRecoveryTime = 0.0f;
	float TumbleCooldownTime = 0.0f;
	bool bJumping = false;
	bool bBodyDrivenPunchActive = false;
	bool bBodyDrivenPunchReleased = false;
	float PunchHandSide = 1.0f;
	FVector PunchAimDirection = FVector::ForwardVector;

	UFUNCTION()
	void OnRep_ControlMode();

	UFUNCTION()
	void OnRep_RagdollSnapshot();

	void ResolveOwnerComponents();
	void ResolveConfiguredPhysicsBones();
	bool ValidateSetup() const;
	void PlaceOnGround();
	/** 取消正在进行的迈步，并关掉两只脚的世界空间位置电机。起跳和落地各调一次。 */
	void CancelFootSteps();
	/**
	 * 按身体当前所在的位置重新播种髋目标、贴地点和步态状态。
	 * 从 Limp 恢复时必须先调它：控制器里存的还是倒下前那一刻的目标，
	 * 直接开电机会把人拽回旧位置。不动 StandHeight，人躺着时那个值算出来是错的。
	 */
	void ReseedFromCurrentPose();
	void ConfigurePhysics();
	bool CreateControls();
	void DestroyControls();
	void CacheStandingState();
	void UpdateControlTargets(float DeltaTime);
	void SetHitReactionStrength(float Multiplier);
	void SetHitFeetPlanted(bool bPlant);
	void BraceTorsoForAction(float DeltaTime, bool bPunch, bool bCarry);
	void UpdatePelvisTarget(float DeltaTime, const FVector& Wish);
	void UpdateFeet(float DeltaTime, const FVector& Wish);
	void KeepFeetOnOwnSide();
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
	bool TraceTumbleSurface(FGroundHit& OutHit) const;
	void UpdateSlopeTumble(float DeltaTime);
	void ApplyLimpState(bool bLimp);
	FVector GetWishDir() const;
	float GetAimYaw() const;
	void SyncOwnerToPelvis(float DeltaTime);
	void CaptureNetworkSnapshot();
	void ApplyNetworkSnapshot(float DeltaTime);
};
