// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeliveryBreakableDoor.generated.h"

class UGeometryCollectionComponent;
class UStaticMeshComponent;

/** A server-authoritative door that swaps to fractured pieces when struck by an axe. */
UCLASS()
class DELIVERY_API ADeliveryBreakableDoor : public AActor
{
	GENERATED_BODY()

public:
	ADeliveryBreakableDoor();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Called only by the server after validating the equipped axe and its swing. */
	bool BreakFromAxe();
	bool IsBroken() const { return bBroken; }
	UStaticMeshComponent* GetIntactMesh() const { return IntactDoor; }
	UGeometryCollectionComponent* GetFragments() const { return Fragments; }

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Door")
	TObjectPtr<UStaticMeshComponent> IntactDoor;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Door")
	TObjectPtr<UGeometryCollectionComponent> Fragments;

	UPROPERTY(ReplicatedUsing=OnRep_Broken, VisibleAnywhere, BlueprintReadOnly, Category="Door")
	bool bBroken = false;

	UFUNCTION()
	void OnRep_Broken();
};
