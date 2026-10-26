// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryCharacter.h"
#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "CollisionQueryParams.h"
#include "Engine/CollisionProfile.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "GAS/Abilities/GA_DeliverPunch.h"
#include "GAS/DeliverGameplayTags.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "Delivery.h"
#include "GAS/DeliverAbilitySystemComponent.h"
#include "GAS/DeliverAttributeSet.h"
#include "GAS/DeliverPlayerState.h"
#include "Combat/DeliveryRagdollCombatComponent.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Interaction/DeliveryInteractionProbeComponent.h"
#include "Interaction/DeliveryInteractionGeometry.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Vehicle/DeliveryVehicleSummonComponent.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Task/DeliveryItemComponent.h"
#include "Task/DeliveryTargetComponent.h"
#include "Task/DeliveryPickupPointComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "UI/DeliveryHotbarWidget.h"
#include "TimerManager.h"

ADeliveryCharacter::ADeliveryCharacter()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(20.0f);
	SetMinNetUpdateFrequency(10.0f);

	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// 胶囊只做镜头根和简单碰撞，不参与布娃娃物理。
	CapsuleComponent = CreateDefaultSubobject<UCapsuleComponent>(TEXT("CollisionCylinder"));
	CapsuleComponent->InitCapsuleSize(42.f, 96.0f);
	CapsuleComponent->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
	CapsuleComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	CapsuleComponent->SetSimulatePhysics(false);
	CapsuleComponent->SetEnableGravity(false);
	RootComponent = CapsuleComponent;

	Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("CharacterMesh0"));
	Mesh->SetupAttachment(RootComponent);
	Mesh->SetRelativeLocation(FVector(0.0f, 0.0f, -96.0f));
	Mesh->SetRelativeRotation(FRotator(0.0f, -90.0f, 0.0f));
	Mesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	Mesh->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Mesh->SetSimulatePhysics(false);
	Mesh->SetEnableGravity(false);

	// Runtime equivalent of an authored skeleton socket: the BP can tune this grip
	// without editing the shared skeleton asset. Held items smooth-follow it post physics.
	HeldItemAnchor = CreateDefaultSubobject<USceneComponent>(TEXT("RightHandItemAnchor"));
	HeldItemAnchor->SetupAttachment(Mesh, TEXT("RightHand"));

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f;
	CameraBoom->bUsePawnControlRotation = true;
	CameraBoom->bDoCollisionTest = true;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GrabHighlightFinder(
		TEXT("/Game/Blueprint/Interaction/M_GrabHighlight"));
	if (GrabHighlightFinder.Succeeded())
	{
		FollowCamera->PostProcessSettings.WeightedBlendables.Array.Add(
			FWeightedBlendable(1.0f, GrabHighlightFinder.Object));
		FollowCamera->PostProcessBlendWeight = 1.0f;
	}

	ActiveRagdoll = CreateDefaultSubobject<UDeliveryActiveRagdollComponent>(TEXT("ActiveRagdoll"));
	RagdollCombat = CreateDefaultSubobject<UDeliveryRagdollCombatComponent>(TEXT("RagdollCombat"));
	GrabComponent = CreateDefaultSubobject<UDeliveryGrabComponent>(TEXT("Grab"));
	GrabbableComponent = CreateDefaultSubobject<UDeliveryGrabbableComponent>(TEXT("Grabbable"));
	InteractProbe = CreateDefaultSubobject<UDeliveryInteractionProbeComponent>(TEXT("InteractProbe"));
	InventoryComponent = CreateDefaultSubobject<UDeliveryInventoryComponent>(TEXT("Inventory"));
	VehicleSummon = CreateDefaultSubobject<UDeliveryVehicleSummonComponent>(TEXT("VehicleSummon"));
	SummonVehicleKey = EKeys::R;
	InteractAction = TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/IA_Interact.IA_Interact")));
	PickupAction = TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/IA_Pickup.IA_Pickup")));
	HotbarWidgetClass = TSoftClassPtr<UDeliveryHotbarWidget>(FSoftClassPath(TEXT("/Game/UI/Inventory/WBP_DeliveryHotbar.WBP_DeliveryHotbar_C")));

	PunchLeftAbilityClass = UGA_DeliverPunchLeft::StaticClass();
	PunchRightAbilityClass = UGA_DeliverPunchRight::StaticClass();

	static ConstructorHelpers::FObjectFinder<UInputAction> LookActionFinder(TEXT("/Game/Input/Actions/IA_Look"));
	if (LookActionFinder.Succeeded())
	{
		LookAction = LookActionFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UInputAction> MouseLookActionFinder(TEXT("/Game/Input/Actions/IA_MouseLook"));
	if (MouseLookActionFinder.Succeeded())
	{
		MouseLookAction = MouseLookActionFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UInputAction> MoveActionFinder(TEXT("/Game/Input/Actions/IA_Move"));
	if (MoveActionFinder.Succeeded())
	{
		MoveAction = MoveActionFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UInputAction> JumpActionFinder(TEXT("/Game/Input/Actions/IA_Jump"));
	if (JumpActionFinder.Succeeded())
	{
		JumpAction = JumpActionFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UInputAction> AttackLeftFinder(TEXT("/Game/Input/Actions/IA_AttackLeft"));
	if (AttackLeftFinder.Succeeded())
	{
		AttackLeftAction = AttackLeftFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UInputAction> AttackRightFinder(TEXT("/Game/Input/Actions/IA_AttackRight"));
	if (AttackRightFinder.Succeeded())
	{
		AttackRightAction = AttackRightFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UAnimSequence> StandPoseFinder(
		TEXT("/Game/Characters/TestDeliveryMan/The_Boss_Anim"));
	if (StandPoseFinder.Succeeded())
	{
		Mesh->SetAnimation(StandPoseFinder.Object);
		Mesh->SetPlayRate(0.0f);
	}
}

bool ADeliveryCharacter::IsIncapacitated() const
{
	const UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
	if (!ASC)
	{
		return false;
	}
	if (ASC->HasMatchingGameplayTag(TAG_State_Stunned))
	{
		return true;
	}
	// Tag 是跟着 GE 走的，血刚归零到 ASC 挂上 Tag 之间有一两帧空窗，这里补一刀。
	return ASC->GetNumericAttribute(UDeliverAttributeSet::GetHealthAttribute()) <= 0.0f;
}

void ADeliveryCharacter::NotifyVehicleImpact()
{
	if (!HasAuthority())
	{
		return;
	}
	if (IsLocallyControlled())
	{
		StartVehicleCameraZoom();
	}
	else
	{
		ClientVehicleImpactCamera();
	}
}

void ADeliveryCharacter::ClientVehicleImpactCamera_Implementation()
{
	StartVehicleCameraZoom();
}

void ADeliveryCharacter::StartVehicleCameraZoom()
{
	if (!IsLocallyControlled() || !CameraBoom || !GetWorld())
	{
		return;
	}
	if (!bVehicleCameraZoomActive)
	{
		VehicleCameraBaseArmLength = CameraBoom->TargetArmLength;
		bVehicleCameraZoomActive = true;
		SetActorTickEnabled(true);
	}
	VehicleCameraHoldUntil = GetWorld()->GetTimeSeconds() + VehicleImpactCameraHoldSeconds;
}

void ADeliveryCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bVehicleCameraZoomActive || !CameraBoom || !GetWorld())
	{
		return;
	}
	const bool bHolding = GetWorld()->GetTimeSeconds() < VehicleCameraHoldUntil;
	const float TargetLength = VehicleCameraBaseArmLength + (bHolding ? VehicleImpactCameraZoom : 0.0f);
	CameraBoom->TargetArmLength = FMath::FInterpTo(CameraBoom->TargetArmLength,
		TargetLength, DeltaSeconds, bHolding ? VehicleImpactCameraZoomOutSpeed : VehicleImpactCameraReturnSpeed);
	if (!bHolding && FMath::IsNearlyEqual(CameraBoom->TargetArmLength, VehicleCameraBaseArmLength, 1.0f))
	{
		CameraBoom->TargetArmLength = VehicleCameraBaseArmLength;
		bVehicleCameraZoomActive = false;
		SetActorTickEnabled(false);
	}
}

void ADeliveryCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();

	// 本机客户端： 已经在 OnPawnSet 时InitAbilityInfo
	if (IsLocallyControlled())
	{
		return;
	}

	// 其他客户端： PS复制时InitAbilityInfo
	ADeliverPlayerState* PS = GetPlayerState<ADeliverPlayerState>();
	if (UDeliverAbilitySystemComponent* ASC = PS ? PS->GetDeliverAbilitySystemComponent() : nullptr)
	{
		ASC->InitializeAbilityActor(PS, this);
	}
}

UAbilitySystemComponent* ADeliveryCharacter::GetAbilitySystemComponent() const
{
	const ADeliverPlayerState* PS = GetPlayerState<ADeliverPlayerState>();
	return PS ? PS->GetAbilitySystemComponent() : nullptr;
}

void ADeliveryCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!EnhancedInputComponent)
	{
		UE_LOG(LogDelivery, Error, TEXT("%s：没有 Enhanced Input 组件。"), *GetNameSafe(this));
		return;
	}

	if (LookAction)
	{
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &ADeliveryCharacter::Look);
	}
	if (MouseLookAction)
	{
		EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &ADeliveryCharacter::MouseLook);
	}
	if (MoveAction)
	{
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ADeliveryCharacter::Move);
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Completed, this, &ADeliveryCharacter::Move);
	}
	if (JumpAction)
	{
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ADeliveryCharacter::JumpStarted);
	}
	if (AttackLeftAction)
	{
		EnhancedInputComponent->BindAction(AttackLeftAction, ETriggerEvent::Started, this, &ADeliveryCharacter::AttackLeftStarted);
		EnhancedInputComponent->BindAction(AttackLeftAction, ETriggerEvent::Completed, this, &ADeliveryCharacter::AttackLeftEnded);
		EnhancedInputComponent->BindAction(AttackLeftAction, ETriggerEvent::Canceled, this, &ADeliveryCharacter::AttackLeftEnded);
	}
	if (AttackRightAction)
	{
		EnhancedInputComponent->BindAction(AttackRightAction, ETriggerEvent::Started, this, &ADeliveryCharacter::AttackRightStarted);
		EnhancedInputComponent->BindAction(AttackRightAction, ETriggerEvent::Completed, this, &ADeliveryCharacter::AttackRightEnded);
		EnhancedInputComponent->BindAction(AttackRightAction, ETriggerEvent::Canceled, this, &ADeliveryCharacter::AttackRightEnded);
	}
	if (UInputAction* Interact = InteractAction.LoadSynchronous())
	{
		EnhancedInputComponent->BindAction(Interact, ETriggerEvent::Started, this, &ADeliveryCharacter::InteractStarted);
	}
	if (UInputAction* Pickup = PickupAction.LoadSynchronous())
	{
		EnhancedInputComponent->BindAction(Pickup, ETriggerEvent::Started, this, &ADeliveryCharacter::PickupStarted);
		EnhancedInputComponent->BindAction(Pickup, ETriggerEvent::Completed, this, &ADeliveryCharacter::PickupEnded);
		EnhancedInputComponent->BindAction(Pickup, ETriggerEvent::Canceled, this, &ADeliveryCharacter::PickupEnded);
	}

	// Hotbar is deliberately direct-keyed: 1 is drop, 2-5 swap with the hand.
	PlayerInputComponent->BindKey(EKeys::One, IE_Pressed, this, &ADeliveryCharacter::InventorySlot1);
	PlayerInputComponent->BindKey(EKeys::Two, IE_Pressed, this, &ADeliveryCharacter::InventorySlot2);
	PlayerInputComponent->BindKey(EKeys::Three, IE_Pressed, this, &ADeliveryCharacter::InventorySlot3);
	PlayerInputComponent->BindKey(EKeys::Four, IE_Pressed, this, &ADeliveryCharacter::InventorySlot4);
	PlayerInputComponent->BindKey(EKeys::Five, IE_Pressed, this, &ADeliveryCharacter::InventorySlot5);
	// Match the direct hotbar actions on gamepad. D-pad up remains the phone action in IMC_Default.
	PlayerInputComponent->BindKey(EKeys::Gamepad_DPad_Down, IE_Pressed, this, &ADeliveryCharacter::InventorySlot1);
	PlayerInputComponent->BindKey(EKeys::Gamepad_DPad_Left, IE_Pressed, this, &ADeliveryCharacter::InventorySlot2);
	PlayerInputComponent->BindKey(EKeys::Gamepad_DPad_Right, IE_Pressed, this, &ADeliveryCharacter::InventorySlot3);
	PlayerInputComponent->BindKey(EKeys::Gamepad_LeftShoulder, IE_Pressed, this, &ADeliveryCharacter::InventorySlot4);
	PlayerInputComponent->BindKey(EKeys::Gamepad_RightShoulder, IE_Pressed, this, &ADeliveryCharacter::InventorySlot5);

	if (SummonVehicleKey.IsValid())
	{
		PlayerInputComponent->BindKey(SummonVehicleKey, IE_Pressed, this, &ADeliveryCharacter::SummonVehiclePressed);
		if (VehicleSummon)
		{
			// 提示里的键名从实际绑的键取，改了 SummonVehicleKey 提示会跟着变，不会说一套做一套。
			VehicleSummon->SetDisplayKeyName(SummonVehicleKey.GetDisplayName(false));
		}
	}

	if (!HotbarWidget && IsLocallyControlled())
	{
		UClass* WidgetClass = HotbarWidgetClass.LoadSynchronous();
		if (!WidgetClass) WidgetClass = UDeliveryHotbarWidget::StaticClass();
		HotbarWidget = CreateWidget<UDeliveryHotbarWidget>(Cast<APlayerController>(Controller), WidgetClass);
		if (HotbarWidget)
		{
			HotbarWidget->SetInventory(InventoryComponent);
			HotbarWidget->AddToViewport(10);
		}
	}
}

void ADeliveryCharacter::Move(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	DoMove(Axis.X, Axis.Y);
}

void ADeliveryCharacter::Look(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	DoLook(Axis.X, Axis.Y);
}

void ADeliveryCharacter::MouseLook(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>() * MouseLookSensitivity;
	DoLook(Axis.X, Axis.Y);
}

void ADeliveryCharacter::JumpStarted(const FInputActionValue& /*Value*/)
{
	DoJumpStart();
}

void ADeliveryCharacter::AttackLeftStarted(const FInputActionValue& /*Value*/)
{
	MousePressed(true);
}

void ADeliveryCharacter::AttackRightStarted(const FInputActionValue& /*Value*/)
{
	MousePressed(false);
}

void ADeliveryCharacter::AttackLeftEnded(const FInputActionValue& /*Value*/)
{
	MouseReleased(true);
}

void ADeliveryCharacter::AttackRightEnded(const FInputActionValue& /*Value*/)
{
	MouseReleased(false);
}

void ADeliveryCharacter::MousePressed(bool bLeft)
{
	bool& ThisDown = bLeft ? bLeftMouseDown : bRightMouseDown;
	if (ThisDown) return;
	const bool bWasEmpty = !bLeftMouseDown && !bRightMouseDown;
	ThisDown = true;
	if (bWasEmpty)
	{
		bFirstMouseLeft = bLeft;
		bSingleMouseResolved = false;
		// 组合抓取判定窗口：先按下的键在这段时间内等第二只键，超时才按单键出拳解析。
		// 从 0.12s 放宽到 0.2s，给玩家更宽松的时间把 LMB+RMB 按成"同时"。
		GetWorldTimerManager().SetTimer(MouseChordTimer, this,
			&ADeliveryCharacter::ResolveSingleMousePress, 0.2f, false);
	}
	else if (!bSingleMouseResolved && !bGrabChordActive)
	{
		GetWorldTimerManager().ClearTimer(MouseChordTimer);
		bGrabChordActive = true;
		if (GrabComponent) GrabComponent->RequestBegin();
	}
	else if (!bGrabChordActive)
	{
		if (bLeft) DoAttackLeft(); else DoAttackRight();
	}
}

void ADeliveryCharacter::ResolveSingleMousePress()
{
	if (bGrabChordActive || bSingleMouseResolved) return;
	bSingleMouseResolved = true;
	if (bFirstMouseLeft && bLeftMouseDown) DoAttackLeft();
	else if (!bFirstMouseLeft && bRightMouseDown) DoAttackRight();
}

void ADeliveryCharacter::MouseReleased(bool bLeft)
{
	bool& ThisDown = bLeft ? bLeftMouseDown : bRightMouseDown;
	if (!ThisDown) return;
	ThisDown = false;
	if (bGrabChordActive)
	{
		if (GrabComponent) GrabComponent->RequestReleaseHand(bLeft);
		if (!bLeftMouseDown && !bRightMouseDown) bGrabChordActive = false;
	}
	else if (!bSingleMouseResolved)
	{
		GetWorldTimerManager().ClearTimer(MouseChordTimer);
		bSingleMouseResolved = true;
		if (bLeft) DoAttackLeft(); else DoAttackRight();
	}
	if (!bLeftMouseDown && !bRightMouseDown)
	{
		GetWorldTimerManager().ClearTimer(MouseChordTimer);
		bSingleMouseResolved = false;
	}
}

void ADeliveryCharacter::DoLook(float Yaw, float Pitch)
{
	if (Controller)
	{
		AddControllerYawInput(Yaw);
		AddControllerPitchInput(Pitch);
	}
}

void ADeliveryCharacter::DoMove(float Right, float Forward)
{
	if (ActiveRagdoll)
	{
		const FVector2D Input(Right, Forward);
		ActiveRagdoll->SetMoveInput(Input);
		if (!HasAuthority())
		{
			// 本地先响应，服务端接收同一输入并运行权威物理。
			ServerSetMoveInput(Input, Controller ? Controller->GetControlRotation().Yaw : 0.0f);
		}
	}
}

void ADeliveryCharacter::DoJumpStart()
{
	if (!ActiveRagdoll)
	{
		return;
	}

	if (HasAuthority())
	{
		ServerJump();
		return;
	}

	// 客户端先本地起跳（无前摇），随后由服务端快照修正误差。
	// 不在地上时 TryStartJump 自己会拒绝，这里不用额外判断。
	ActiveRagdoll->TryStartJump();
	ServerJump();
}

void ADeliveryCharacter::DoJumpEnd()
{
}

void ADeliveryCharacter::DoAttackLeft()
{
	if (GrabComponent && GrabComponent->IsGrabbing()) return;
	if (InventoryComponent && InventoryComponent->HasHeldItem())
	{
		InventoryComponent->UseHeldItem();
		return;
	}
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponent())
	{
		ASC->TryActivateAbilitiesByTag(FGameplayTagContainer(TAG_Ability_Attack_Punch_Left));
	}
}

void ADeliveryCharacter::DoAttackRight()
{
	if (GrabComponent && GrabComponent->IsGrabbing()) return;
	if (InventoryComponent && InventoryComponent->HasHeldItem()) return;
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponent())
	{
		ASC->TryActivateAbilitiesByTag(FGameplayTagContainer(TAG_Ability_Attack_Punch_Right));
	}
}

void ADeliveryCharacter::InteractStarted(const FInputActionValue& /*Value*/)
{
	DoInteract();
}

void ADeliveryCharacter::PickupStarted(const FInputActionValue& /*Value*/)
{
	BeginPickupInteraction();
}

void ADeliveryCharacter::PickupEnded(const FInputActionValue& /*Value*/)
{
	CancelPickupHold();
}

void ADeliveryCharacter::InventorySlot1() { if (InventoryComponent) InventoryComponent->RequestSlotAction(0); }
void ADeliveryCharacter::InventorySlot2() { if (InventoryComponent) InventoryComponent->RequestSlotAction(1); }
void ADeliveryCharacter::InventorySlot3() { if (InventoryComponent) InventoryComponent->RequestSlotAction(2); }
void ADeliveryCharacter::InventorySlot4() { if (InventoryComponent) InventoryComponent->RequestSlotAction(3); }
void ADeliveryCharacter::InventorySlot5() { if (InventoryComponent) InventoryComponent->RequestSlotAction(4); }

void ADeliveryCharacter::SummonVehiclePressed()
{
	// 倒着的人别想召唤车。和按 F 上车同一条规矩。
	if (VehicleSummon && !IsIncapacitated())
	{
		VehicleSummon->RequestSummon();
	}
}

void ADeliveryCharacter::DoInteract()
{
	// 客户端拿本地探测到的目标；在服务器上调 Server RPC 等价于直接调实现。
	AActor* Target = InteractProbe ? InteractProbe->GetFocusedActor() : nullptr;
	if (Target)
	{
		ServerInteract(Target);
	}
}

void ADeliveryCharacter::DoPickup()
{
	BeginPickupInteraction();
}

void ADeliveryCharacter::DoPickupRelease()
{
	CancelPickupHold();
}

void ADeliveryCharacter::BeginPickupInteraction()
{
	if (bPickupHeld) return;
	AActor* Target = InteractProbe ? InteractProbe->GetFocusedPickupActor() : nullptr;
	UDeliveryInteractableComponent* Interactable = UDeliveryInteractableComponent::FindOn(Target);
	if (!Target || !Interactable || !CanUsePickupTarget(Target)) return;

	if (Interactable->HoldDuration <= KINDA_SMALL_NUMBER)
	{
		if (Target->IsA<ADeliveryHandheldItem>()) ServerPickup(Target);
		return;
	}

	bPickupHeld = true;
	PickupHoldTarget = Target;
	PickupHoldStartedAt = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	PickupHoldDuration = Interactable->HoldDuration;
	ServerBeginPickupHold(Target);
	GetWorldTimerManager().SetTimer(PickupHoldTimer, this,
		&ADeliveryCharacter::UpdatePickupHold, 0.02f, true);
}

void ADeliveryCharacter::UpdatePickupHold()
{
	AActor* Target = PickupHoldTarget.Get();
	if (!bPickupHeld || !Target || !InteractProbe
		|| InteractProbe->GetFocusedPickupActor() != Target || !CanUsePickupTarget(Target))
	{
		CancelPickupHold();
		return;
	}
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (Now - PickupHoldStartedAt < PickupHoldDuration) return;

	GetWorldTimerManager().ClearTimer(PickupHoldTimer);
	bPickupHeld = false;
	PickupHoldTarget.Reset();
	PickupHoldDuration = 0.0f;
	ServerCompletePickupHold(Target);
}

void ADeliveryCharacter::CancelPickupHold(bool bNotifyServer)
{
	if (!bPickupHeld) return;
	AActor* Target = PickupHoldTarget.Get();
	GetWorldTimerManager().ClearTimer(PickupHoldTimer);
	bPickupHeld = false;
	PickupHoldTarget.Reset();
	PickupHoldDuration = 0.0f;
	if (bNotifyServer && Target) ServerCancelPickupHold(Target);
}

float ADeliveryCharacter::GetPickupHoldProgress(const AActor* Target) const
{
	if (!bPickupHeld || PickupHoldTarget.Get() != Target || PickupHoldDuration <= KINDA_SMALL_NUMBER)
	{
		return -1.0f;
	}
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	return FMath::Clamp((Now - PickupHoldStartedAt) / PickupHoldDuration, 0.0f, 1.0f);
}

bool ADeliveryCharacter::CanUsePickupTarget(const AActor* Target, bool bCheckTaskAvailability) const
{
	const UDeliveryInteractableComponent* Interactable = UDeliveryInteractableComponent::FindOn(Target);
	if (!Target || !Interactable || Interactable->InteractionKey != EDeliveryInteractionKey::PickupE
		|| !Interactable->CanInteract(this)) return false;
	if (GrabComponent && GrabComponent->IsGrabbing()) return false;
	if (const UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
		ASC && ASC->HasMatchingGameplayTag(TAG_State_Stunned)) return false;
	// 骑车时那具身体的布娃娃是停掉的（StopRagdoll），控制模式不是 Active。
	// 所以这道门槛只对步行有效，不然上车之后 E 就彻底失灵了
	const bool bRiding = IsRidingVehicle();
	if (!bRiding && ActiveRagdoll
		&& ActiveRagdoll->GetControlMode() != EDeliveryRagdollControlMode::Active) return false;

	// 取货点：从这里领快递，领到的放哪由 ReceiveTaskPackage 决定
	if (const UDeliveryPickupPointComponent* PickupPoint = UDeliveryPickupPointComponent::FindOn(Target))
	{
		return !bCheckTaskAvailability || PickupPoint->CanDispense();
	}
	if (const ADeliveryHandheldItem* Item = Cast<ADeliveryHandheldItem>(Target))
	{
		if (Item->GetOwner()) return false;
		if (const UDeliveryItemComponent* DeliveryItem = Item->FindComponentByClass<UDeliveryItemComponent>())
		{
			return !bCheckTaskAvailability || DeliveryItem->CanBeAcquired();
		}
		// 非任务道具（斧头之类）骑车时不给捡：它要进背包，而背包表现挂在
		// 那具被隐藏的身体上，骑车时捡起来玩家什么都看不到
		return !bRiding;
	}
	if (const UDeliveryTargetComponent* DeliveryTarget = Target->FindComponentByClass<UDeliveryTargetComponent>())
	{
		// 这里**不能**用 Inventory->GetHeldItem()：骑车时快递在车上、手上是空的，
		// 用背包那句会让"骑到收件点交不了货"
		return DeliveryTarget->CanAcceptDelivery(GetCarriedTaskPackage(), GetTaskPlayerState());
	}
	return false;
}

ADeliveryMotorbike* ADeliveryCharacter::GetCurrentVehicle() const
{
	// 附着父级就是权威：上车时角色被 attach 到车的碰撞盒上，下车时 detach。
	// 还要复核 Driver 指的是我——被别人撞飞落到车上这种情况不算"在骑车"
	ADeliveryMotorbike* Bike = Cast<ADeliveryMotorbike>(GetAttachParentActor());

	return (Bike && Bike->GetDriver() == this) ? Bike : nullptr;
}

ADeliveryPackageItem* ADeliveryCharacter::GetCarriedTaskPackage() const
{
	// 车上优先：骑车时快递在车上、手上是空的
	if (const ADeliveryMotorbike* Bike = GetCurrentVehicle())
	{
		if (ADeliveryPackageItem* Cargo = Bike->GetCargo())
		{
			return Cargo;
		}
	}

	return InventoryComponent ? Cast<ADeliveryPackageItem>(InventoryComponent->GetHeldItem()) : nullptr;
}

APlayerState* ADeliveryCharacter::GetTaskPlayerState() const
{
	if (APlayerState* Mine = GetPlayerState())
	{
		return Mine;
	}

	// 被 UnPossess 之后自己这边是空的，去车那边取——骑车时控制器 Possess 的是车
	const ADeliveryMotorbike* Bike = GetCurrentVehicle();

	return Bike ? Bike->GetPlayerState() : nullptr;
}

bool ADeliveryCharacter::ReceiveTaskPackage(ADeliveryPackageItem* Package, bool bPlayFeedback)
{
	if (!HasAuthority() || !IsValid(Package))
	{
		return false;
	}

	// 已经带着一件了就不再收。两件同时存在的话，交付时取到哪一件是不确定的
	if (GetCarriedTaskPackage())
	{
		return false;
	}

	UDeliveryItemComponent* Item = Package->GetDeliveryItemComponent();

	if (ADeliveryMotorbike* Bike = GetCurrentVehicle())
	{
		if (!Bike->AttachCargo(Package))
		{
			return false;
		}

		// 绑上车之后才让任务进入进行中：NotifyAcquired 失败的话上面那一步要能回滚，
		// 而回滚一个已经改过的任务状态比回滚一次附着麻烦得多
		if (Item && !Item->NotifyAcquired(GetTaskPlayerState()))
		{
			Bike->ReleaseCargo();

			return false;
		}

		if (bPlayFeedback) ClientPickupSucceeded();

		return true;
	}

	// 步行：走现有背包那条路。TryPickup 内部会调 NotifyAcquired，不要在这里重复调
	if (InventoryComponent && InventoryComponent->TryPickup(Package))
	{
		if (bPlayFeedback) ClientPickupSucceeded();

		return true;
	}

	return false;
}

void ADeliveryCharacter::ServerInteract_Implementation(AActor* Target)
{
	// F is the existing general interaction key. Inventory pickup belongs exclusively to E.
	if (Target && Target->IsA<ADeliveryHandheldItem>()) return;
	UDeliveryInteractableComponent* Interactable = UDeliveryInteractableComponent::FindOn(Target);
	if (Interactable && Interactable->InteractionKey == EDeliveryInteractionKey::GeneralF
		&& Interactable->CanInteract(this))
	{
		Interactable->Execute(this);
	}
}

void ADeliveryCharacter::ServerPickup_Implementation(AActor* Target)
{
	ADeliveryHandheldItem* Item = Cast<ADeliveryHandheldItem>(Target);
	UDeliveryInteractableComponent* Interactable = UDeliveryInteractableComponent::FindOn(Target);
	// Delivery items are hold-only; they may enter through ServerCompletePickupHold, never this tap RPC.
	if (Item && !Item->FindComponentByClass<UDeliveryItemComponent>() && Interactable
		&& Interactable->InteractionKey == EDeliveryInteractionKey::PickupE
		&& Interactable->HoldDuration <= KINDA_SMALL_NUMBER
		&& ValidateServerPickupTarget(Target) && InventoryComponent)
	{
		if (InventoryComponent->TryPickup(Item))
		{
			ClientPickupSucceeded();
		}
	}
}

bool ADeliveryCharacter::ValidateServerPickupTarget(AActor* Target) const
{
	// E interactions use range and gameplay eligibility, independent of camera or facing.
	return Target && Target->GetWorld() == GetWorld() && CanUsePickupTarget(Target);
}

void ADeliveryCharacter::ServerBeginPickupHold_Implementation(AActor* Target)
{
	UDeliveryInteractableComponent* Interactable = UDeliveryInteractableComponent::FindOn(Target);
	if (!Interactable || Interactable->HoldDuration <= KINDA_SMALL_NUMBER
		|| !ValidateServerPickupTarget(Target)) return;
	ServerPickupHoldTarget = Target;
	ServerPickupHoldStartedAt = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}

void ADeliveryCharacter::ServerCompletePickupHold_Implementation(AActor* Target)
{
	UDeliveryInteractableComponent* Interactable = UDeliveryInteractableComponent::FindOn(Target);
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	const bool bHeldLongEnough = Interactable && Interactable->HoldDuration > KINDA_SMALL_NUMBER
		&& Now - ServerPickupHoldStartedAt >= FMath::Max(0.0f, Interactable->HoldDuration - 0.03f);
	if (ServerPickupHoldTarget.Get() != Target || !bHeldLongEnough || !ValidateServerPickupTarget(Target))
	{
		ServerPickupHoldTarget.Reset();
		return;
	}
	ServerPickupHoldTarget.Reset();

	// 取货点发货。成功音效由 ReceiveTaskPackage 自己放——它才知道到底收下了没有
	if (UDeliveryPickupPointComponent* PickupPoint = UDeliveryPickupPointComponent::FindOn(Target))
	{
		PickupPoint->TryDispense(this);
		return;
	}
	if (ADeliveryPackageItem* Package = Cast<ADeliveryPackageItem>(Target))
	{
		// 掉在地上的任务快递也走同一个出口：步行捡回手上、骑车捡上车，
		// "放哪"这个判断全程只有 ReceiveTaskPackage 一处
		ReceiveTaskPackage(Package);
		return;
	}
	if (ADeliveryHandheldItem* Item = Cast<ADeliveryHandheldItem>(Target))
	{
		if (InventoryComponent && InventoryComponent->TryPickup(Item))
		{
			ClientPickupSucceeded();
		}
		return;
	}
	if (UDeliveryTargetComponent* DeliveryTarget = Target->FindComponentByClass<UDeliveryTargetComponent>())
	{
		if (DeliveryTarget->TryDeliver(GetCarriedTaskPackage(), GetTaskPlayerState()))
		{
			ClientDeliverySucceeded();
		}
	}
}

void ADeliveryCharacter::ClientPickupSucceeded_Implementation()
{
	if (PickupSuccessSound)
	{
		UGameplayStatics::PlaySound2D(this, PickupSuccessSound);
	}
}

void ADeliveryCharacter::ClientDeliverySucceeded_Implementation()
{
	if (DeliverySuccessSound)
	{
		UGameplayStatics::PlaySound2D(this, DeliverySuccessSound);
	}
}

void ADeliveryCharacter::ServerCancelPickupHold_Implementation(AActor* Target)
{
	if (!Target || ServerPickupHoldTarget.Get() == Target) ServerPickupHoldTarget.Reset();
}

bool ADeliveryCharacter::ComputePunchAim(EMeleeHand Hand, FVector& OutAimDir) const
{
	// 出拳跟随角色朝向，而非相机/准星朝向：转动镜头观察时不会改变拳路。
	OutAimDir = ActiveRagdoll ? ActiveRagdoll->GetBodyForward() : GetActorForwardVector().GetSafeNormal2D();
	return !OutAimDir.IsNearlyZero();
}

bool ADeliveryCharacter::StartMeleeAttack(EMeleeHand Hand)
{
	if (!RagdollCombat)
	{
		return false;
	}

	FVector AimDir;
	return ComputePunchAim(Hand, AimDir)
		&& RagdollCombat->StartPunch(Hand, AimDir);
}

TArray<AActor*> ADeliveryCharacter::GatherMeleeHits(EMeleeHand Hand) const
{
	TArray<AActor*> Results;
	if (!HasAuthority() || !RagdollCombat || !GetWorld())
	{
		return Results;
	}
	const FVector Center = RagdollCombat->GetPunchTraceTransform().GetLocation();
	const float Radius = RagdollCombat->GetHitRadius();

	TArray<FOverlapResult> Overlaps;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PunchHit), false, this);
	GetWorld()->OverlapMultiByChannel(
		Overlaps, Center, FQuat::Identity, ECC_PhysicsBody, FCollisionShape::MakeSphere(Radius), Params);

	for (const FOverlapResult& Overlap : Overlaps)
	{
		ADeliveryCharacter* Other = Cast<ADeliveryCharacter>(Overlap.GetActor());
		if (Other && Other != this && Other->GetPlayerState<ADeliverPlayerState>()
			&& Other->GetActiveRagdoll() && Other->GetActiveRagdoll()->IsRagdollActive())
		{
			// 一个角色有多个刚体，同一拳命中多个部位也只结算一次。
			Results.AddUnique(Other);
		}
	}

	return Results;
}

void ADeliveryCharacter::EndMeleeAttack(EMeleeHand /*Hand*/)
{
}

bool ADeliveryCharacter::IsMeleeAttacking() const
{
	return RagdollCombat->IsPunching();
}

void ADeliveryCharacter::ServerSetMoveInput_Implementation(FVector2D Input, float AimYaw)
{
	if (Controller && FMath::IsFinite(AimYaw))
	{
		FRotator ControlRotation = Controller->GetControlRotation();
		ControlRotation.Yaw = FMath::UnwindDegrees(AimYaw);
		Controller->SetControlRotation(ControlRotation);
	}

	if (ActiveRagdoll)
	{
		const float Right = FMath::IsFinite(Input.X)
			? FMath::Clamp(Input.X, -1.0f, 1.0f)
			: 0.0f;
		const float Forward = FMath::IsFinite(Input.Y)
			? FMath::Clamp(Input.Y, -1.0f, 1.0f)
			: 0.0f;
		ActiveRagdoll->SetMoveInput(FVector2D(Right, Forward));
	}
}

void ADeliveryCharacter::ServerJump_Implementation()
{
	UWorld* World = GetWorld();
	if (!ActiveRagdoll || !World)
	{
		return;
	}

	const float Now = World->GetTimeSeconds();
	if (Now - LastServerJumpTime < MinimumJumpInterval)
	{
		return;
	}

	// 只有真的起跳成功才记时间戳，否则连点被拒的那几次会白白占掉间隔。
	if (ActiveRagdoll->TryStartJump())
	{
		LastServerJumpTime = Now;
	}
}
