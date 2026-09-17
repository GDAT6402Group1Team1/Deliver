// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemInterface.h"
#include "Combat/DeliveryCombatInterface.h"
#include "GameFramework/Pawn.h"
#include "DeliveryCharacter.generated.h"

class UAbilitySystemComponent;
class UGameplayEffect;
class UCapsuleComponent;
class UDeliveryActiveRagdollComponent;
class UDeliveryRagdollCombatComponent;
class USkeletalMeshComponent;
class USpringArmComponent;
class UCameraComponent;
class UInputAction;
class UGameplayAbility;
struct FInputActionValue;

/** 第三人称Pawn：胶囊跟镜头，身体由主动滑稽布娃娃驱动。 */
UCLASS()
class ADeliveryCharacter : public APawn, public IAbilitySystemInterface, public IDeliveryCombatInterface
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCapsuleComponent> CapsuleComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USkeletalMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> FollowCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UDeliveryActiveRagdollComponent> ActiveRagdoll;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UDeliveryRagdollCombatComponent> RagdollCombat;

protected:

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> JumpAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> MouseLookAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> AttackLeftAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> AttackRightAction;

	/**
	 * 服务端两次起跳之间的最小间隔，只用来拦客户端刷包，不是玩法上的冷却。
	 * 能不能跳由 UDeliveryActiveRagdollComponent::IsGrounded() 决定：落地即可再跳。
	 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|Network", meta=(ClampMin="0.0"))
	float MinimumJumpInterval = 0.05f;

	// 在 BP_DeliveryMan 里指定 GE_HealthRegen
	UPROPERTY(EditDefaultsOnly, Category="Ability")
	TSubclassOf<UGameplayEffect> HealthRegenEffect;

	// 在 BP_DeliveryMan 里指定 GE_Damage（Instant + SetByCaller Effect.Type.Damage）
	UPROPERTY(EditDefaultsOnly, Category="Ability")
	TSubclassOf<UGameplayEffect> DamageEffect;

	// 默认用 C++ GA；也可在蓝图里换成子类
	UPROPERTY(EditDefaultsOnly, Category="Ability")
	TSubclassOf<UGameplayAbility> PunchLeftAbilityClass;

	UPROPERTY(EditDefaultsOnly, Category="Ability")
	TSubclassOf<UGameplayAbility> PunchRightAbilityClass;

	/**
	 * 晕倒后回血到最大生命值的百分之多少就自动醒来。HP 归零即晕倒，晕倒期间不再受伤害，
	 * 靠 GE_HealthRegen 按 HealthRegenRate（6/秒）回血：0% → 40% 约 6.7 秒。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Ability", meta=(ClampMin="1.0", ClampMax="100.0"))
	float StunRecoverHealthPercent = 40.0f;

public:

	ADeliveryCharacter();

	// PlayerState复制时调用
	virtual void OnRep_PlayerState() override;
	
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

protected:

	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

	void Move(const FInputActionValue& Value);
	void Look(const FInputActionValue& Value);
	void JumpStarted(const FInputActionValue& Value);
	void AttackLeftStarted(const FInputActionValue& Value);
	void AttackRightStarted(const FInputActionValue& Value);

public:

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoMove(float Right, float Forward);

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoLook(float Yaw, float Pitch);

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoJumpStart();

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoJumpEnd();

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoAttackLeft();

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoAttackRight();

	// IDeliveryCombatInterface
	virtual bool StartMeleeAttack(EMeleeHand Hand) override;
	virtual TArray<AActor*> GatherMeleeHits(EMeleeHand Hand) const override;
	virtual void EndMeleeAttack(EMeleeHand Hand) override;
	virtual bool IsMeleeAttacking() const override;

	UFUNCTION(Server, Unreliable)
	void ServerSetMoveInput(FVector2D Input, float AimYaw);

	UFUNCTION(Server, Reliable)
	void ServerJump();

	FORCEINLINE UCapsuleComponent* GetCapsuleComponent() const { return CapsuleComponent; }
	FORCEINLINE USkeletalMeshComponent* GetMesh() const { return Mesh; }
	FORCEINLINE USpringArmComponent* GetCameraBoom() const { return CameraBoom; }
	FORCEINLINE UCameraComponent* GetFollowCamera() const { return FollowCamera; }
	FORCEINLINE UDeliveryActiveRagdollComponent* GetActiveRagdoll() const { return ActiveRagdoll; }
	FORCEINLINE UDeliveryRagdollCombatComponent* GetRagdollCombat() const { return RagdollCombat; }
	FORCEINLINE TSubclassOf<UGameplayEffect> GetHealthRegenEffect() const { return HealthRegenEffect; }
	FORCEINLINE TSubclassOf<UGameplayEffect> GetDamageEffect() const { return DamageEffect; }
	FORCEINLINE TSubclassOf<UGameplayAbility> GetPunchLeftAbilityClass() const { return PunchLeftAbilityClass; }
	FORCEINLINE TSubclassOf<UGameplayAbility> GetPunchRightAbilityClass() const { return PunchRightAbilityClass; }
	FORCEINLINE float GetStunRecoverHealthPercent() const { return StunRecoverHealthPercent; }

private:

	/** 取得角色水平面向，作为物理出拳方向。Hand 保留给未来左右拳差异化使用。 */
	bool ComputePunchAim(EMeleeHand Hand, FVector& OutAimDir) const;

	float LastServerJumpTime = -1000.0f;
};
