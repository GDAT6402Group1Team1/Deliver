// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "DeliveryCharacter.generated.h"

class UCapsuleComponent;
class UDeliveryActiveRagdollComponent;
class USkeletalMeshComponent;
class USpringArmComponent;
class UCameraComponent;
class UInputAction;
struct FInputActionValue;

/** 第三人称Pawn：胶囊跟镜头，身体由主动滑稽布娃娃驱动。 */
UCLASS()
class ADeliveryCharacter : public APawn
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

protected:

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> JumpAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> MouseLookAction;

	/** 跳跃时给盆骨的向上速度（厘米/秒）。 */
	UPROPERTY(EditAnywhere, Category="Ragdoll", meta=(ClampMin="0.0"))
	float JumpSpeed = 700.0f;

	UPROPERTY(EditAnywhere, Category="Ragdoll|Network", meta=(ClampMin="0.05"))
	float MinimumJumpInterval = 0.2f;

public:

	ADeliveryCharacter();

protected:

	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

	void Move(const FInputActionValue& Value);
	void Look(const FInputActionValue& Value);
	void JumpStarted(const FInputActionValue& Value);

public:

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoMove(float Right, float Forward);

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoLook(float Yaw, float Pitch);

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoJumpStart();

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoJumpEnd();

	UFUNCTION(Server, Unreliable)
	void ServerSetMoveInput(FVector2D Input, float AimYaw);

	UFUNCTION(Server, Reliable)
	void ServerJump();

	FORCEINLINE UCapsuleComponent* GetCapsuleComponent() const { return CapsuleComponent; }
	FORCEINLINE USkeletalMeshComponent* GetMesh() const { return Mesh; }
	FORCEINLINE USpringArmComponent* GetCameraBoom() const { return CameraBoom; }
	FORCEINLINE UCameraComponent* GetFollowCamera() const { return FollowCamera; }
	FORCEINLINE UDeliveryActiveRagdollComponent* GetActiveRagdoll() const { return ActiveRagdoll; }

private:

	float LastServerJumpTime = -1000.0f;
};
