// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Combat/DeliveryCombatTypes.h"
#include "DeliveryRagdollCombatComponent.generated.h"

class USkeletalMeshComponent;
class UDeliveryActiveRagdollComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliverPunchHitWindow, EMeleeHand, Hand);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliverPunchEnded, EMeleeHand, Hand);

/**
 * 布娃娃近战表现：按瞄准方向给手臂冲量，定时广播命中窗口与攻击结束。
 * 伤害与 CD 由 GAS 负责，本组件不管。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryRagdollCombatComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryRagdollCombatComponent();

	virtual void BeginPlay() override;

	/** 开始一次挥拳。AimDir 由 Character 准心射线算出。 */
	bool StartPunch(EMeleeHand Hand, FVector AimDir);

	void CancelPunch();

	/** 命中检测 Sphere 中心（手臂骨骼 + 沿瞄准方向前伸）。 */
	FTransform GetPunchTraceTransform() const;

	float GetHitRadius() const { return HitRadius; }
	bool IsPunching() const { return bIsPunching; }

	UPROPERTY(BlueprintAssignable, Category="Punch")
	FDeliverPunchHitWindow OnPunchHitWindow;

	UPROPERTY(BlueprintAssignable, Category="Punch")
	FDeliverPunchEnded OnPunchEnded;

protected:

	/** 释放瞬间给前臂和拳头沿瞄准方向的速度变化，让拳头是被抡出去的而不是被牵着走。 */
	UPROPERTY(EditAnywhere, Category="Punch", meta=(ClampMin="0.0"))
	float PunchImpulse = 1800.f;

	/** 收拳到释放之间的时间。太短就看不出抡的过程。 */
	UPROPERTY(EditAnywhere, Category="Punch", meta=(ClampMin="0.0", ClampMax="0.6"))
	float WindupDelay = 0.26f;

	/** 前送接近最远点时进入命中窗口。 */
	UPROPERTY(EditAnywhere, Category="Punch", meta=(ClampMin="0.0"))
	float HitWindowDelay = 0.4f;

	/** 整段攻击多久结束。结束时手臂肌肉重新打开。 */
	UPROPERTY(EditAnywhere, Category="Punch", meta=(ClampMin="0.0"))
	float PunchEndDelay = 0.72f;

	/** 检测点相对拳头骨骼沿瞄准方向的前伸距离（厘米）。 */
	UPROPERTY(EditAnywhere, Category="Punch", meta=(ClampMin="0.0"))
	float ArmReachOffset = 20.f;

	UPROPERTY(EditAnywhere, Category="Punch", meta=(ClampMin="0.0"))
	float HitRadius = 55.f;

	UPROPERTY(EditAnywhere, Category="Punch")
	FName LeftArmBone = TEXT("LeftArm");

	UPROPERTY(EditAnywhere, Category="Punch")
	FName RightArmBone = TEXT("RightArm");

	UPROPERTY(EditAnywhere, Category="Punch")
	FName LeftForearmBone = TEXT("LeftForeArm");

	UPROPERTY(EditAnywhere, Category="Punch")
	FName RightForearmBone = TEXT("RightForeArm");

	UPROPERTY(EditAnywhere, Category="Punch")
	FName LeftHandBone = TEXT("LeftHand");

	UPROPERTY(EditAnywhere, Category="Punch")
	FName RightHandBone = TEXT("RightHand");

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<UDeliveryActiveRagdollComponent> ActiveRagdoll;

	FVector CachedAimDir = FVector::ForwardVector;
	EMeleeHand ActiveHand = EMeleeHand::Left;
	bool bIsPunching = false;

	FTimerHandle HitWindowTimer;
	FTimerHandle PunchDriveTimer;
	FTimerHandle EndTimer;

	/** 蓄力完成后将冲量集中施加在前臂和手，形成短促、有落点的直拳。 */
	void FirePunchDrive();
	void FireHitWindow();
	void FirePunchEnded();
};
