// Copyright Epic Games, Inc. All Rights Reserved.

#include "Vehicle/DeliveryMotorbike.h"
#include "AbilitySystemComponent.h"
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
#include "GameplayEffect.h"
#include "GAS/DeliverAttributeSet.h"
#include "GAS/DeliverGameplayTags.h"
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

	MeshAlign = CreateDefaultSubobject<USceneComponent>(TEXT("MeshAlign"));
	MeshAlign->SetupAttachment(MeshRoot);

	SteerPivot = CreateDefaultSubobject<USceneComponent>(TEXT("SteerPivot"));
	SteerPivot->SetupAttachment(MeshAlign);

	BarPivot = CreateDefaultSubobject<USceneComponent>(TEXT("BarPivot"));
	BarPivot->SetupAttachment(MeshAlign);

	RiderPivot = CreateDefaultSubobject<USceneComponent>(TEXT("RiderPivot"));
	RiderPivot->SetupAttachment(MeshAlign);

	// 定额槽位而不是运行时创建组件：构造脚本每次重跑都动态建/删组件，在编辑器里很容易
	// 留下重复实例或者丢掉实例覆盖（这个项目在样条实例数据上已经吃过类似的亏）。
	BodyParts.Reserve(MaxBodyParts);
	for (int32 Index = 0; Index < MaxBodyParts; ++Index)
	{
		UStaticMeshComponent* Part = CreateDefaultSubobject<UStaticMeshComponent>(
			*FString::Printf(TEXT("BodyPart%02d"), Index));
		Part->SetupAttachment(MeshAlign);
		// 车体只负责好看，碰撞统一交给 CollisionBox，省得几十个部件各自算碰撞。
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetGenerateOverlapEvents(false);
		BodyParts.Add(Part);
	}

	RiderMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("RiderMesh"));
	RiderMesh->SetupAttachment(RiderPivot);
	RiderMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RiderMesh->SetGenerateOverlapEvents(false);
	// 骑手没有动画蓝图，就停在参考姿势上——这份 FBX 的参考姿势本来就是坐姿。
	RiderMesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	RiderMesh->SetVisibility(false);

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 650.0f;
	CameraBoom->SocketOffset = FVector(0.0f, 0.0f, 120.0f);
	// 镜头死死跟在车尾后方，不吃控制旋转、不吃鼠标。
	//
	// 之前用 bUsePawnControlRotation=true + 延时回正，结果是上车瞬间镜头还停在人物
	// 原来的朝向上，车头却朝别处——玩家按 W 看到车"横着走"，方向感整个是错的。
	// 载具阶段"W 永远是往屏幕里开"比自由视角重要得多，所以这里直接写死。
	// 俯仰用组件自己的相对角度（bInheritPitch=false 时弹簧臂就取相对值），
	// 这样车爬坡低头时镜头不会跟着翻。
	CameraBoom->bUsePawnControlRotation = false;
	CameraBoom->bInheritPitch = false;
	CameraBoom->bInheritYaw = true;
	CameraBoom->bInheritRoll = false;
	CameraBoom->bDoCollisionTest = true;
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 8.0f;
	CameraBoom->SetRelativeRotation(FRotator(CameraPitch, 0.0f, 0.0f));

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
	// 三个转轴都只是"挂点"：自己摆到轴心上，孩子再把这段偏移减回去，
	// 于是网格留在原地不动，但从此绕这根轴转。
	if (SteerPivot)
	{
		SteerPivot->SetRelativeLocation(SteerPivotLocation);
	}
	if (BarPivot)
	{
		BarPivot->SetRelativeLocation(SteerPivotLocation);
	}
	if (RiderPivot)
	{
		RiderPivot->SetRelativeLocation(RiderPivotLocation);
	}
	if (RiderMesh)
	{
		// 显式改挂：已有的蓝图资产是按"骑手挂在 MeshAlign 上"那一版存下来的，
		// 光改构造函数里的 SetupAttachment 不一定能把老实例带过来。
		if (RiderPivot && RiderMesh->GetAttachParent() != RiderPivot)
		{
			RiderMesh->AttachToComponent(RiderPivot, FAttachmentTransformRules::KeepRelativeTransform);
		}
		RiderMesh->SetRelativeLocation(-RiderPivotLocation);
	}

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

		USceneComponent* Target = MeshAlign;
		bool bOnSteerAxis = false;
		if (SteerPivot && SteeringPartIndices.Contains(Index))
		{
			Target = SteerPivot;
			bOnSteerAxis = true;
		}
		else if (BarPivot && HandlebarPartIndices.Contains(Index))
		{
			Target = BarPivot;
			bOnSteerAxis = true;
		}

		if (Target && Part->GetAttachParent() != Target)
		{
			Part->AttachToComponent(Target, FAttachmentTransformRules::KeepRelativeTransform);
		}
		Part->SetRelativeLocation(bOnSteerAxis ? -SteerPivotLocation : FVector::ZeroVector);
		Part->SetRelativeRotation(FRotator::ZeroRotator);
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
		// 先对齐再混合，否则过渡的起点是人物原来的朝向，看着像镜头甩了一下。
		PC->SetControlRotation(FRotator(CameraPitch, GetActorRotation().Yaw, 0.0f));
		PC->SetViewTargetWithBlend(this, 0.35f);
	}

	ThrottleInput = 0.0f;
	SteerInput = 0.0f;
	TrafficImpactCount = 0;
	ApplyDriverPresentation();
	return true;
}

bool ADeliveryMotorbike::NotifyTrafficImpact(const FVector& CarVelocity)
{
	// 车上没人就不记账：空车被撞不该攒次数，下次有人骑上来是干净的。
	if (!HasAuthority() || !Driver)
	{
		return false;
	}

	++TrafficImpactCount;
	UE_LOG(LogDelivery, Log, TEXT("%s：被交通车撞击 %d/%d 次（来车速度 %.0f cm/s）"),
		*GetName(), TrafficImpactCount, ImpactsToDismount, CarVelocity.Size2D());

	const FVector Direction = CarVelocity.GetSafeNormal2D();
	if (!Direction.IsNearlyZero())
	{
		// 被撞歪的正负取"来车方向相对车身的左右分量"：从右边撞来就往左歪，反之亦然。
		const float Side = FVector::DotProduct(Direction, GetActorRightVector());

		KnockVelocity += Direction * FMath::Min(CarVelocity.Size2D() * KnockbackFraction, MaxKnockbackSpeed);
		KnockYawRate += Side * KnockYawPerHit;
		// UE 里正 Roll 是往左倒；从右边撞来（Side<0）应该往左倒，所以取负号。
		KnockTilt += -Side * KnockTiltPerHit;
		ShakeAmount = 1.0f;
	}

	ADeliveryCharacter* Rider = Driver;
	if (TrafficImpactCount >= ImpactsToDismount)
	{
		// 车会被击退推向 Side 那一侧，人就放到相反的一侧，免得车追上去压住他。
		const float Side = Direction.IsNearlyZero()
			? 0.0f : FVector::DotProduct(Direction, GetActorRightVector());
		const float AwaySign = (FMath::Abs(Side) > 0.1f) ? -FMath::Sign(Side) : 0.0f;
		PendingExitSideSign = AwaySign;

		// 和按 F 下车走同一条路：人放回车边地面、镜头切回角色、布娃娃重新站起来。
		ExitVehicle();

		// 车不许再动：残余的车速和击退速度会让它继续朝人的方向滑过去。
		CurrentSpeed = 0.0f;
		KnockVelocity = FVector::ZeroVector;
		KnockYawRate = 0.0f;

		if (bKnockDownDriverOnDismount)
		{
			// 必须排在 ExitVehicle 之后：它内部的 StartRagdoll 会重建刚体，
			// 之前加的冲量会被冲掉；血也要等人真下了车再清，才看得到倒地。
			//
			// 抛射方向也不用来车方向，而是"车身横向背离车的那一侧" + 一点来车方向，
			// 这样人是被掀到路边去的，不是和车一起往前飞。
			FVector LaunchDir = Direction;
			if (!FMath::IsNearlyZero(AwaySign))
			{
				LaunchDir = (GetActorRightVector() * AwaySign * 1.6f + Direction).GetSafeNormal2D();
			}
			KnockDownDriver(Rider, LaunchDir);
		}
	}
	return true;
}

void ADeliveryMotorbike::KnockDownDriver(ADeliveryCharacter* Rider, const FVector& LaunchDirection)
{
	if (!IsValid(Rider))
	{
		return;
	}

	UAbilitySystemComponent* ASC = Rider->GetAbilitySystemComponent();
	USkeletalMeshComponent* Mesh = Rider->GetMesh();
	if (!ASC || !Mesh)
	{
		return;
	}

	const FGameplayAttribute HealthAttribute = UDeliverAttributeSet::GetHealthAttribute();
	const float Health = ASC->GetNumericAttribute(HealthAttribute);

	// 优先走既有的伤害 GE，和"被车撞倒"保持完全同一条结算路径。
	if (Health > 0.0f && Rider->GetDamageEffect())
	{
		const FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
		FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(Rider->GetDamageEffect(), 1.0f, Context);
		if (Spec.IsValid())
		{
			Spec.Data->SetSetByCallerMagnitude(TAG_Effect_Type_Damage, -Health);
			ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
		}
	}
	// 兜底：回血 GE 还活着时可能把这次扣血抵掉一部分，导致偶发不晕倒。
	// 交通车撞人那边也是这么补的，这里照搬。
	if (ASC->GetNumericAttribute(HealthAttribute) > 0.5f)
	{
		ASC->SetNumericAttributeBase(HealthAttribute, 0.0f);
	}

	// 血清零之后 ASC 自己会把角色切进 Stunned/Limp。这一记冲量把人掀到路边去，
	// 而不是原地瘫成一摊、更不是和车一起往前滑。
	const FVector Direction = LaunchDirection.GetSafeNormal2D();
	if (!Direction.IsNearlyZero())
	{
		const FVector Launch = Direction * KnockDownLaunchSpeed + FVector::UpVector * KnockDownLaunchUp;
		// 只推髋部的话冲量会被整条受约束刚体链分摊掉大半，人几乎飞不起来；
		// 撞人那边也是这么给全身的。
		Mesh->AddImpulse(Launch, TEXT("Hips"), true);
		Mesh->AddImpulseToAllBodiesBelow(FVector::UpVector * KnockDownLaunchUp * 0.5f,
			TEXT("Hips"), true, true);
	}

	// 镜头反馈：这时候视角已经切回角色了，车上那套抖动看不见，
	// 所以复用角色自己"被车撞"的镜头脉冲。
	Rider->NotifyVehicleImpact();

	UE_LOG(LogDelivery, Log, TEXT("%s：驾驶员 %s 被撞下车并打晕"), *GetName(), *GetNameSafe(Rider));
}

void ADeliveryMotorbike::UpdateImpactReaction(float DeltaSeconds)
{
	// 统一的指数衰减：撞击是个尖峰，之后一路平滑回到 0。
	const float Decay = FMath::Exp(-KnockDecay * DeltaSeconds);

	// 只有自己在跑运动的那一端才推，否则会和复制过来的位置打架。
	const bool bSimulate = HasAuthority() || IsLocallyControlled();

	if (!KnockVelocity.IsNearlyZero())
	{
		if (bSimulate)
		{
			FHitResult Hit;
			AddActorWorldOffset(KnockVelocity * DeltaSeconds, true, &Hit);
			if (Hit.bBlockingHit)
			{
				KnockVelocity = FVector::ZeroVector;
			}
		}
		KnockVelocity *= Decay;
	}

	if (!FMath::IsNearlyZero(KnockYawRate))
	{
		if (bSimulate)
		{
			AddActorWorldRotation(FRotator(0.0f, KnockYawRate * DeltaSeconds, 0.0f));
		}
		KnockYawRate *= Decay;
	}

	KnockTilt *= Decay;

	if (ShakeAmount > UE_KINDA_SMALL_NUMBER)
	{
		ShakeAmount *= Decay;
		ShakePhase += DeltaSeconds * ShakeFrequency;
	}
	else
	{
		ShakeAmount = 0.0f;
	}

	if (CameraBoom)
	{
		// 两个轴用不同频率，不然抖动看着像单纯在点头。
		const float Pitch = CameraPitch + FMath::Sin(ShakePhase) * ShakeAngle * ShakeAmount;
		const float Roll = FMath::Sin(ShakePhase * 1.7f) * ShakeAngle * ShakeAmount;
		CameraBoom->SetRelativeRotation(FRotator(Pitch, 0.0f, Roll));
	}
}

void ADeliveryMotorbike::ExitVehicle()
{
	if (!HasAuthority() || !Driver)
	{
		return;
	}

	// 落点要在清掉 Driver 之前算：FindExitLocation 会把驾驶员排除出扫描。
	const FVector ExitLocation = FindExitLocation(PendingExitSideSign);
	PendingExitSideSign = 0.0f;

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
	TrafficImpactCount = 0;
	ApplyDriverPresentation();
}

FVector ADeliveryMotorbike::FindExitLocation(float PreferredSideSign) const
{
	const UWorld* World = GetWorld();
	const FVector Center = GetActorLocation();
	if (!World)
	{
		return Center + FVector(0.0f, 0.0f, 150.0f);
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(MotorbikeExit), false, this);
	Params.AddIgnoredActor(Driver);

	// 横向至少要让开"车身半宽 + 角色胶囊半径 + 余量"，否则人落下来还压在车上。
	// 直接用碰撞盒的实际尺寸，车模型换了也不用回来改这个数。
	const FVector BoxExtent = CollisionBox ? CollisionBox->GetScaledBoxExtent() : FVector(110.0f, 45.0f, 45.0f);
	const float SideDistance = FMath::Max(FMath::Abs(ExitSideOffset), BoxExtent.Y + 42.0f + 60.0f);

	// 首选侧由调用方指定（被撞下车时是"车被推离的那一侧"），没指定就用配置的默认侧。
	const float FirstSign = !FMath::IsNearlyZero(PreferredSideSign)
		? FMath::Sign(PreferredSideSign)
		: FMath::Sign(ExitSideOffset != 0.0f ? ExitSideOffset : -1.0f);

	const FVector Right = GetActorRightVector();
	const FVector Back = -GetActorForwardVector();
	const FVector Candidates[3] = {
		Center + Right * (SideDistance * FirstSign),
		Center + Right * (SideDistance * -FirstSign),
		Center + Back * (BoxExtent.X + 90.0f),
	};

	for (const FVector& Candidate : Candidates)
	{
		FHitResult BlockHit;
		if (World->SweepSingleByChannel(BlockHit, Center, Candidate, FQuat::Identity, ECC_Visibility,
			FCollisionShape::MakeSphere(45.0f), Params))
		{
			continue;
		}

		FHitResult GroundHit;
		if (World->LineTraceSingleByChannel(GroundHit, Candidate + FVector(0.0f, 0.0f, 150.0f),
			Candidate - FVector(0.0f, 0.0f, 500.0f), ECC_Visibility, Params))
		{
			// 角色胶囊半高 96，落点抬到地面上方一个胶囊高度。
			return GroundHit.ImpactPoint + FVector(0.0f, 0.0f, 100.0f);
		}
		return Candidate;
	}

	// 三个方向全被占：抬到车顶上方放下，让他自己掉下来。
	// 不能放回 Center——那就是车身内部，布娃娃会直接卡在车底下。
	return Center + FVector(0.0f, 0.0f, BoxExtent.Z + 140.0f);
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
	// 没有绑 Look：骑车时镜头固定在车尾后方，鼠标不参与（见构造函数里的说明）。
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

	UpdateImpactReaction(DeltaSeconds);
	UpdateLean(DeltaSeconds);
	UpdateSteerVisual(DeltaSeconds);

	if (Driver && IsLocallyControlled())
	{
		SyncControlRotation();
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
	// 被撞歪的倾斜直接叠在转弯侧倾上：它自己按 KnockDecay 衰减，不用再插值一次，
	// 不然撞击那一下的尖峰会被抹平，看着就不像挨了一下。
	MeshRoot->SetRelativeRotation(FRotator(0.0f, 0.0f, CurrentLean + KnockTilt));
}

void ADeliveryMotorbike::UpdateSteerVisual(float DeltaSeconds)
{
	if (!SteerPivot)
	{
		return;
	}

	// 和车身实际转向不同，龙头**不乘速度系数**：停着打把车把也该跟着动，
	// 那是玩家按键有没有被接收到的即时反馈。车身不转、龙头转，观感上也正确。
	const float Target = FMath::Clamp(SteerInput, -1.0f, 1.0f) * MaxVisualSteerAngle;
	CurrentVisualSteer = FMath::FInterpTo(CurrentVisualSteer, Target, DeltaSeconds, SteerVisualSpeed);

	// 局部 Yaw：+X 转向 +Y 就是往右打，和 SteerInput>0 = 按 D 对得上。
	SteerPivot->SetRelativeRotation(FRotator(0.0f, CurrentVisualSteer, 0.0f));
	if (BarPivot)
	{
		BarPivot->SetRelativeRotation(FRotator(0.0f, CurrentVisualSteer * HandlebarSteerRatio, 0.0f));
	}
	if (RiderPivot)
	{
		RiderPivot->SetRelativeRotation(FRotator(0.0f, CurrentVisualSteer * RiderSteerRatio, 0.0f));
	}
}

void ADeliveryMotorbike::SyncControlRotation()
{
	// 镜头自己不吃控制旋转，但下车后角色的弹簧臂要用它。一路同步着车头朝向，
	// 下车那一瞬间视角就是连续的，不会突然甩回上车之前的方向。
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		PC->SetControlRotation(FRotator(CameraPitch, GetActorRotation().Yaw, 0.0f));
	}
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
