// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/NetSerialization.h"
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
 * 走路不是动画位移，也不是 IK 反解腿部角度。每一帧先根据水平移动意图算出髋部接下来要去的水平位置，
 * 再在那个位置竖直向下探测地面，把探测到的高度加上站立身高，得到髋部的完整目标；电机把髋刚体拉向这个目标。
 * 脚的落点从同一个髋部目标推出来：一只脚留在地上做支撑，另一只脚沿一条抬起再落下的曲线迈向新落点，
 * 只有迈步中的那只脚打开世界空间位置电机。胸和头跟随启动时记下的站立姿势；大腿、小腿、脚踝和手臂
 * 跟随当前骨骼动画的旋转，强度较低，所以碰撞和外力仍然能把肢体带走。物理资产上的关节限位始终限制活动范围。
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
	float DesiredMoveSpeed = 300.0f;

	/** 用速度误差估计髋部水平目标时，向前看多长时间。时间越长，目标离当前髋越远。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0"))
	float TargetLeadTime = 0.08f;

	/** 髋部水平目标相对当前髋的最大距离，避免电机一次性把身体拉得太远。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0"))
	float MaxTargetLead = 18.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0", ClampMax="20.0"))
	float AccelerationLeanAngle = 4.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.1"))
	float TurnResponsiveness = 5.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0", ClampMax="8.0"))
	float BouncyPelvisWobbleAngle = 3.5f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0", ClampMax="20.0"))
	float LooseTorsoSwingAngle = 14.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0", ClampMax="8.0"))
	float SmoothBounceHeight = 2.5f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.0"))
	float ControlledStrideLength = 96.0f;

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

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float RootLinearStrength = 4.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float StabilizedRootAngularStrength = 9.0f;

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
	FVector2D MoveInput = FVector2D::ZeroVector;
	FQuat ReferencePelvisRotation = FQuat::Identity;
	FQuat ReferenceSpineRelativeRotation = FQuat::Identity;
	FQuat ReferenceHeadRotation = FQuat::Identity;
	FTransform InitialMeshRelativeTransform = FTransform::Identity;
	FVector LastWishDirection = FVector::ForwardVector;
	FVector SmoothedBalanceOffset = FVector::ZeroVector;
	FVector SmoothedMoveLead = FVector::ZeroVector;
	/** 本帧已经算好的髋部目标，脚的落点从这里推出，不再使用当前髋骨骼位置另算一套。 */
	FVector PlannedPelvisTarget = FVector::ZeroVector;
	FVector UprightInPelvisSpace = FVector::UpVector;
	float SmoothedAccelerationAlpha = 0.0f;
	FFoot LeftFoot;
	FFoot RightFoot;
	FName PelvisControl;
	FName ChestControl;
	FName HeadControl;
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
	/** 先更新髋部目标，再更新脚。脚的落点使用本帧已经写好的 PlannedPelvisTarget。 */
	void UpdateControlTargets(float DeltaTime);
	/** 根据水平移动意图写出髋部目标：先确定水平位置，再在该点探测地面高度。 */
	void UpdatePelvisTarget(float DeltaTime, const FVector& Wish);
	/** 选择支撑脚和摆动脚，并把摆动脚的世界空间位置电机目标沿抬脚曲线推进。 */
	void UpdateFeet(float DeltaTime, const FVector& Wish);
	/** 从 PlannedPelvisTarget 计算落点。成功后打开该脚的世界空间位置电机。 */
	bool BeginStep(FFoot& Foot, const FVector& Wish);
	/** 把摆动脚从起点插值到落点。曲线只描述路径，不改变已经定好的落点。 */
	void UpdateFootTarget(FFoot& Foot, float DeltaTime);
	/** 先在规划位置探测地面；没有碰到地面时，改在当前髋下方再探测一次。 */
	bool ResolveGroundHeight(const FVector& PlannedHorizontal, const FVector& Fallback, FVector& GroundPoint) const;
	FVector GetWholeBodyCenterOfMass() const;
	float GetUprightDot() const;
	bool TraceGround(const FVector& Around, FVector& GroundPoint) const;
	FVector GetWishDir() const;
	float GetAimYaw() const;
	void SyncOwnerToPelvis(float DeltaTime);
	void CaptureNetworkSnapshot();
	void ApplyNetworkSnapshot(float DeltaTime);
};
