#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryGrabbableComponent.generated.h"

class ADeliveryCharacter;
class UPrimitiveComponent;

/** Explicit opt-in for a simulated prop or a stunned player to be grabbed. */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryGrabbableComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UDeliveryGrabbableComponent();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Optional component name for props with more than one primitive. Defaults to a simulating primitive. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Grab")
	FName BodyComponentName;

	UPROPERTY(Replicated, BlueprintReadOnly, Category="Grab")
	int32 GrabberCount = 0;
	/** Ordinary props are server-positioned while carried; stunned characters remain physical. */
	UPROPERTY(ReplicatedUsing=OnRep_KinematicCarry, BlueprintReadOnly, Category="Grab")
	bool bKinematicCarry = false;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Grab|Carry", meta=(ClampMin="0.1"))
	float CarryFollowSpeed = 20.0f;

	UPrimitiveComponent* GetGrabBody(FName& OutBone) const;
	bool CanGrab(const ADeliveryCharacter* Requester) const;
	bool RegisterGrabber(ADeliveryCharacter* Requester);
	void UnregisterGrabber(ADeliveryCharacter* Requester);
	void ReleaseAllGrabbers();

private:
	UFUNCTION()
	void OnRep_KinematicCarry();
	void ApplyCarryMode();
	TArray<TWeakObjectPtr<ADeliveryCharacter>> Grabbers;
	TEnumAsByte<ECollisionResponse> SavedPawnResponse = ECR_Block;
	TEnumAsByte<ECollisionResponse> SavedPhysicsBodyResponse = ECR_Block;
	bool bHasSavedPawnResponse = false;
	FVector PreviousCarryLocation = FVector::ZeroVector;
	FVector LastCarryVelocity = FVector::ZeroVector;
};
