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
class UDeliveryGrabComponent;
class UDeliveryGrabbableComponent;
class UDeliveryInteractionProbeComponent;
class UDeliveryInventoryComponent;
class UDeliveryHotbarWidget;
class USkeletalMeshComponent;
class USpringArmComponent;
class UCameraComponent;
class UInputAction;
class UGameplayAbility;
class USceneComponent;
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

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UDeliveryGrabComponent> GrabComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UDeliveryGrabbableComponent> GrabbableComponent;

	/** 探测身边"能按 F 的东西"并推出浮窗提示。纯本地表现，和 Grab 的双键抓取是两套东西。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UDeliveryInteractionProbeComponent> InteractProbe;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta=(AllowPrivateAccess="true"))
	TObjectPtr<UDeliveryInventoryComponent> InventoryComponent;

	/** Right-hand item grip anchor. Designers can tune this in BP without changing every item. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta=(AllowPrivateAccess="true"))
	TObjectPtr<USceneComponent> HeldItemAnchor;

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
	 * 交互键（F）。故意用软引用而不是构造函数里的 ConstructorHelpers：
	 * IA_Interact 是 setup_motorbike.py 生成的资产，构造函数只在模块加载时跑一次，
	 * 资产要是这次会话里才新建的就永远解析不到——必须等绑定输入时再加载。
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	TSoftObjectPtr<UInputAction> InteractAction;

	/** 拾取键（E）。和 F 通用交互严格分开。 */
	UPROPERTY(EditAnywhere, Category="Input")
	TSoftObjectPtr<UInputAction> PickupAction;

	/** 可在编辑器里继续美化的 UMG 子类；资产缺失时回退到原生 Hotbar。 */
	UPROPERTY(EditDefaultsOnly, Category="UI")
	TSoftClassPtr<UDeliveryHotbarWidget> HotbarWidgetClass;

	/**
	 * 服务端两次起跳之间的最小间隔，只用来拦客户端刷包，不是玩法上的冷却。
	 * 能不能跳由 UDeliveryActiveRagdollComponent::IsGrounded() 决定：落地即可再跳。
	 */
	UPROPERTY(EditAnywhere, Category="Ragdoll|Network", meta=(ClampMin="0.0"))
	float MinimumJumpInterval = 0.05f;

	UPROPERTY(EditDefaultsOnly, Category="Camera|Vehicle Impact", meta=(ClampMin="0.0"))
	float VehicleImpactCameraZoom = 120.0f;
	UPROPERTY(EditDefaultsOnly, Category="Camera|Vehicle Impact", meta=(ClampMin="0.0"))
	float VehicleImpactCameraHoldSeconds = 0.35f;
	UPROPERTY(EditDefaultsOnly, Category="Camera|Vehicle Impact", meta=(ClampMin="0.1"))
	float VehicleImpactCameraZoomOutSpeed = 8.0f;
	UPROPERTY(EditDefaultsOnly, Category="Camera|Vehicle Impact", meta=(ClampMin="0.1"))
	float VehicleImpactCameraReturnSpeed = 3.0f;

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
	 * 靠 GE_HealthRegen 按 HealthRegenRate（6/秒）回血：0% → 50% 约 8.3 秒（原 40%，约 6.7 秒，
	 * 嫌被拖拽的人醒得太快而放宽）。这是全局阈值，不区分"躺着没人管"还是"正被拖走"，
	 * 拖拽本身不会暂停或减慢这段回血，所以调这个值会让所有晕倒（互殴、车撞等）都变长，不只是被拖的情况。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Ability", meta=(ClampMin="1.0", ClampMax="100.0"))
	float StunRecoverHealthPercent = 50.0f;

public:

	ADeliveryCharacter();
	virtual void Tick(float DeltaSeconds) override;
	void NotifyVehicleImpact();

	UFUNCTION(Client, Reliable)
	void ClientVehicleImpactCamera();

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
	void AttackLeftEnded(const FInputActionValue& Value);
	void AttackRightEnded(const FInputActionValue& Value);
	void ResolveSingleMousePress();
	void MousePressed(bool bLeft);
	void MouseReleased(bool bLeft);
	void InteractStarted(const FInputActionValue& Value);
	void PickupStarted(const FInputActionValue& Value);
	void PickupEnded(const FInputActionValue& Value);
	void InventorySlot1();
	void InventorySlot2();
	void InventorySlot3();
	void InventorySlot4();
	void InventorySlot5();

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

	/** 对当前探测到的目标发起一次交互。客户端只是"我想按这个"，真正的判定在服务器。 */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoInteract();

	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoPickup();

	/** Local probe/UI query. Negative progress means this target is not currently being held. */
	float GetPickupHoldProgress(const AActor* Target) const;
	bool CanUsePickupTarget(const AActor* Target, bool bCheckTaskAvailability = true) const;

	// IDeliveryCombatInterface
	virtual bool StartMeleeAttack(EMeleeHand Hand) override;
	virtual TArray<AActor*> GatherMeleeHits(EMeleeHand Hand) const override;
	virtual void EndMeleeAttack(EMeleeHand Hand) override;
	virtual bool IsMeleeAttacking() const override;

	UFUNCTION(Server, Unreliable)
	void ServerSetMoveInput(FVector2D Input, float AimYaw);

	UFUNCTION(Server, Reliable)
	void ServerJump();

	/** 服务器复核：目标身上确实有可交互组件、距离也够得着，才真的执行。 */
	UFUNCTION(Server, Reliable)
	void ServerInteract(AActor* Target);

	UFUNCTION(Server, Reliable)
	void ServerPickup(AActor* Target);

	UFUNCTION(Server, Reliable)
	void ServerBeginPickupHold(AActor* Target);

	UFUNCTION(Server, Reliable)
	void ServerCompletePickupHold(AActor* Target);

	UFUNCTION(Server, Reliable)
	void ServerCancelPickupHold(AActor* Target);

	/**
	 * 是不是正躺着（血空/晕倒）。倒地的人不该还能按 F 上车、按 E 捡东西。
	 *
	 * 判据用复制过来的 State.Stunned Tag 而不是 ASC 上那个 bStunned——后者只在服务器上维护，
	 * 客户端读永远是 false，本机的提示浮窗会照样弹出来。血量兜底一条，防止 Tag 还没同步到。
	 */
	UFUNCTION(BlueprintPure, Category="State")
	bool IsIncapacitated() const;

	FORCEINLINE UCapsuleComponent* GetCapsuleComponent() const { return CapsuleComponent; }
	FORCEINLINE USkeletalMeshComponent* GetMesh() const { return Mesh; }
	FORCEINLINE USpringArmComponent* GetCameraBoom() const { return CameraBoom; }
	FORCEINLINE UCameraComponent* GetFollowCamera() const { return FollowCamera; }
	FORCEINLINE UDeliveryActiveRagdollComponent* GetActiveRagdoll() const { return ActiveRagdoll; }
	FORCEINLINE UDeliveryRagdollCombatComponent* GetRagdollCombat() const { return RagdollCombat; }
	FORCEINLINE UDeliveryGrabComponent* GetGrabComponent() const { return GrabComponent; }
	FORCEINLINE UDeliveryGrabbableComponent* GetGrabbableComponent() const { return GrabbableComponent; }
	FORCEINLINE UDeliveryInteractionProbeComponent* GetInteractProbe() const { return InteractProbe; }
	FORCEINLINE UDeliveryInventoryComponent* GetInventoryComponent() const { return InventoryComponent; }
	FORCEINLINE USceneComponent* GetHeldItemAnchor() const { return HeldItemAnchor; }
	bool IsGrabChordHeld() const { return bGrabChordActive && bLeftMouseDown && bRightMouseDown; }
	FORCEINLINE TSubclassOf<UGameplayEffect> GetHealthRegenEffect() const { return HealthRegenEffect; }
	FORCEINLINE TSubclassOf<UGameplayEffect> GetDamageEffect() const { return DamageEffect; }
	FORCEINLINE TSubclassOf<UGameplayAbility> GetPunchLeftAbilityClass() const { return PunchLeftAbilityClass; }
	FORCEINLINE TSubclassOf<UGameplayAbility> GetPunchRightAbilityClass() const { return PunchRightAbilityClass; }
	FORCEINLINE float GetStunRecoverHealthPercent() const { return StunRecoverHealthPercent; }

private:

	/** 取得角色水平面向，作为物理出拳方向。Hand 保留给未来左右拳差异化使用。 */
	bool ComputePunchAim(EMeleeHand Hand, FVector& OutAimDir) const;
	void BeginPickupInteraction();
	void UpdatePickupHold();
	void CancelPickupHold(bool bNotifyServer = true);
	bool ValidateServerPickupTarget(AActor* Target) const;

	float LastServerJumpTime = -1000.0f;
	float VehicleCameraBaseArmLength = 0.0f;
	float VehicleCameraHoldUntil = 0.0f;
	bool bVehicleCameraZoomActive = false;
	void StartVehicleCameraZoom();
	FTimerHandle MouseChordTimer;
	bool bLeftMouseDown = false;
	bool bRightMouseDown = false;
	bool bGrabChordActive = false;
	bool bSingleMouseResolved = false;
	bool bFirstMouseLeft = false;
	bool bPickupHeld = false;
	float PickupHoldStartedAt = 0.0f;
	float PickupHoldDuration = 0.0f;
	TWeakObjectPtr<AActor> PickupHoldTarget;
	FTimerHandle PickupHoldTimer;
	TWeakObjectPtr<AActor> ServerPickupHoldTarget;
	float ServerPickupHoldStartedAt = -1000.0f;

	UPROPERTY(Transient)
	TObjectPtr<UDeliveryHotbarWidget> HotbarWidget;
};
