// Copyright Epic Games, Inc. All Rights Reserved.

#include "DeliveryCharacter.h"
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

ADeliveryCharacter::ADeliveryCharacter()
{
	// -- 基础设置 --
	PrimaryActorTick.bCanEverTick = false;

	// -- 角色不跟随控制器旋转 --
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// -- 创建胶囊体 --
	CapsuleComponent = CreateDefaultSubobject<UCapsuleComponent>(TEXT("CollisionCylinder"));
	CapsuleComponent->InitCapsuleSize(42.f, 96.0f);
	
	CapsuleComponent->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
	CapsuleComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);		// 开启射线检测和物理碰撞
	CapsuleComponent->SetSimulatePhysics(true);												// 物理模拟
	CapsuleComponent->SetEnableGravity(true);												// 启用重力
	RootComponent = CapsuleComponent;

	// -- 创建网格体 --
	Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("CharacterMesh0"));
	Mesh->SetupAttachment(RootComponent);
	Mesh->SetRelativeLocation(FVector(0.0f, 0.0f, -96.0f));
	Mesh->SetRelativeRotation(FRotator(0.0f, -90.0f, 0.0f));
	
	Mesh->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);		
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);					// 仅开启射线检测
	Mesh->SetSimulatePhysics(false);											// 不参与物理模拟
	Mesh->SetEnableGravity(false);											// 不启用重力

	// -- 创建弹簧臂 --
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f;
	CameraBoom->bUsePawnControlRotation = true;								// 弹簧臂跟随控制器旋转
	CameraBoom->bDoCollisionTest = true;

	// -- 创建摄像机 --
	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

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
}

// 在 Pawn 被 PlayerController 控制时，输入组件初始化时由引擎调用
void ADeliveryCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &ADeliveryCharacter::Look);
		EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &ADeliveryCharacter::Look);
	}
	else
	{
		UE_LOG(LogDelivery, Error, TEXT("'%s' Failed to find an Enhanced Input component."), *GetNameSafe(this));
	}
}

void ADeliveryCharacter::Look(const FInputActionValue& Value)
{
	const FVector2D LookAxisVector = Value.Get<FVector2D>();
	DoLook(LookAxisVector.X, LookAxisVector.Y);
}

void ADeliveryCharacter::DoLook(float Yaw, float Pitch)
{
	if (Controller != nullptr)
	{
		AddControllerYawInput(Yaw);
		AddControllerPitchInput(Pitch);
	}
}

void ADeliveryCharacter::DoMove(float Right, float Forward)
{
}

void ADeliveryCharacter::DoJumpStart()
{
}

void ADeliveryCharacter::DoJumpEnd()
{
}
