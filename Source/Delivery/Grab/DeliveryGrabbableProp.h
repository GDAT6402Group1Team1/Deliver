#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeliveryGrabbableProp.generated.h"

class UStaticMeshComponent;
class UDeliveryGrabbableComponent;

/** Simple physics prop base; replace the cube mesh in a Blueprint child for game items. */
UCLASS(Blueprintable)
class DELIVERY_API ADeliveryGrabbableProp : public AActor
{
	GENERATED_BODY()

public:
	ADeliveryGrabbableProp();
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Grab")
	TObjectPtr<UStaticMeshComponent> PhysicsMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Grab")
	TObjectPtr<UDeliveryGrabbableComponent> Grabbable;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Grab", meta=(ClampMin="0.1"))
	float MassKg = 3.0f;
};
