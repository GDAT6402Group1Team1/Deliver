// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryActiveRagdollComponent.generated.h"

class UCapsuleComponent;
class UPhysicsControlComponent;
class USkeletalMeshComponent;

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
 * 全身物理角色。Physics Control 提供肌肉，骨盆和脚目标负责平衡与移动；
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

	UFUNCTION(BlueprintCallable, Category="Ragdoll")
	void StartRagdoll();

	UFUNCTION(BlueprintCallable, Category="Ragdoll")
	void StopRagdoll();

	UFUNCTION(BlueprintCallable, Category="Ragdoll")
	void SetLimp(bool bLimp);

	UFUNCTION(BlueprintCallable, Category="Ragdoll")
	void SetMoveInput(FVector2D RightForward);

	UFUNCTION(BlueprintCallable, Category="Ragdoll")
	void AddImpulse(FVector Impulse, bool bVelocityChange = true);

	UFUNCTION(BlueprintPure, Category="Ragdoll")
	bool IsRagdollActive() const { return bIsActive; }

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
	float DesiredMoveSpeed = 240.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|移动", meta=(ClampMin="0.0"))
	float TargetLeadTime = 0.08f;

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
	float ControlledStrideLength = 86.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.0"))
	float StableComedyStance = 32.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.0"))
	float ControlledStepHeight = 38.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.0"))
	float StableMinimumFootSide = 16.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.0"))
	float MovingCrossingRecoveryMargin = 6.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.0"))
	float LandingClearance = 4.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|步态", meta=(ClampMin="0.05"))
	float ControlledStrideDuration = 0.42f;

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
	float StableHipStrength = 5.5f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float ArticulatedKneeStrength = 3.2f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float ComedyArmStrength = 0.2f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float LegPullStrength = 3.8f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|肌肉", meta=(ClampMin="0.0"))
	float FootFacingStrength = 2.5f;

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
	float StartupFootPlantDuration = 0.65f;

	UPROPERTY(EditAnywhere, Category="Ragdoll")
	float CapsuleHipsZOffset = 0.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|镜头", meta=(ClampMin="0.0"))
	float CameraSmoothingSpeed = 7.0f;

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<UCapsuleComponent> Capsule;

	struct FFoot
	{
		FName Bone;
		FName Control;
		FVector Start = FVector::ZeroVector;
		FVector Target = FVector::ZeroVector;
		FVector SideAxis = FVector::RightVector;
		FQuat ReferenceRotation = FQuat::Identity;
		FQuat TargetRotation = FQuat::Identity;
		float GroundOffset = 0.0f;
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
	bool bStepLeftNext = true;
	bool bIsActive = false;
	bool bIsLimp = false;

	void ResolveOwnerComponents();
	bool ValidateSetup() const;
	void PlaceOnGround();
	void ConfigurePhysics();
	bool CreateControls();
	void DestroyControls();
	void CacheStandingState();
	void UpdateControlTargets(float DeltaTime);
	void UpdatePelvisTarget(float DeltaTime, const FVector& Wish);
	void UpdateFeet(float DeltaTime, const FVector& Wish);
	void BeginStep(FFoot& Foot, const FVector& Wish);
	void UpdateFootTarget(FFoot& Foot, float DeltaTime);
	FVector GetWholeBodyCenterOfMass() const;
	float GetUprightDot() const;
	bool TraceGround(const FVector& Around, FVector& GroundPoint) const;
	FVector GetWishDir() const;
	float GetAimYaw() const;
	void SyncOwnerToPelvis(float DeltaTime);
};
