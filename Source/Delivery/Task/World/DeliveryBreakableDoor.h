// 同步门体破坏状态，并切换完整模型与碎片表现
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeliveryBreakableDoor.generated.h"

class UGeometryCollectionComponent;
class UStaticMeshComponent;
class USoundBase;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDeliveryDoorBroken);

/** 由服务器确认破门，并将门的完整模型切换为碎片 */
UCLASS()

class DELIVERY_API ADeliveryBreakableDoor : public AActor
{
	GENERATED_BODY()

  public:
	ADeliveryBreakableDoor();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

    /** 服务器挥斧窗口调用；同一次挥动的去重由布娃娃组件负责 */
    bool ReceiveAxeHit(APawn* Attacker, const FVector& HitPoint);
    int32 GetHitCount() const { return HitCount; }
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Door|Impact", meta=(ClampMin="1", ClampMax="10"))
    int32 HitsToBreak = 3;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Door|Impact")
    TObjectPtr<USoundBase> HitSound;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Door|Impact")
    TObjectPtr<USoundBase> BreakSound;

	// 服务器破门与状态查询
	/** 服务器确认角色持有斧头且正在挥动后调用 */
	bool BreakFromAxe();

	/** 服务器破门以及客户端收到破门状态时通知订阅者 */
	UPROPERTY(BlueprintAssignable, Category = "Door")
	FDeliveryDoorBroken OnDoorBroken;

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
    // 保留原根组件做稳定碰撞，受击只移动无碰撞的可见副本
    UPROPERTY(VisibleAnywhere, Category="Door") TObjectPtr<UStaticMeshComponent> ImpactVisual;
    UPROPERTY(Replicated, VisibleAnywhere, Category="Door") int32 HitCount = 0;
    UFUNCTION(NetMulticast, Reliable) void MulticastImpact(FVector_NetQuantize HitPoint, bool bFinal);
    float ImpactElapsed = 1.f;

	// 破门状态同步
	UPROPERTY(ReplicatedUsing = OnRep_Broken, VisibleAnywhere, BlueprintReadOnly, Category = "Door")
	bool bBroken = false;

	UFUNCTION()
	void OnRep_Broken();
};
