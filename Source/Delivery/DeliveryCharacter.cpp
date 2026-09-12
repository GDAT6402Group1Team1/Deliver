// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryCharacter.h"
#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
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
#include "UObject/ConstructorHelpers.h"
#include "Delivery.h"
#include "GAS/DeliverAbilitySystemComponent.h"
#include "GAS/DeliverPlayerState.h"
#include "Combat/DeliveryRagdollCombatComponent.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"

ADeliveryCharacter::ADeliveryCharacter()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(true);
	NetUpdateFrequency = 20.0f;
	MinNetUpdateFrequency = 10.0f;

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

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f;
	CameraBoom->bUsePawnControlRotation = true;
	CameraBoom->bDoCollisionTest = true;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	ActiveRagdoll = CreateDefaultSubobject<UDeliveryActiveRagdollComponent>(TEXT("ActiveRagdoll"));
	RagdollCombat = CreateDefaultSubobject<UDeliveryRagdollCombatComponent>(TEXT("RagdollCombat"));

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
		EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &ADeliveryCharacter::Look);
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
	}
	if (AttackRightAction)
	{
		EnhancedInputComponent->BindAction(AttackRightAction, ETriggerEvent::Started, this, &ADeliveryCharacter::AttackRightStarted);
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

void ADeliveryCharacter::JumpStarted(const FInputActionValue& /*Value*/)
{
	DoJumpStart();
}

void ADeliveryCharacter::AttackLeftStarted(const FInputActionValue& /*Value*/)
{
	DoAttackLeft();
}

void ADeliveryCharacter::AttackRightStarted(const FInputActionValue& /*Value*/)
{
	DoAttackRight();
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
	if (ActiveRagdoll && JumpSpeed > 0.0f)
	{
		if (HasAuthority())
		{
			ServerJump();
		}
		else
		{
			// 客户端先播放跳跃，随后由服务端快照修正误差。
			ActiveRagdoll->AddImpulse(FVector(0.0f, 0.0f, JumpSpeed), true);
			ServerJump();
		}
	}
}

void ADeliveryCharacter::DoJumpEnd()
{
}

void ADeliveryCharacter::DoAttackLeft()
{
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponent())
	{
		ASC->TryActivateAbilitiesByTag(FGameplayTagContainer(TAG_Ability_Attack_Punch_Left));
	}
}

void ADeliveryCharacter::DoAttackRight()
{
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponent())
	{
		ASC->TryActivateAbilitiesByTag(FGameplayTagContainer(TAG_Ability_Attack_Punch_Right));
	}
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
	const FVector Center = RagdollCombat->GetPunchTraceTransform().GetLocation();
	const float Radius = RagdollCombat->GetHitRadius();

	TArray<FOverlapResult> Overlaps;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(PunchHit), false, this);
	GetWorld()->OverlapMultiByChannel(
		Overlaps, Center, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(Radius), Params);

	for (const FOverlapResult& Overlap : Overlaps)
	{
		APawn* Other = Cast<APawn>(Overlap.GetActor());
		if (Other && Other != this && Other->GetPlayerState<ADeliverPlayerState>())
		{
			Results.Add(Other);
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
	if (!ActiveRagdoll || !World || JumpSpeed <= 0.0f)
	{
		return;
	}

	const float Now = World->GetTimeSeconds();
	if (Now - LastServerJumpTime < MinimumJumpInterval)
	{
		return;
	}

	LastServerJumpTime = Now;
	ActiveRagdoll->AddImpulse(FVector(0.0f, 0.0f, JumpSpeed), true);
}
