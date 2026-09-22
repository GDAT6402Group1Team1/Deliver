#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/NetSerialization.h"
#include "DeliveryGrabComponent.generated.h"

class ADeliveryCharacter;
class UDeliveryGrabbableComponent;
class UPhysicsConstraintComponent;
class UPrimitiveComponent;

/** Props use server-positioned carry; stunned characters retain server-owned hand constraints. */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryGrabComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UDeliveryGrabComponent();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	void RequestBegin();
	void RequestReleaseHand(bool bLeft);
	void ForceRelease(); // Server only; also called when a grabbed player wakes.
	bool IsGrabbing() const { return (GrabTarget && (WantedHands & ~LocallyReleasedHands)) || PredictedHands != 0; }
	bool IsCarryingProp() const;
	bool IsDraggingCharacter() const;
	bool GetHandGoal(bool bLeft, FVector& OutGoal) const;
	bool GetGrabFacingDirection(FVector& OutDirection) const;
	FVector FilterApproachWish(const FVector& Wish) const;
	bool GetCarryPoseFor(const AActor* Target, FVector& OutCenter, FVector& OutForward) const;
#if !UE_BUILD_SHIPPING
	/** PIE 诊断：与正常抓取使用同一个候选查询。 */
	bool DebugFindCandidate(AActor*& OutActor, FVector& OutPoint) const { return FindCandidate(OutActor, OutPoint); }
#endif

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Grab")
	TObjectPtr<AActor> GrabTarget;

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Grab")
	uint8 AttachedHands = 0;
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category="Grab|Debug")
	float LeftHandGap = 0.0f;
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category="Grab|Debug")
	float RightHandGap = 0.0f;

protected:
	UPROPERTY(EditAnywhere, Category="Grab", meta=(ClampMin="0.0"))
	float GrabDistance = 140.0f;
	UPROPERTY(EditAnywhere, Category="Grab", meta=(ClampMin="0.0"))
	float HighlightAimMargin = 25.0f;
	UPROPERTY(EditAnywhere, Category="Grab", meta=(ClampMin="0.0"))
	float AttachDistance = 18.0f;
	UPROPERTY(EditAnywhere, Category="Grab|Drag", meta=(ClampMin="0.0"))
	float DragAttachDistance = 38.0f;
	/** 第二人争抢时不能瞬移已经被拖住的人，改用有力上限的弹簧靠近。 */
	UPROPERTY(EditAnywhere, Category="Grab|Drag", meta=(ClampMin="0.0"))
	float DragReelStrength = 1200.0f;
	UPROPERTY(EditAnywhere, Category="Grab|Drag", meta=(ClampMin="0.0"))
	float DragReelDamping = 500.0f;
	UPROPERTY(EditAnywhere, Category="Grab|Drag", meta=(ClampMin="0.0"))
	float DragReelForceLimit = 30000.0f;
	UPROPERTY(EditAnywhere, Category="Grab|Drag", meta=(ClampMin="0.0"))
	float DragBreakDistance = 260.0f;
	/** Top speed of the planar drag assist; must exceed the grabber's 360 cm/s walking speed. */
	UPROPERTY(EditAnywhere, Category="Grab|Drag", meta=(ClampMin="0.0"))
	float DragFollowSpeed = 480.0f;
	UPROPERTY(EditAnywhere, Category="Grab|Drag", meta=(ClampMin="0.0"))
	float DragFollowAcceleration = 3200.0f;
	UPROPERTY(EditAnywhere, Category="Grab", meta=(ClampMin="0.0"))
	float MaxHandSeparation = 150.0f;
	UPROPERTY(EditAnywhere, Category="Grab|Carry", meta=(ClampMin="0.0"))
	float CarryHeightAboveSpine = 0.0f;
	UPROPERTY(EditAnywhere, Category="Grab|Carry", meta=(ClampMin="0.0"))
	float CarryHandDistance = 48.0f;
	UPROPERTY(EditAnywhere, Category="Grab|Reach", meta=(ClampMin="0.0"))
	float ApproachSlowDistance = 140.0f;
	UPROPERTY(EditAnywhere, Category="Grab|Reach", meta=(ClampMin="0.0"))
	float ApproachStopDistance = 55.0f;

private:
	UFUNCTION(Server, Reliable)
	void ServerRequestBegin(AActor* Target, FVector_NetQuantize10 HitPoint);
	UFUNCTION(Server, Reliable)
	void ServerReleaseHand(bool bLeft);

	bool FindCandidate(AActor*& OutActor, FVector& OutPoint) const;
	void BeginOnServer(AActor* Target, const FVector& HitPoint);
	void ReleaseHandOnServer(int32 Side);
	void UpdateHighlight(AActor* Candidate);
	void ClearHighlight();
	FVector GripWorld(int32 Side) const;
	void FindUndersideGripPoints(const UPrimitiveComponent* Body, const FVector& Forward,
		FVector& OutLeft, FVector& OutRight) const;
	bool FindClosestDragGrip(const ADeliveryCharacter* Target, int32& OutSide,
		FName& OutBone, FVector& OutPoint) const;
	FVector CarryCenterWorld(const UPrimitiveComponent* Body) const;
	UPrimitiveComponent* GetTargetBody() const;
	void TryAttach(int32 Side);

	UPROPERTY(Replicated)
	FName GrabBone;
	UPROPERTY(Replicated)
	FVector_NetQuantize10 GripLocalLeft;
	UPROPERTY(Replicated)
	FVector_NetQuantize10 GripLocalRight;
	UPROPERTY(Replicated)
	uint8 WantedHands = 0;
	UPROPERTY(Replicated)
	FVector_NetQuantizeNormal CarryForward = FVector::ForwardVector;

	UPROPERTY(Transient)
	TObjectPtr<UPhysicsConstraintComponent> HandConstraints[2];
	TWeakObjectPtr<AActor> PredictedTarget;
	FVector PredictedGripPoint = FVector::ZeroVector;
	uint8 PredictedHands = 0;
	uint8 LocallyReleasedHands = 0;
	float PredictionExpiresAt = 0.0f;
	bool bVerticalFree[2] = { false, false };
	bool bDragReeling[2] = { false, false };
	TWeakObjectPtr<UPrimitiveComponent> HighlightedBody;
	bool bOldCustomDepth = false;
	int32 OldStencil = 0;
};
