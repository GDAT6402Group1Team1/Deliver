// Copyright Epic Games, Inc. All Rights Reserved.

#include "Vehicle/DeliveryMotorbike.h"
#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CollisionQueryParams.h"
#include "Delivery.h"
#include "DeliveryCharacter.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Grab/DeliveryGrabComponent.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "Net/UnrealNetwork.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryMotorbike::ADeliveryMotorbike()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(30.0f);
	SetMinNetUpdateFrequency(10.0f);

	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	CollisionBox = CreateDefaultSubobject<UBoxComponent>(TEXT("CollisionBox"));
	CollisionBox->InitBoxExtent(FVector(110.0f, 45.0f, 45.0f));
	CollisionBox->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
	CollisionBox->SetSimulatePhysics(false);
	CollisionBox->SetEnableGravity(false);
	RootComponent = CollisionBox;

	MeshRoot = CreateDefaultSubobject<USceneComponent>(TEXT("MeshRoot"));
	MeshRoot->SetupAttachment(RootComponent);

	// 定额槽位而不是运行时创建组件：构造脚本每次重跑都动态建/删组件，在编辑器里很容易
	// 留下重复实例或者丢掉实例覆盖（这个项目在样条实例数据上已经吃过类似的亏）。
	BodyParts.Reserve(MaxBodyParts);
	for (int32 Index = 0; Index < MaxBodyParts; ++Index)
	{
		UStaticMeshComponent* Part = CreateDefaultSubobject<UStaticMeshComponent>(
			*FString::Printf(TEXT("BodyPart%02d"), Index));
		Part->SetupAttachment(MeshRoot);
		// 车体只负责好看，碰撞统一交给 CollisionBox，省得几十个部件各自算碰撞。
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetGenerateOverlapEvents(false);
		BodyParts.Add(Part);
	}

	RiderMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("RiderMesh"));
	RiderMesh->SetupAttachment(MeshRoot);
	RiderMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RiderMesh->SetGenerateOverlapEvents(false);
	// 骑手没有动画蓝图，就停在参考姿势上——这份 FBX 的参考姿势本来就是坐姿。
	RiderMesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	RiderMesh->SetVisibility(false);

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 650.0f;
	CameraBoom->SocketOffset = FVector(0.0f, 0.0f, 120.0f);
	CameraBoom->bUsePawnControlRotation = true;
	CameraBoom->bDoCollisionTest = true;
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 8.0f;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	Interactable = CreateDefaultSubobject<UDeliveryInteractableComponent>(TEXT("Interactable"));

	DrivePromptText = NSLOCTEXT("Delivery", "MotorbikeDrivePrompt", "按 F 驾驶");
	ExitPromptText = NSLOCTEXT("Delivery", "MotorbikeExitPrompt", "按 F 下车");

	static ConstructorHelpers::FObjectFinder<UInputAction> MoveActionFinder(TEXT("/Game/Input/Actions/IA_Move"));
	if (MoveActionFinder.Succeeded())
	{
		MoveAction = MoveActionFinder.Object;
	}
	static ConstructorHelpers::FObjectFinder<UInputAction> LookActionFinder(TEXT("/Game/Input/Actions/IA_Look"));
	if (LookActionFinder.Succeeded())
	{
		LookAction = LookActionFinder.Object;
	}
	static ConstructorHelpers::FObjectFinder<UInputAction> MouseLookFinder(TEXT("/Game/Input/Actions/IA_MouseLook"));
	if (MouseLookFinder.Succeeded())
	{
		MouseLookAction = MouseLookFinder.Object;
	}

	InteractAction = TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/IA_Interact.IA_Interact")));
}

void ADeliveryMotorbike::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ADeliveryMotorbike, Driver);
}

void ADeliveryMotorbike::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ApplyBodyMeshes();
}

void ADeliveryMotorbike::BeginPlay()
{
	Super::BeginPlay();
	ApplyBodyMeshes();

	if (Interactable)
	{
		Interactable->PromptText = DrivePromptText;
		Interactable->OnInteract.AddDynamic(this, &ADeliveryMotorbike::HandleInteractRequested);
	}

	ApplyDriverPresentation();
}

void ADeliveryMotorbike::ApplyBodyMeshes()
{
	for (int32 Index = 0; Index < BodyParts.Num(); ++Index)
	{
		UStaticMeshComponent* Part = BodyParts[Index];
		if (!Part)
		{
			continue;
		}
		UStaticMesh* Mesh = BodyMeshes.IsValidIndex(Index) ? BodyMeshes[Index].Get() : nullptr;
		Part->SetStaticMesh(Mesh);
		Part->SetVisibility(Mesh != nullptr);
	}

	if (BodyMeshes.Num() > MaxBodyParts)
	{
		UE_LOG(LogDelivery, Warning,
			TEXT("%s：车体网格有 %d 个，超过了 %d 个固定槽位，多出来的不会显示。"),
			*GetName(), BodyMeshes.Num(), MaxBodyParts);
	}
}

void ADeliveryMotorbike::ApplyDriverPresentation()
{
	const bool bHasDriver = Driver != nullptr;
	if (RiderMesh)
	{
		RiderMesh->SetVisibility(bHasDriver);
	}
	if (Interactable)
	{
		// 有人骑着的时候别人不该再看到"按 F 驾驶"。
		Interactable->bInteractEnabled = !bHasDriver;
	}
}

void ADeliveryMotorbike::OnRep_Driver()
{
	ApplyDriverPresentation();
}

void ADeliveryMotorbike::HandleInteractRequested(APawn* Interactor)
{
	TryEnter(Cast<ADeliveryCharacter>(Interactor));
}

bool ADeliveryMotorbike::TryEnter(ADeliveryCharacter* NewDriver)
{
	if (!HasAuthority() || Driver || !IsValid(NewDriver))
	{
		return false;
	}

	AController* DriverController = NewDriver->GetController();
	if (!DriverController)
	{
		return false;
	}

	Driver = NewDriver;

	// 手上还抓着东西就先松开：抓取是用物理约束把目标连在手骨上的，
	// 驾驶员一会儿要被停掉物理并隐藏，约束留着会把货物/别的玩家一路拖在车上。
	if (UDeliveryGrabComponent* Grab = NewDriver->GetGrabComponent())
	{
		Grab->ForceRelease();
	}

	// 先把那具布娃娃停掉再隐藏：StopRagdoll 会关掉刚体模拟并把网格挂回胶囊，
	// 否则被挂到车上的身体还在自己模拟，会一路被车拖着抽搐。
	if (UDeliveryActiveRagdollComponent* Ragdoll = NewDriver->GetActiveRagdoll())
	{
		Ragdoll->StopRagdoll();
	}
	NewDriver->SetActorEnableCollision(false);
	NewDriver->SetActorHiddenInGame(true);
	NewDriver->AttachToComponent(CollisionBox, FAttachmentTransformRules::SnapToTargetNotIncludingScale);

	DriverController->Possess(this);
	if (APlayerController* PC = Cast<APlayerController>(DriverController))
	{
		PC->SetViewTargetWithBlend(this, 0.35f);
	}

	ThrottleInput = 0.0f;
	SteerInput = 0.0f;
	ApplyDriverPresentation();
	return true;
}

void ADeliveryMotorbike::ExitVehicle()
{
	if (!HasAuthority() || !Driver)
	{
		return;
	}

	// 落点要在清掉 Driver 之前算：FindExitLocation 会把驾驶员排除出扫描。
	const FVector ExitLocation = FindExitLocation();

	ADeliveryCharacter* Leaving = Driver;
	Driver = nullptr;
	AController* BikeController = GetController();

	Leaving->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	Leaving->SetActorEnableCollision(true);
	Leaving->SetActorHiddenInGame(false);
	Leaving->SetActorLocationAndRotation(
		ExitLocation, FRotator(0.0f, GetActorRotation().Yaw, 0.0f), false, nullptr, ETeleportType::TeleportPhysics);

	if (BikeController)
	{
		BikeController->Possess(Leaving);
		if (APlayerController* PC = Cast<APlayerController>(BikeController))
		{
			PC->SetViewTargetWithBlend(Leaving, 0.35f);
		}
	}

	// StartRagdoll 里带 PlaceOnGround，所以要在挪好位置之后再开。
	if (UDeliveryActiveRagdollComponent* Ragdoll = Leaving->GetActiveRagdoll())
	{
		Ragdoll->StartRagdoll();
	}

	ThrottleInput = 0.0f;
	SteerInput = 0.0f;
	ApplyDriverPresentation();
}

FVector ADeliveryMotorbike::FindExitLocation() const
{
	const UWorld* World = GetWorld();
	const FVector Center = GetActorLocation();
	if (!World)
	{
		return Center + FVector(0.0f, 0.0f, 150.0f);
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(MotorbikeExit), false, this);
	Params.AddIgnoredActor(Driver);

	// 先试首选那一侧，被占了就试另一侧；两侧都不行就原地抬高放下，总比卡进墙里好。
	const float Offsets[2] = { ExitSideOffset, -ExitSideOffset };
	for (const float Offset : Offsets)
	{
		const FVector Side = Center + GetActorRightVector() * Offset;

		FHitResult BlockHit;
		if (World->SweepSingleByChannel(BlockHit, Center, Side, FQuat::Identity, ECC_Visibility,
			FCollisionShape::MakeSphere(45.0f), Params))
		{
			continue;
		}

		FHitResult GroundHit;
		if (World->LineTraceSingleByChannel(GroundHit, Side + FVector(0.0f, 0.0f, 150.0f),
			Side - FVector(0.0f, 0.0f, 500.0f), ECC_Visibility, Params))
		{
			// 角色胶囊半高 96，落点抬到地面上方一个胶囊高度。
			return GroundHit.ImpactPoint + FVector(0.0f, 0.0f, 100.0f);
		}
		return Side;
	}

	return Center + FVector(0.0f, 0.0f, 150.0f);
}

void ADeliveryMotorbike::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!EnhancedInput)
	{
		return;
	}

	if (MoveAction)
	{
		EnhancedInput->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ADeliveryMotorbike::MoveInput);
		EnhancedInput->BindAction(MoveAction, ETriggerEvent::Completed, this, &ADeliveryMotorbike::MoveInput);
	}
	if (LookAction)
	{
		EnhancedInput->BindAction(LookAction, ETriggerEvent::Triggered, this, &ADeliveryMotorbike::LookInput);
	}
	if (MouseLookAction)
	{
		EnhancedInput->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &ADeliveryMotorbike::LookInput);
	}
	if (UInputAction* Interact = InteractAction.LoadSynchronous())
	{
		EnhancedInput->BindAction(Interact, ETriggerEvent::Started, this, &ADeliveryMotorbike::InteractPressed);
	}
}

void ADeliveryMotorbike::MoveInput(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	SteerInput = FMath::Clamp(Axis.X, -1.0f, 1.0f);
	ThrottleInput = FMath::Clamp(Axis.Y, -1.0f, 1.0f);

	if (!HasAuthority())
	{
		ServerSetDriveInput(ThrottleInput, SteerInput);
	}
}

void ADeliveryMotorbike::ServerSetDriveInput_Implementation(float InThrottle, float InSteer)
{
	ThrottleInput = FMath::IsFinite(InThrottle) ? FMath::Clamp(InThrottle, -1.0f, 1.0f) : 0.0f;
	SteerInput = FMath::IsFinite(InSteer) ? FMath::Clamp(InSteer, -1.0f, 1.0f) : 0.0f;
}

void ADeliveryMotorbike::LookInput(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	if (!Controller)
	{
		return;
	}
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(Axis.Y);

	if (!Axis.IsNearlyZero() && GetWorld())
	{
		LastLookTime = GetWorld()->GetTimeSeconds();
	}
}

void ADeliveryMotorbike::InteractPressed(const FInputActionValue& /*Value*/)
{
	if (HasAuthority())
	{
		ExitVehicle();
		return;
	}
	ServerRequestExit();
}

void ADeliveryMotorbike::ServerRequestExit_Implementation()
{
	// Server RPC 只能由拥有这个 Pawn 的连接发起，也就是现在骑着它的那个玩家，
	// 所以这里不需要再校验身份；ExitVehicle 自己会挡住"没人骑"的情况。
	ExitVehicle();
}

void ADeliveryMotorbike::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (HasAuthority() && Driver && !IsValid(Driver))
	{
		Driver = nullptr;
		ApplyDriverPresentation();
	}

	// 服务器一直算（这样停在路边的空车也会自己落到路面上）；
	// 本地驾驶者同步预测一份，免得转向/加速要等一个来回才有反馈。
	const bool bSimulate = HasAuthority() || (Driver && IsLocallyControlled());
	if (bSimulate)
	{
		if (Driver)
		{
			UpdateSpeed(DeltaSeconds);
			UpdateSteering(DeltaSeconds);
		}
		else
		{
			CurrentSpeed = 0.0f;
		}
		UpdateGroundAndMove(DeltaSeconds);
	}

	UpdateLean(DeltaSeconds);

	if (Driver && IsLocallyControlled())
	{
		UpdateCameraRecenter(DeltaSeconds);
		PushExitPrompt();
	}
}

void ADeliveryMotorbike::UpdateSpeed(float DeltaSeconds)
{
	const float Throttle = FMath::Clamp(ThrottleInput, -1.0f, 1.0f);
	if (FMath::IsNearlyZero(Throttle))
	{
		CurrentSpeed = FMath::FInterpConstantTo(CurrentSpeed, 0.0f, DeltaSeconds, CoastDeceleration);
		return;
	}

	// 油门方向和当前车速反着来 = 刹车，用大得多的减速度，手感上按一下就明显慢下来。
	const bool bOpposing = (Throttle * CurrentSpeed) < -UE_KINDA_SMALL_NUMBER;
	const float Rate = bOpposing ? BrakeDeceleration : ThrottleAcceleration;
	const float Target = Throttle > 0.0f ? MaxSpeed * Throttle : MaxReverseSpeed * Throttle;
	CurrentSpeed = FMath::FInterpConstantTo(CurrentSpeed, Target, DeltaSeconds, Rate);
}

void ADeliveryMotorbike::UpdateSteering(float DeltaSeconds)
{
	if (FMath::IsNearlyZero(SteerInput))
	{
		return;
	}

	// 摩托车靠前进才能转向，停着打把是原地转圈，很出戏。
	const float SpeedFactor = FMath::Clamp(FMath::Abs(CurrentSpeed) / TurnSpeedReference, 0.0f, 1.0f);
	const float Direction = CurrentSpeed >= 0.0f ? 1.0f : -1.0f;
	const float YawDelta = SteerInput * MaxTurnRate * SpeedFactor * Direction * DeltaSeconds;
	AddActorWorldRotation(FRotator(0.0f, YawDelta, 0.0f));
}

void ADeliveryMotorbike::UpdateGroundAndMove(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 1) 水平推进。带 sweep，撞墙就掉速而不是穿过去。
	if (!FMath::IsNearlyZero(CurrentSpeed))
	{
		FHitResult MoveHit;
		AddActorWorldOffset(GetActorForwardVector() * CurrentSpeed * DeltaSeconds, true, &MoveHit);
		if (MoveHit.bBlockingHit)
		{
			CurrentSpeed *= 0.3f;
		}
	}

	// 2) 竖直贴地。这一步不能用 sweep：sweep 会被地面挡住，和"我要贴到地面上"互相打架。
	FVector Location = GetActorLocation();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MotorbikeGround), false, this);
	Params.AddIgnoredActor(Driver);

	FHitResult GroundHit;
	const bool bHitGround = World->LineTraceSingleByChannel(
		GroundHit,
		Location + FVector(0.0f, 0.0f, GroundTraceUp),
		Location - FVector(0.0f, 0.0f, GroundTraceDown),
		ECC_Visibility, Params);

	if (bHitGround)
	{
		const float DesiredZ = GroundHit.ImpactPoint.Z + HoverHeight;
		// 离地不超过 60cm 才算"还贴着地"，再高就当是飞出去了，交给重力。
		if (Location.Z <= DesiredZ + 60.0f)
		{
			Location.Z = FMath::FInterpTo(Location.Z, DesiredZ, DeltaSeconds, GroundSnapSpeed);
			VerticalVelocity = 0.0f;
			bWasGrounded = true;
		}
		else
		{
			VerticalVelocity += GravityZ * DeltaSeconds;
			Location.Z = FMath::Max(Location.Z + VerticalVelocity * DeltaSeconds, DesiredZ);
			bWasGrounded = false;
		}
	}
	else
	{
		VerticalVelocity += GravityZ * DeltaSeconds;
		Location.Z += VerticalVelocity * DeltaSeconds;
		bWasGrounded = false;
	}

	SetActorLocation(Location, false, nullptr, ETeleportType::None);

	// 3) 车身跟着坡面抬头/低头。太陡的面（马路牙子、墙根）不跟，跟了会把车竖起来。
	if (bHitGround && bWasGrounded)
	{
		const FVector Normal = GroundHit.ImpactNormal;
		const float SlopeAngle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Normal.Z, -1.0f, 1.0f)));
		if (SlopeAngle <= MaxGroundAlignAngle)
		{
			const FVector PlanarForward = FVector::VectorPlaneProject(GetActorForwardVector(), Normal).GetSafeNormal();
			if (!PlanarForward.IsNearlyZero())
			{
				const FRotator Current = GetActorRotation();
				FRotator Aligned = FMath::RInterpTo(
					Current, FRotationMatrix::MakeFromXZ(PlanarForward, Normal).Rotator(),
					DeltaSeconds, GroundAlignSpeed);
				// 朝向只由转向控制，别让地面对齐把车头带跑。
				Aligned.Yaw = Current.Yaw;
				SetActorRotation(Aligned);
			}
		}
	}
}

void ADeliveryMotorbike::UpdateLean(float DeltaSeconds)
{
	if (!MeshRoot)
	{
		return;
	}

	const float SpeedFactor = FMath::Clamp(FMath::Abs(CurrentSpeed) / TurnSpeedReference, 0.0f, 1.0f);
	// UE 里正 Roll 是往左倒，摩托车要往转弯内侧压，所以右转（SteerInput>0）取负值。
	const float TargetLean = -SteerInput * MaxLeanAngle * SpeedFactor;
	CurrentLean = FMath::FInterpTo(CurrentLean, TargetLean, DeltaSeconds, LeanSpeed);
	MeshRoot->SetRelativeRotation(FRotator(0.0f, 0.0f, CurrentLean));
}

void ADeliveryMotorbike::UpdateCameraRecenter(float DeltaSeconds)
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	UWorld* World = GetWorld();
	if (!PC || !World)
	{
		return;
	}
	// 刚拨过镜头就别抢，让玩家自己看；停着也不抢，不然掉头看后面都做不到。
	if (World->GetTimeSeconds() - LastLookTime < CameraRecenterDelay || FMath::Abs(CurrentSpeed) < 50.0f)
	{
		return;
	}

	FRotator ControlRotation = PC->GetControlRotation();
	const float Delta = FMath::FindDeltaAngleDegrees(ControlRotation.Yaw, GetActorRotation().Yaw);
	ControlRotation.Yaw += Delta * FMath::Clamp(CameraRecenterSpeed * DeltaSeconds, 0.0f, 1.0f);
	PC->SetControlRotation(ControlRotation);
}

void ADeliveryMotorbike::PushExitPrompt()
{
	if (UDeliveryPromptSubsystem* Prompt = UDeliveryPromptSubsystem::Get(this))
	{
		const FVector Anchor = GetActorLocation()
			+ (Interactable ? Interactable->PromptOffset : FVector(0.0f, 0.0f, 180.0f));
		Prompt->PushPrompt(ExitPromptText, Anchor);
	}
}
