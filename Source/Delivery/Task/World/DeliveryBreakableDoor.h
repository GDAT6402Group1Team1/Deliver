// 同步门体破坏状态，并切换完整模型与碎片表现
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeliveryBreakableDoor.generated.h"

class UGeometryCollectionComponent;
class UStaticMeshComponent;

/** 由服务器确认破门，并将门的完整模型切换为碎片 */
UCLASS()

class DELIVERY_API ADeliveryBreakableDoor : public AActor
{
	GENERATED_BODY()

  public:
	ADeliveryBreakableDoor();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 服务器破门与状态查询
	/** 服务器确认角色持有斧头且正在挥动后调用 */
	bool BreakFromAxe();

	bool IsBroken() const
	{
		return bBroken;
	}

	UStaticMeshComponent* GetIntactMesh() const
	{
		return IntactDoor;
	}

	UGeometryCollectionComponent* GetFragments() const
	{
		return Fragments;
	}

  protected:
	// 完整门体与破碎模型
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Door")
	TObjectPtr<UStaticMeshComponent> IntactDoor;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Door")
	TObjectPtr<UGeometryCollectionComponent> Fragments;

	// 破门状态同步
	UPROPERTY(ReplicatedUsing = OnRep_Broken, VisibleAnywhere, BlueprintReadOnly, Category = "Door")
	bool bBroken = false;

	UFUNCTION()
	void OnRep_Broken();
};
