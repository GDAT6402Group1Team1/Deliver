// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryCharacter.h"
#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "EnhancedInputComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "UObject/ConstructorHelpers.h"
#include "Delivery.h"
#include "GAS/DeliverAbilitySystemComponent.h"
#include "GAS/DeliverPlayerState.h"
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
