// Copyright Epic Games, Inc. All Rights Reserved.

#include "Vehicle/DeliveryMotorbike.h"
#include "AbilitySystemComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/AudioComponent.h"
#include "Audio/DeliveryVehicleAudioComponent.h"
#include "Components/BoxComponent.h"
#include "Combat/DeliveryImpactReceiver.h"
#include "Combat/DeliveryCollisionChannels.h"
#include "Vehicle/DeliveryRiderAnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Components/StaticMeshComponent.h"
#include "CollisionQueryParams.h"
#include "Delivery.h"
#include "Character/DeliveryCharacter.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Engine/CollisionProfile.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameplayEffect.h"
#include "AbilitySystem/DeliverAttributeSet.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "Grab/DeliveryGrabComponent.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "Kismet/GameplayStatics.h"
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
	// 碰撞盒跟着视觉一起缩：只缩画面不缩碰撞的话，车会在离墙一截的地方就撞上
	CollisionBox->InitBoxExtent(BaseCollisionExtent * VisualScale);
	CollisionBox->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
	// 保持 Pawn 类型：交通车靠它识别电动车；步行身体单独阻挡 Pawn。
	CollisionBox->SetCollisionObjectType(ECC_Pawn);
	CollisionBox->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
	CollisionBox->SetSimulatePhysics(false);
	CollisionBox->SetEnableGravity(false);
	RootComponent = CollisionBox;

	EngineAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("EngineAudio"));
	EngineAudio->SetupAttachment(RootComponent);
	EngineAudio->bAutoActivate = false;
    Events = CreateDefaultSubobject<UDeliveryEventComponent>(TEXT("Events"));
    VehicleAudio = CreateDefaultSubobject<UDeliveryVehicleAudioComponent>(TEXT("VehicleAudio"));

	MeshRoot = CreateDefaultSubobject<USceneComponent>(TEXT("MeshRoot"));
	MeshRoot->SetupAttachment(RootComponent);

	MeshScale = CreateDefaultSubobject<USceneComponent>(TEXT("MeshScale"));
	MeshScale->SetupAttachment(MeshRoot);

	MeshAlign = CreateDefaultSubobject<USceneComponent>(TEXT("MeshAlign"));
	MeshAlign->SetupAttachment(MeshScale);

	CargoPivot = CreateDefaultSubobject<USceneComponent>(TEXT("CargoPivot"));
	CargoPivot->SetupAttachment(MeshAlign);

	TrunkLidPivot = CreateDefaultSubobject<USceneComponent>(TEXT("TrunkLidPivot"));
	TrunkLidPivot->SetupAttachment(MeshAlign);

	TrunkAnchor = CreateDefaultSubobject<USceneComponent>(TEXT("TrunkAnchor"));
	TrunkAnchor->SetupAttachment(MeshAlign);

	SteerPivot = CreateDefaultSubobject<USceneComponent>(TEXT("SteerPivot"));
	SteerPivot->SetupAttachment(MeshAlign);

	BarPivot = CreateDefaultSubobject<USceneComponent>(TEXT("BarPivot"));
	BarPivot->SetupAttachment(MeshAlign);

	RiderPivot = CreateDefaultSubobject<USceneComponent>(TEXT("RiderPivot"));
	RiderPivot->SetupAttachment(MeshAlign);

	// 轮轴先挂在模型校正层，识别出前轮后再改挂到转向层
	
	WheelPivots.Reserve(MaxWheels);
	for (int32 Index = 0; Index < MaxWheels; ++Index)
	{
		USceneComponent* Pivot =
		    CreateDefaultSubobject<USceneComponent>(*FString::Printf(TEXT("WheelPivot%d"), Index));
		Pivot->SetupAttachment(MeshAlign);
		WheelPivots.Add(Pivot);
	}

	// 预先创建固定组件槽位，避免重跑构造脚本时产生重复组件或丢失实例配置
	
	BodyParts.Reserve(MaxBodyParts);
	for (int32 Index = 0; Index < MaxBodyParts; ++Index)
	{
		UStaticMeshComponent* Part =
		    CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("BodyPart%02d"), Index));
		Part->SetupAttachment(MeshAlign);
		// 车体只负责好看，碰撞统一交给 CollisionBox，省得几十个部件各自算碰撞
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetGenerateOverlapEvents(false);
		BodyParts.Add(Part);
	}

	RiderMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("RiderMesh"));
	RiderMesh->SetupAttachment(RiderPivot);
	RiderMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RiderMesh->SetGenerateOverlapEvents(false);
	// 动画蓝图以坐姿参考骨架为起点，程序化求身体惯性，再用 IK 固定手脚
	RiderMesh->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	RiderMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	RiderMesh->bEnableUpdateRateOptimizations = false;
	RiderMesh->SetVisibility(false);
	LeftHandGrip = CreateDefaultSubobject<USceneComponent>(TEXT("LeftHandGrip"));
	RightHandGrip = CreateDefaultSubobject<USceneComponent>(TEXT("RightHandGrip"));
	LeftFootPeg = CreateDefaultSubobject<USceneComponent>(TEXT("LeftFootPeg"));
	RightFootPeg = CreateDefaultSubobject<USceneComponent>(TEXT("RightFootPeg"));
	LeftHandGrip->SetupAttachment(BarPivot);
	RightHandGrip->SetupAttachment(BarPivot);
	LeftFootPeg->SetupAttachment(MeshAlign);
	RightFootPeg->SetupAttachment(MeshAlign);

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 650.0f;
	CameraBoom->SocketOffset = FVector(0.0f, 0.0f, 120.0f);
	// 先配置固定车尾镜头，运行时再按视角开关切换

// 镜头俯仰使用独立角度，不随车身上坡或下坡翻转
	
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

	// 后备箱是第二个交互组件，按键是左键；两个组件在同一个 Actor 上，
	// 所以服务器复核目标时必须按"键"去找组件，不能拿第一个（见 FindOn 的键重载）
	TrunkInteractable = CreateDefaultSubobject<UDeliveryInteractableComponent>(TEXT("TrunkInteractable"));
	TrunkInteractable->InteractionKey = EDeliveryInteractionKey::ActionLeft;
	TrunkInteractable->InteractRadius = TrunkInteractRadius;
	TrunkInteractable->HoldDuration = 0.0f;
	TrunkInteractable->PromptOffset = FVector(0.0f, 0.0f, 60.0f);

	DrivePromptText = NSLOCTEXT("Delivery", "MotorbikeDrivePrompt", "按 F 驾驶");
	ExitPromptText = NSLOCTEXT("Delivery", "MotorbikeExitPrompt", "按 F 下车");
	TrunkLoadPromptText = NSLOCTEXT("Delivery", "MotorbikeTrunkLoad", "左键装货");
	TrunkUnloadPromptText = NSLOCTEXT("Delivery", "MotorbikeTrunkUnload", "左键取货");

	static ConstructorHelpers::FObjectFinder<UInputAction> MoveActionFinder(
	    TEXT("/Game/Input/Actions/IA_Move"));
	if (MoveActionFinder.Succeeded())
	{
		MoveAction = MoveActionFinder.Object;
	}

	// 和角色身上用的是同两个 IA，鼠标/手柄灵敏度手感一致，也省得再建资产
	static ConstructorHelpers::FObjectFinder<UInputAction> LookActionFinder(
	    TEXT("/Game/Input/Actions/IA_Look"));
	if (LookActionFinder.Succeeded())
	{
		LookAction = LookActionFinder.Object;
	}
	static ConstructorHelpers::FObjectFinder<UInputAction> MouseLookActionFinder(
	    TEXT("/Game/Input/Actions/IA_MouseLook"));
	if (MouseLookActionFinder.Succeeded())
	{
		MouseLookAction = MouseLookActionFinder.Object;
	}
	InteractAction =
	    TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/IA_Interact.IA_Interact")));
	PickupAction =
	    TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/IA_Pickup.IA_Pickup")));
	CameraToggleAction = TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/IA_ToggleCamera.IA_ToggleCamera")));
	RiderMeshAsset = TSoftObjectPtr<USkeletalMesh>(
	    FSoftObjectPath(TEXT("/Game/Vehicle/Motorbike/SK_MotorbikeRider.SK_MotorbikeRider")));
}

// 同步驾驶员、货物和车辆运动所需状态
void ADeliveryMotorbike::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ADeliveryMotorbike, Driver);
	DOREPLIFETIME(ADeliveryMotorbike, Cargo);
	DOREPLIFETIME(ADeliveryMotorbike, ReplicatedRiderMotion);
}

// 在编辑器中根据配置更新车辆外观
void ADeliveryMotorbike::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ApplyBodyMeshes();
	if (bAutoCalibrateRiderContacts)
		CalibrateRiderContacts();
}

// 车辆初始化

// 准备车辆网格、驾驶交互和运行时状态
void ADeliveryMotorbike::BeginPlay()
{
	Super::BeginPlay();
	// 交通车靠 Pawn 类型识别电动车，旧蓝图实例也在运行时明确对齐。
	CollisionBox->SetCollisionObjectType(ECC_Pawn);
	CollisionBox->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
	ApplyBodyMeshes();
	if (bAutoCalibrateRiderContacts)
		CalibrateRiderContacts();
	RiderMesh->SetAnimInstanceClass(RiderAnimationClass ? RiderAnimationClass.Get()
	                                                    : UDeliveryRiderAnimInstance::StaticClass());
	// 先算完车体/车把的变换，再让动画读取接触点，避免手比车把慢一帧
	RiderMesh->AddTickPrerequisiteActor(this);

	if (Interactable)
	{
		Interactable->PromptText = DrivePromptText;
		Interactable->OnInteract.AddDynamic(this, &ADeliveryMotorbike::HandleInteractRequested);
		// 手上有快递时不让直接上车：提示和按键读同一个条件，不会一个说能一个不能
		Interactable->CanUse.BindLambda(
		    [this](APawn* Interactor, bool) { return CanDriveNow(Cast<ADeliveryCharacter>(Interactor)); });
	}
	if (TrunkInteractable)
	{
		TrunkInteractable->CanUse.BindUObject(this, &ADeliveryMotorbike::CanUseTrunk);
		TrunkInteractable->Use.BindUObject(this, &ADeliveryMotorbike::UseTrunk);
	}

	ApplyTrunkPresentation();
	ApplyDriverPresentation();
	ApplyCameraMode();
}

// 根据车身和车轮网格确定车辆尺寸与显示位置
void ADeliveryMotorbike::AutoConfigureFromMeshes()
{
	// 骑手组件未配置模型时，按需加载备用模型
	
	if (RiderMesh && !RiderMesh->GetSkinnedAsset())
	{
		if (USkeletalMesh* Loaded = RiderMeshAsset.LoadSynchronous())
		{
			RiderMesh->SetSkinnedAssetAndUpdate(Loaded);
			UE_LOG(LogDelivery, Log, TEXT("%s：骑手网格是按软引用兜底加载的（%s）。"), *GetName(),
			       *Loaded->GetName());
		}
	}

	// 仅自动补全未配置的参数，保留手动设置
	const bool bNeedWheels = WheelPartIndices.Num() == 0;
	const bool bNeedSteer = SteeringPartIndices.Num() == 0 && HandlebarPartIndices.Num() == 0;
	const bool bNeedTrunk = TrunkInsideOffset.IsNearlyZero() || TrunkLidPartIndex == INDEX_NONE ||
	                        TrunkLidHinge.IsNearlyZero() || TrunkOpeningOffset.IsNearlyZero();
	if (!bNeedWheels && !bNeedSteer && !bNeedTrunk)
	{
		return;
	}

	// 导入部件保留整车坐标，可直接用包围盒识别部件位置
	
	TArray<FBox> Boxes;
	Boxes.SetNum(BodyMeshes.Num());
	FBox Whole(ForceInit);
	int32 ValidCount = 0;
	for (int32 Index = 0; Index < BodyMeshes.Num(); ++Index)
	{
		if (const UStaticMesh* Mesh = BodyMeshes[Index].Get())
		{
			Boxes[Index] = Mesh->GetBoundingBox();
			Whole += Boxes[Index];
			++ValidCount;
		}
		else
		{
			Boxes[Index] = FBox(ForceInit);
		}
	}
	if (ValidCount == 0)
	{
		return;
	}

	if (bNeedWheels)
	{
		// 轮子需同时满足接近圆形且厚度较小，避免将车架误识别为轮子
		
		float RadiusSum = 0.0f;
		for (int32 Index = 0; Index < Boxes.Num(); ++Index)
		{
			if (!Boxes[Index].IsValid)
			{
				continue;
			}
			const FVector Size = Boxes[Index].GetSize();
			const float Diameter = FMath::Max(Size.Y, Size.Z);
			if (Diameter <= KINDA_SMALL_NUMBER)
			{
				continue;
			}
			const float Roundness = FMath::Min(Size.Y, Size.Z) / Diameter;
			if (Roundness > 0.9f && Size.X < Diameter * 0.6f)
			{
				WheelPartIndices.Add(Index);
				WheelCenters.Add(Boxes[Index].GetCenter());
				RadiusSum += Diameter * 0.5f;
			}
		}
		if (WheelPartIndices.Num() > 0)
		{
			WheelRadius = RadiusSum / WheelPartIndices.Num();
			UE_LOG(LogDelivery, Log, TEXT("%s：自动认出 %d 个轮子，半径 %.0fcm。"), *GetName(),
			       WheelPartIndices.Num(), WheelRadius);
		}
	}

	if (bNeedSteer)
	{
		// 将横向最宽的部件识别为车把
		int32 BarIndex = INDEX_NONE;
		float BestWidth = -1.0f;
		int32 FrontMost = INDEX_NONE;
		float BestFrontY = -FLT_MAX;
		for (int32 Index = 0; Index < Boxes.Num(); ++Index)
		{
			if (!Boxes[Index].IsValid)
			{
				continue;
			}
			const float Width = Boxes[Index].GetSize().X;
			if (Width > BestWidth)
			{
				BestWidth = Width;
				BarIndex = Index;
			}
			const float CenterY = Boxes[Index].GetCenter().Y;
			if (CenterY > BestFrontY)
			{
				BestFrontY = CenterY;
				FrontMost = Index;
			}
		}
		if (BarIndex != INDEX_NONE && FrontMost != INDEX_NONE)
		{
			// 前轮/前叉 = 包围盒中心落在车身前四分之一的部件；导入空间里车头朝 +Y
			const float FrontLine = Whole.Min.Y + Whole.GetSize().Y * 0.75f;
			for (int32 Index = 0; Index < Boxes.Num(); ++Index)
			{
				if (Boxes[Index].IsValid && Index != BarIndex && Boxes[Index].GetCenter().Y >= FrontLine)
				{
					SteeringPartIndices.Add(Index);
				}
			}
			HandlebarPartIndices.Add(BarIndex);

			// 转向轴取"车把中心"和"最前部件中心"的水平中点，大致就是前叉的位置
			// 用竖直轴而不是带后倾角的真实转向轴：这个尺寸下看不出差别，且省一层旋转补偿
			const FVector BarCenter = Boxes[BarIndex].GetCenter();
			const FVector FrontCenter = Boxes[FrontMost].GetCenter();
			SteerPivotLocation = FVector((BarCenter.X + FrontCenter.X) * 0.5f,
			                             (BarCenter.Y + FrontCenter.Y) * 0.5f, BarCenter.Z);
			UE_LOG(LogDelivery, Log, TEXT("%s：自动认出车把槽位 %d（宽 %.0fcm）、跟转槽位 %d 个。"),
			       *GetName(), BarIndex, BestWidth, SteeringPartIndices.Num());
		}
	}

	if (bNeedTrunk)
	{
		/**
		 * 认后备箱：箱盖 = 车尾那块"最高的扁平件"，箱体 = 紧贴它下面、平面上和它重叠的那块
		 *
		 * 判据只用包围盒，和认轮子/车把一样——这 9 个网格的名字是
		 * `摩托车_车_001`…`_008`，名字里没有任何信息可用。
		 * 实测这辆车：箱盖是槽位 1（79×63×14，顶面 z=93，整车最高）、
		 * 箱体是槽位 7（69×56×56，顶面 z=80，正好接在箱盖下面）。
		 */
		const float RearLine = Whole.Min.Y + Whole.GetSize().Y * 0.4f;
		int32 LidIndex = TrunkLidPartIndex;
		if (LidIndex == INDEX_NONE)
		{
			float BestTop = -FLT_MAX;
			for (int32 Index = 0; Index < Boxes.Num(); ++Index)
			{
				if (!Boxes[Index].IsValid || WheelPartIndices.Contains(Index) ||
				    SteeringPartIndices.Contains(Index) || HandlebarPartIndices.Contains(Index))
				{
					continue;
				}
				const FVector Size = Boxes[Index].GetSize();
				const float Footprint = FMath::Min(Size.X, Size.Y);
				// 扁平：厚度不到平面尺寸的四成；车架、箱体这些都过不了这一条
				if (Footprint <= KINDA_SMALL_NUMBER || Size.Z > Footprint * 0.4f)
				{
					continue;
				}
				if (Boxes[Index].GetCenter().Y <= RearLine && Boxes[Index].Max.Z > BestTop)
				{
					BestTop = Boxes[Index].Max.Z;
					LidIndex = Index;
				}
			}
		}

		int32 BoxIndex = TrunkBoxPartIndex;
		if (BoxIndex == INDEX_NONE && Boxes.IsValidIndex(LidIndex))
		{
			const FBox& Lid = Boxes[LidIndex];
			float BestTop = -FLT_MAX;
			for (int32 Index = 0; Index < Boxes.Num(); ++Index)
			{
				if (Index == LidIndex || !Boxes[Index].IsValid || WheelPartIndices.Contains(Index))
				{
					continue;
				}
				const FBox& Candidate = Boxes[Index];
				// 平面上要和箱盖重叠，而且顶面在箱盖下面——"盖子底下那一件"
				const bool bOverlapXY = Candidate.Min.X <= Lid.Max.X && Candidate.Max.X >= Lid.Min.X &&
				                        Candidate.Min.Y <= Lid.Max.Y && Candidate.Max.Y >= Lid.Min.Y;
				if (bOverlapXY && Candidate.Max.Z <= Lid.Max.Z && Candidate.Max.Z > BestTop)
				{
					BestTop = Candidate.Max.Z;
					BoxIndex = Index;
				}
			}
		}

		if (Boxes.IsValidIndex(LidIndex))
		{
			const FBox& Lid = Boxes[LidIndex];
			const FBox& Container = Boxes.IsValidIndex(BoxIndex) ? Boxes[BoxIndex] : Lid;
			if (TrunkLidPartIndex == INDEX_NONE)
			{
				TrunkLidPartIndex = LidIndex;
			}
			if (TrunkBoxPartIndex == INDEX_NONE)
			{
				TrunkBoxPartIndex = BoxIndex;
			}
			if (TrunkLidHinge.IsNearlyZero())
			{
				/**
				 * 铰链在箱盖的一条边的底面：局部 +Y 朝车头，所以前沿是 Max.Y、后沿是 Min.Y
				 *
				 * 哪条边由 bTrunkLidHingeAtFront 决定，旋转方向读的也是同一个开关
				 * （见 GetTrunkLidLiftDirection），所以不会配出"铰链在后沿却往后转"
				 * 这种把盖子转进车身的组合。
				 */
				TrunkLidHinge = FVector(Lid.GetCenter().X,
				                        bTrunkLidHingeAtFront ? Lid.Max.Y : Lid.Min.Y, Lid.Min.Z);
			}
			if (TrunkInsideOffset.IsNearlyZero())
			{
				TrunkInsideOffset = Container.GetCenter();
			}
			if (TrunkOpeningOffset.IsNearlyZero())
			{
				// 箱口上方：装卸动画里快递要从这儿看得见地落进去/升出来
				TrunkOpeningOffset =
				    FVector(Container.GetCenter().X, Container.GetCenter().Y, Lid.Max.Z + 25.0f);
			}
			UE_LOG(LogDelivery, Log,
			       TEXT("%s：自动认出后备箱——箱盖槽位 %d、箱体槽位 %d，铰链 (%.0f, %.0f, %.0f)，")
			           TEXT("箱内 (%.0f, %.0f, %.0f)。"),
			       *GetName(), TrunkLidPartIndex, TrunkBoxPartIndex, TrunkLidHinge.X, TrunkLidHinge.Y,
			       TrunkLidHinge.Z, TrunkInsideOffset.X, TrunkInsideOffset.Y, TrunkInsideOffset.Z);
		}
		else
		{
			// 认不出来就退回整车尾部，盖子不动——"盖子乱翻"比"盖子不动"难查得多
			if (TrunkInsideOffset.IsNearlyZero())
			{
				TrunkInsideOffset = FVector(Whole.GetCenter().X, RearLine, Whole.Max.Z);
			}
			if (TrunkOpeningOffset.IsNearlyZero())
			{
				TrunkOpeningOffset = TrunkInsideOffset + FVector(0.0f, 0.0f, 40.0f);
			}
			UE_LOG(LogDelivery, Warning,
			       TEXT("%s：没认出后备箱的箱盖（车尾没有扁平件），装卸货仍然可用但盖子不会开合。"),
			       *GetName());
		}
	}
}

// 应用车身、车轮和骑手的资源配置
void ADeliveryMotorbike::ApplyBodyMeshes()
{
	AutoConfigureFromMeshes();

	// 将挂点移到旋转中心，再反向补偿子模型位置，保持模型原位
	
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
	// 快递挂点在箱子里面，不需要对子物体抵消位置偏移
	
	if (CargoPivot)
	{
		CargoPivot->SetRelativeLocationAndRotation(TrunkInsideOffset, CargoRotation);
	}
	if (TrunkLidPivot)
	{
		TrunkLidPivot->SetRelativeLocation(TrunkLidHinge);
	}
	if (TrunkAnchor)
	{
		TrunkAnchor->SetRelativeLocation(TrunkInsideOffset);
	}
	if (TrunkInteractable)
	{
		// 交互距离从箱子量起：车辆原点在车身中间，按它算的话站在车尾也显得老远
		TrunkInteractable->InteractAnchor = TrunkAnchor;
		TrunkInteractable->InteractRadius = TrunkInteractRadius;
	}
	ApplyVisualScale();
	if (RiderMesh)
	{
		// 显式更新骑手的附着关系，兼容已有蓝图实例保存的层级
		
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

		// 箱盖挂在自己的铰链层上，开盖时绕铰链转
		if (TrunkLidPivot && Index == TrunkLidPartIndex)
		{
			if (Part->GetAttachParent() != TrunkLidPivot)
			{
				Part->AttachToComponent(TrunkLidPivot,
				                        FAttachmentTransformRules::KeepRelativeTransform);
			}
			// 和轮子同一个手法：挂点挪到旋转中心，网格把这段偏移减回去
			Part->SetRelativeLocation(-TrunkLidHinge);
			Part->SetRelativeRotation(FRotator::ZeroRotator);
			continue;
		}

		// 前轮挂在转向层下，同时支持转向和绕轮心自转
		
		const int32 WheelSlot = WheelPartIndices.IndexOfByKey(Index);
		if (WheelSlot != INDEX_NONE && WheelPivots.IsValidIndex(WheelSlot) &&
		    WheelCenters.IsValidIndex(WheelSlot))
		{
			USceneComponent* Pivot = WheelPivots[WheelSlot];
			const FVector Center = WheelCenters[WheelSlot];
			if (Pivot)
			{
				if (Pivot->GetAttachParent() != Target)
				{
					Pivot->AttachToComponent(Target, FAttachmentTransformRules::KeepRelativeTransform);
				}
				// 轮轴在父级里的位置：挂在 SteerPivot 下时父级原点已经是转向轴，要减掉
				Pivot->SetRelativeLocation(bOnSteerAxis ? Center - SteerPivotLocation : Center);

				if (Part->GetAttachParent() != Pivot)
				{
					Part->AttachToComponent(Pivot, FAttachmentTransformRules::KeepRelativeTransform);
				}
				// 网格把轮心这段偏移减回去，顶点仍落在导入空间原处
				Part->SetRelativeLocation(-Center);
				Part->SetRelativeRotation(FRotator::ZeroRotator);
				continue;
			}
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
		UE_LOG(LogDelivery, Warning, TEXT("%s：车体网格有 %d 个，超过了 %d 个固定槽位，多出来的不会显示。"),
		       *GetName(), BodyMeshes.Num(), MaxBodyParts);
	}
}

// 根据是否有驾驶员更新角色隐藏和骑手显示
void ADeliveryMotorbike::ApplyDriverPresentation()
{
	const bool bHasDriver = Driver != nullptr;
	if (RiderMesh)
	{
		RiderMesh->SetVisibility(bHasDriver);
	}
	if (Interactable)
	{
		// 有人骑着的时候别人不该再看到"按 F 驾驶"
		Interactable->bInteractEnabled = !bHasDriver;
	}
	ApplyTrunkPresentation();
	ApplyRiderMaterials();
}

// 将驾驶员材质应用到车辆上的骑手模型
void ADeliveryMotorbike::ApplyRiderMaterials()
{
	if (!RiderMesh)
	{
		return;
	}

	// 无人驾驶或关闭材质同步时，清除覆盖材质并恢复模型默认材质
	
	if (!Driver || !bUseDriverMaterials)
	{
		RiderMesh->EmptyOverrideMaterials();
		return;
	}

	USkeletalMeshComponent* DriverMesh = Driver->GetMesh();
	if (!DriverMesh)
	{
		return;
	}

	// 优先按材质槽名称匹配驾驶员和骑手，避免槽位顺序不同导致贴错材质

const TArray<FName> DriverSlots = DriverMesh->GetMaterialSlotNames();
	const TArray<FName> RiderSlots = RiderMesh->GetMaterialSlotNames();

	for (int32 Index = 0; Index < RiderSlots.Num(); ++Index)
	{
		UMaterialInterface* Chosen =
		    RiderMaterialOverrides.IsValidIndex(Index) ? RiderMaterialOverrides[Index].Get() : nullptr;

		if (!Chosen)
		{
			const int32 DriverIndex = DriverSlots.IndexOfByKey(RiderSlots[Index]);
			// 槽位名称不匹配时，尝试使用相同下标的材质
			
			Chosen = DriverMesh->GetMaterial(DriverIndex != INDEX_NONE ? DriverIndex : Index);
		}

		if (Chosen)
		{
			RiderMesh->SetMaterial(Index, Chosen);
		}
	}
}

// 驾驶员与货物同步

void ADeliveryMotorbike::OnRep_Driver()
{
	ApplyDriverPresentation();
}

void ADeliveryMotorbike::OnRep_Cargo()
{
	ApplyCargoPresentation();
}

// 把当前快递固定到箱内挂点；装好之后整件藏起来
void ADeliveryMotorbike::ApplyCargoPresentation()
{
	if (Cargo && CargoPivot)
	{
		Cargo->SetVehicleCargoPresentation(CargoPivot);
		/**
		 * 箱子关着的时候把快递藏起来
		 *
		 * 它在一个关好的箱子里，看不见才是对的；而且快递比箱子内腔大时
		 * 不藏就会穿出箱盖。装卸动画期间由 UpdateTrunkAnimation 临时显示。
		 */
		Cargo->SetActorHiddenInGame(TrunkPhase == EDeliveryTrunkPhase::Idle);
		Cargo->SetActorRelativeLocation(FVector::ZeroVector);
	}
	ApplyTrunkPresentation();
}

// 返回仍然有效的货架快递
ADeliveryPackageItem* ADeliveryMotorbike::GetCargo() const
{
	// 有效性检查需要快递类的完整类型，因此实现放在此文件
	
	return IsValid(Cargo) ? Cargo.Get() : nullptr;
}

// 在服务器登记货物归属并固定到货架
bool ADeliveryMotorbike::AttachCargo(ADeliveryPackageItem* Package)
{
	if (!HasAuthority() || HasCargo() || !IsValid(Package) || !CargoPivot)
	{
		return false;
	}

	Cargo = Package;
	// 将快递归属设为车辆，防止被当作无人持有的地面物品
	
	Package->SetOwner(this);
	ApplyCargoPresentation();

	return true;
}

// 清空货架并解除物品归属，后续去向交给调用方处理
ADeliveryPackageItem* ADeliveryMotorbike::ReleaseCargo()
{
	ADeliveryPackageItem* Released = GetCargo();
	// 清除已失效的快递引用，允许车辆接收下一件快递
	Cargo = nullptr;
	if (!HasAuthority() || !Released)
	{
		return nullptr;
	}
	Released->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	Released->SetOwner(nullptr);

	// 只解除车辆关系，后续位置和物理状态由接收快递的调用方决定
	
	return Released;
}

void ADeliveryMotorbike::HandleInteractRequested(APawn* Interactor)
{
	TryEnter(Cast<ADeliveryCharacter>(Interactor));
}

// 上车与驾驶权切换

// 检查驾驶员条件，转移快递和控制权，再切换骑手表现
bool ADeliveryMotorbike::TryEnter(ADeliveryCharacter* NewDriver)
{
	if (!HasAuthority() || Driver || !IsValid(NewDriver))
	{
		return false;
	}

	// 服务器复核上车条件：晕倒、手上还拿着快递、正在播装卸动画都不给上车
	
	if (!CanDriveNow(NewDriver))
	{
		return false;
	}

	AController* DriverController = NewDriver->GetController();
	if (!DriverController)
	{
		return false;
	}

	Driver = NewDriver;
	ResetRiderMotion();

	// 上车前释放抓取约束，避免停用身体物理后继续拖住其他物体
	
	if (UDeliveryGrabComponent* Grab = NewDriver->GetGrabComponent())
	{
		Grab->ForceRelease();
	}

	// 先停止身体物理再隐藏角色，避免角色附着到车辆后仍独立模拟
	
	if (UDeliveryActiveRagdollComponent* Ragdoll = NewDriver->GetActiveRagdoll())
	{
		Ragdoll->StopRagdoll();
	}
	NewDriver->SetActorEnableCollision(false);
	NewDriver->SetActorHiddenInGame(true);
	NewDriver->AttachToComponent(CollisionBox, FAttachmentTransformRules::SnapToTargetNotIncludingScale);

	/**
	 * 上车不再顺手把手上的快递搬到车上
	 *
	 * 装货现在是一个独立动作（走到车尾按左键、开盖、放进去、合盖），
	 * 所以手上有货的人根本进不到这里——CanDriveNow 已经把他挡在外面了。
	 * 这两件事必须一起改：只加装货动作、不去掉这里的自动搬运，
	 * 表现就是"按 F 直接把货带上车"，那个新动作永远不会被用到。
	 */
	DriverController->Possess(this);
	if (APlayerController* PC = Cast<APlayerController>(DriverController))
	{
		// 先将镜头对齐车尾再过渡，上车后从这一方向开始观察
		
		ApplyCameraMode();
		PC->SetControlRotation(FRotator(CameraPitch, GetActorRotation().Yaw, 0.0f));
		PC->SetViewTargetWithBlend(this, 0.35f);
	}

	ThrottleInput = 0.0f;
	SteerInput = 0.0f;
	MoveInputRaw = FVector2D::ZeroVector;
	TurnAroundSign = 0.0f;
	bBrakingForTurn = false;
	TrafficImpactCount = 0;
	LastImpactTime = -1000.0f;
	ApplyDriverPresentation();
	MulticastVehicleEvent(EDeliveryEvent::VehicleEntered);
	return true;
}

// 召唤与车辆复位

// 把无人驾驶的车辆放到经过检查的召唤位置
bool ADeliveryMotorbike::SummonTo(const FVector& DesiredLocation, float DesiredYaw)
{
	// 召唤是瞬移：清掉扫描锚点，下一帧重新建立。
	// 不清的话那一帧会算出一段巨大的"路径"，把沿途的路人全部撞飞
	bHasPedestrianSweepLocation = false;
	UWorld* World = GetWorld();
	if (!HasAuthority() || !World || Driver)
	{
		return false;
	}

	// 召唤时先探测地面，再确定车辆高度，避免生成在地下或空中
	
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MotorbikeSummon), false, this);
	FHitResult GroundHit;
	const bool bHit = World->LineTraceSingleByChannel(
	    GroundHit, DesiredLocation + FVector(0.0f, 0.0f, GroundTraceUp + 200.0f),
	    DesiredLocation - FVector(0.0f, 0.0f, GroundTraceDown), ECC_Visibility, Params);
	if (!bHit)
	{
		return false;
	}

	const FVector Target(DesiredLocation.X, DesiredLocation.Y, GroundHit.ImpactPoint.Z + HoverHeight);
	SetActorLocationAndRotation(Target, FRotator(0.0f, DesiredYaw, 0.0f), false, nullptr,
	                            ETeleportType::TeleportPhysics);

	// 清除旧车速和撞击状态，避免召唤后继续滑动或倾斜
	
	CurrentSpeed = 0.0f;
	VerticalVelocity = 0.0f;
	ThrottleInput = 0.0f;
	SteerInput = 0.0f;
	MoveInputRaw = FVector2D::ZeroVector;
	TurnAroundSign = 0.0f;
	bBrakingForTurn = false;
	KnockVelocity = FVector::ZeroVector;
	KnockYawRate = 0.0f;
	KnockTilt = 0.0f;
	ShakeAmount = 0.0f;
	TrafficImpactCount = 0;
	LastImpactTime = -1000.0f;
	bWheelLocationValid = false;

	UE_LOG(LogDelivery, Log, TEXT("%s：被召唤到 (%.0f, %.0f, %.0f)"), *GetName(), Target.X, Target.Y,
	       Target.Z);
	MulticastVehicleEvent(EDeliveryEvent::VehicleSummoned);
	return true;
}

// 交通撞击与摔落

// 记录交通撞击，更新车辆失控和驾驶员受击
bool ADeliveryMotorbike::NotifyTrafficImpact(const FVector& CarVelocity)
{
	// 空车不累计撞击次数
	if (!HasAuthority() || !Driver)
	{
		return false;
	}

	// 先检查撞击间隔，再计数和播放反馈，避免多辆车同时碰撞造成重复结算
	
	
	if (const UWorld* World = GetWorld())
	{
		const float Now = World->GetTimeSeconds();
		if (MinImpactInterval > 0.0f && Now - LastImpactTime < MinImpactInterval)
		{
			UE_LOG(LogDelivery, Verbose, TEXT("%s：撞击被最小间隔挡掉（距上次 %.2f 秒 < %.2f 秒）"),
			       *GetName(), Now - LastImpactTime, MinImpactInterval);
			return false;
		}
		LastImpactTime = Now;
	}

	const float ImpactSpeed = CarVelocity.Size2D();
	MulticastVehicleEvent(EDeliveryEvent::VehicleImpacted);
	++TrafficImpactCount;
	UE_LOG(LogDelivery, Log, TEXT("%s：被交通车撞击 %d/%d 次（来车速度 %.0f cm/s）"), *GetName(),
	       TrafficImpactCount, ImpactsToDismount, ImpactSpeed);

	const FVector Direction = CarVelocity.GetSafeNormal2D();
	RiderImpact = FVector2D(FVector::DotProduct(Direction, GetActorForwardVector()),
	                        FVector::DotProduct(Direction, GetActorRightVector())) *
	              FMath::Clamp(ImpactSpeed / FMath::Max(KnockDownSpeedReference, 1.f), .2f, 1.f);
	ReplicatedRiderMotion.Impact = RiderImpact;
	ForceNetUpdate();
	if (!Direction.IsNearlyZero())
	{
		// 被撞歪的正负取"来车方向相对车身的左右分量"：从右边撞来就往左歪，反之亦然
		const float Side = FVector::DotProduct(Direction, GetActorRightVector());

		KnockVelocity += Direction * FMath::Min(ImpactSpeed * KnockbackFraction, MaxKnockbackSpeed);
		KnockYawRate += Side * KnockYawPerHit;
		// UE 里正 Roll 是往左倒；从右边撞来（Side<0）应该往左倒，所以取负号
		KnockTilt += -Side * KnockTiltPerHit;
		ShakeAmount = 1.0f;
	}

	ADeliveryCharacter* Rider = Driver;
	if (TrafficImpactCount >= ImpactsToDismount)
	{
		// 车会被击退推向 Side 那一侧，人就放到相反的一侧，免得车追上去压住他
		const float Side =
		    Direction.IsNearlyZero() ? 0.0f : FVector::DotProduct(Direction, GetActorRightVector());
		const float AwaySign = (FMath::Abs(Side) > 0.1f) ? -FMath::Sign(Side) : 0.0f;
		PendingExitSideSign = AwaySign;

		// 和按 F 下车走同一条路：人放回车边地面、镜头切回角色、布娃娃重新站起来
		ExitVehicle();

		// 车不许再动：残余的车速和击退速度会让它继续朝人的方向滑过去
		CurrentSpeed = 0.0f;
		KnockVelocity = FVector::ZeroVector;
		KnockYawRate = 0.0f;

		if (bKnockDownDriverOnDismount)
		{
			// 先完成下车和身体重建，再扣血并施加向车侧的冲量

FVector LaunchDir = Direction;
			if (!FMath::IsNearlyZero(AwaySign))
			{
				LaunchDir = (GetActorRightVector() * AwaySign * 1.6f + Direction).GetSafeNormal2D();
			}
			// 撞得越快飞得越远，但保底也要飞一点：慢车蹭一下不该看起来像自己躺下的
			const float LaunchScale = FMath::Lerp(
			    KnockDownLaunchMinScale, 1.0f,
			    FMath::Clamp(ImpactSpeed / FMath::Max(KnockDownSpeedReference, 1.0f), 0.0f, 1.0f));
			KnockDownDriver(Rider, LaunchDir, LaunchScale);
		}
	}
	return true;
}

// 让驾驶员脱离车辆，并施加倒地冲量
void ADeliveryMotorbike::KnockDownDriver(ADeliveryCharacter* Rider, const FVector& LaunchDirection,
                                         float LaunchScale)
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

	// 优先走既有的伤害 GE，和"被车撞倒"保持完全同一条结算路径
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
	// 检查伤害结算后的生命值，必要时补足扣减，避免回复效果抵消本次击倒
	
	if (ASC->GetNumericAttribute(HealthAttribute) > 0.5f)
	{
		ASC->SetNumericAttributeBase(HealthAttribute, 0.0f);
	}

	// 生命值归零后由技能组件进入晕倒状态，再将身体推离车辆
	
	const FVector Direction = LaunchDirection.GetSafeNormal2D();
	if (!Direction.IsNearlyZero())
	{
		const float Scale = FMath::Clamp(LaunchScale, 0.0f, 1.0f);
		const FVector Launch =
		    (Direction * KnockDownLaunchSpeed + FVector::UpVector * KnockDownLaunchUp) * Scale;
		// 将速度变化应用到全身，避免只推髋部后被关节约束分摊
		
		Mesh->AddImpulse(Launch, TEXT("Hips"), true);
		Mesh->AddImpulseToAllBodiesBelow(FVector::UpVector * KnockDownLaunchUp * 0.5f, TEXT("Hips"), true,
		                                 true);
	}

	// 镜头已切回角色，使用角色的撞击镜头反馈
	
	Rider->NotifyVehicleImpact();

	UE_LOG(LogDelivery, Log, TEXT("%s：驾驶员 %s 被撞下车并打晕（抛射力度 %.0f%%）"), *GetName(),
	       *GetNameSafe(Rider), FMath::Clamp(LaunchScale, 0.0f, 1.0f) * 100.0f);
}

// 逐步更新车辆受撞后的偏移和恢复
// 服务器沿本帧路径扫路人，撞到就打飞
void ADeliveryMotorbike::ProcessPedestrianImpacts(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (!World || PedestrianImpactImpulse <= 0.0f || !CollisionBox)
	{
		return;
	}

	const FVector Current = GetActorLocation();
	if (!bHasPedestrianSweepLocation)
	{
		PreviousPedestrianSweepLocation = Current;
		bHasPedestrianSweepLocation = true;
		return;
	}

	const FVector Start = PreviousPedestrianSweepLocation;
	const FVector Travel = Current - Start;
	PreviousPedestrianSweepLocation = Current;

	// 按 R 召唤会把车瞬移过来，那一帧的位移远大于车速能走的距离。
	// 不挡住的话被召唤的车会把沿途（其实根本没经过）的路人全部撞飞。
	// 交通车那边是同一类问题（OnRouteLost 瞬移那一帧算出假的 3053 cm/s）
	const float MaxTravel = FMath::Max(FMath::Abs(CurrentSpeed), MaxSpeed) * DeltaSeconds + 50.0f;
	if (Travel.Size() > MaxTravel)
	{
		return;
	}

	// 停着或者慢慢挪不撞人：蹭一下就把人撞飞很出戏
	if (FMath::Abs(CurrentSpeed) < PedestrianImpactMinSpeed)
	{
		return;
	}

	const FVector Direction = (FMath::Abs(Travel.SizeSquared2D()) > 1.0f)
	                              ? Travel.GetSafeNormal2D()
	                              : GetActorForwardVector().GetSafeNormal2D();
	if (Direction.IsNearlyZero())
	{
		return;
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(MotorbikePedestrianImpact), false, this);
	Params.AddIgnoredActor(this);
	// 驾驶员那具身体被挂在车上，扫到它没有意义
	Params.AddIgnoredActor(Driver);

	// 按**物体类型**查：路人通道的默认响应是 Ignore，按通道查会被响应筛掉、
	// 扫描恒为空（避让检测当年就是这么死的）
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(DeliveryCollision::Pedestrian);

	TArray<FHitResult> Hits;
	World->SweepMultiByObjectType(Hits, Start, Current, GetActorQuat(), Objects,
	                              FCollisionShape::MakeBox(CollisionBox->GetScaledBoxExtent()),
	                              Params);

	// 一次扫描会碰到同一个人的多个部位（躺下之后更是每根骨头一个刚体）
	TSet<AActor*> Seen;
	for (const FHitResult& Hit : Hits)
	{
		AActor* HitActor = Hit.GetActor();
		if (!HitActor || Seen.Contains(HitActor))
		{
			continue;
		}
		Seen.Add(HitActor);

		// 判据是"实现了 IDeliveryImpactReceiver"，不是 Cast<ADeliveryPedestrian>：
		// 后者会让 Vehicle/ 反向依赖 NPC/，而且同一个判断要在撞车和出拳两处各写一遍
		IDeliveryImpactReceiver* Receiver = Cast<IDeliveryImpactReceiver>(HitActor);
		if (!Receiver || Receiver->IsKnockedDown())
		{
			continue;
		}

		// 力度按车速缩放，但留一个下限：慢车蹭一下人是原地瘫软，看着像自己躺下的
		const float SpeedScale =
		    FMath::Clamp(FMath::Abs(CurrentSpeed) / FMath::Max(PedestrianImpactSpeedReference, 1.0f),
		                 PedestrianImpactMinScale, 1.0f);
		const FVector Impulse =
		    (Direction + FVector::UpVector * PedestrianImpactUpwardFraction).GetSafeNormal() *
		    PedestrianImpactImpulse * SpeedScale;
		Receiver->ReceiveImpact(Impulse, Hit.ImpactPoint);
	}
}

void ADeliveryMotorbike::UpdateImpactReaction(float DeltaSeconds)
{
	// 统一的指数衰减：撞击是个尖峰，之后一路平滑回到 0
	const float Decay = FMath::Exp(-KnockDecay * DeltaSeconds);

	// 只有自己在跑运动的那一端才推，否则会和复制过来的位置打架
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
		// 两个轴用不同频率，不然抖动看着像单纯在点头
		const float Roll = FMath::Sin(ShakePhase * 1.7f) * ShakeAngle * ShakeAmount;
		if (bFreeLookCamera)
		{
			// 自由视角的水平和俯仰由玩家控制，仅叠加侧倾震动
			
			
			CameraBoom->SetRelativeRotation(FRotator(0.0f, 0.0f, Roll));
		}
		else
		{
			const float Pitch = CameraPitch + FMath::Sin(ShakePhase) * ShakeAngle * ShakeAmount;
			CameraBoom->SetRelativeRotation(FRotator(Pitch, 0.0f, Roll));
		}
	}
}

// 下车与角色控制恢复

// 寻找下车位置，恢复角色与物品并交回控制权
void ADeliveryMotorbike::ExitVehicle()
{
	if (!HasAuthority() || !Driver)
	{
		return;
	}

	// 落点要在清掉 Driver 之前算：FindExitLocation 会把驾驶员排除出扫描
	const FVector ExitLocation = FindExitLocation(PendingExitSideSign);
	PendingExitSideSign = 0.0f;

	ADeliveryCharacter* Leaving = Driver;
	Driver = nullptr;
	ResetRiderMotion();
	AController* BikeController = GetController();

	/**
	 * 下车时快递**留在后备箱里**，不再自动回到手上
	 *
	 * 要取出来得走到车尾按左键（和装进去对称）。所以这里连 ReleaseCargo 都不调——
	 * 调了就等于"下车即卸货"，车上那条记录也就没了。
	 */

	Leaving->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	Leaving->SetActorEnableCollision(true);
	Leaving->SetActorHiddenInGame(false);
	Leaving->SetActorLocationAndRotation(ExitLocation, FRotator(0.0f, GetActorRotation().Yaw, 0.0f), false,
	                                     nullptr, ETeleportType::TeleportPhysics);

#if !UE_BUILD_SHIPPING
	UE_LOG(LogDelivery, Log, TEXT("InputDiag %.3f ExitVehicle 开始占有 leaving=%s controller=%s"),
	       GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f, *GetNameSafe(Leaving),
	       *GetNameSafe(BikeController));
#endif
	if (BikeController)
	{
		BikeController->Possess(Leaving);
		if (APlayerController* PC = Cast<APlayerController>(BikeController))
		{
			PC->SetViewTargetWithBlend(Leaving, 0.35f);
		}
	}

	// StartRagdoll 里带 PlaceOnGround，所以要在挪好位置之后再开
	if (UDeliveryActiveRagdollComponent* Ragdoll = Leaving->GetActiveRagdoll())
	{
		Ragdoll->StartRagdoll();
	}

	ThrottleInput = 0.0f;
	SteerInput = 0.0f;
	MoveInputRaw = FVector2D::ZeroVector;
	TurnAroundSign = 0.0f;
	bBrakingForTurn = false;
	TrafficImpactCount = 0;
	LastImpactTime = -1000.0f;
	ApplyDriverPresentation();
	MulticastVehicleEvent(EDeliveryEvent::VehicleExited);
}

// 网络通知到达后，各端只发布自己这辆车的事件
void ADeliveryMotorbike::MulticastVehicleEvent_Implementation(EDeliveryEvent Event)
{
    Events->Publish(Event);
}

// 在车辆附近寻找不会被障碍挡住的下车位置
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

	// 横向至少要让开"车身半宽 + 角色胶囊半径 + 余量"，否则人落下来还压在车上
	// 直接用碰撞盒的实际尺寸，车模型换了也不用回来改这个数
	const FVector BoxExtent =
	    CollisionBox ? CollisionBox->GetScaledBoxExtent() : FVector(110.0f, 45.0f, 45.0f);
	const float SideDistance = FMath::Max(FMath::Abs(ExitSideOffset), BoxExtent.Y + 42.0f + 60.0f);

	// 首选侧由调用方指定（被撞下车时是"车被推离的那一侧"），没指定就用配置的默认侧
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
			// 角色胶囊半高 96，落点抬到地面上方一个胶囊高度
			return GroundHit.ImpactPoint + FVector(0.0f, 0.0f, 100.0f);
		}
		return Candidate;
	}

	// 周围无安全落点时放到车顶上方，避免将角色放进车身内部
	
	return Center + FVector(0.0f, 0.0f, BoxExtent.Z + 140.0f);
}

// 驾驶输入

// 绑定驾驶、视角、下车和骑车取件操作
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
	// 始终绑定视角输入，由回调读取当前模式，支持运行时切换
	
	
	if (LookAction)
	{
		EnhancedInput->BindAction(LookAction, ETriggerEvent::Triggered, this, &ADeliveryMotorbike::LookInput);
	}
	if (MouseLookAction)
	{
		EnhancedInput->BindAction(MouseLookAction, ETriggerEvent::Triggered, this,
		                          &ADeliveryMotorbike::MouseLookInput);
	}

    if (UInputAction* Action = GetCameraToggleAction())
    {
        EnhancedInput->BindAction(Action, ETriggerEvent::Started, this, &ADeliveryMotorbike::ToggleCameraMode);
    }
	if (UInputAction* Interact = InteractAction.LoadSynchronous())
	{
		EnhancedInput->BindAction(Interact, ETriggerEvent::Started, this,
		                          &ADeliveryMotorbike::InteractPressed);
	}
	if (UInputAction* Pickup = PickupAction.LoadSynchronous())
	{
		// 松开和取消输入都要结束长按，防止服务器保留上次操作
		
		EnhancedInput->BindAction(Pickup, ETriggerEvent::Started, this, &ADeliveryMotorbike::PickupPressed);
		EnhancedInput->BindAction(Pickup, ETriggerEvent::Completed, this,
		                          &ADeliveryMotorbike::PickupReleased);
		EnhancedInput->BindAction(Pickup, ETriggerEvent::Canceled, this, &ADeliveryMotorbike::PickupReleased);
	}
}

// 读取驾驶输入，按当前控制方式转换成油门和转向
void ADeliveryMotorbike::MoveInput(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	MoveInputRaw.X = FMath::Clamp(Axis.X, -1.0f, 1.0f);
	MoveInputRaw.Y = FMath::Clamp(Axis.Y, -1.0f, 1.0f);

	// 车头模式直接发送输入，镜头模式每帧换算后再发送
	
	if (!UsesCameraRelativeMove())
	{
		SteerInput = MoveInputRaw.X;
		ThrottleInput = MoveInputRaw.Y;
		if (!HasAuthority())
		{
			ServerSetDriveInput(ThrottleInput, SteerInput);
			LastSentThrottle = ThrottleInput;
			LastSentSteer = SteerInput;
		}
	}
}

// 判断是否使用镜头方向作为驾驶输入参考
bool ADeliveryMotorbike::UsesCameraRelativeMove() const
{
	// 固定视角已跟随车头，无需再做镜头方向换算
	
	return bCameraRelativeMoveInFreeLook && bFreeLookCamera;
}

// 把相对镜头的移动意图转换为车辆转向和前后行驶
void ADeliveryMotorbike::ApplyCameraRelativeInput()
{
	if (!UsesCameraRelativeMove())
	{
		return;
	}

	// 松手就彻底归零，别留残值——否则松手后车还会按最后一次的方向继续修正
	if (MoveInputRaw.IsNearlyZero())
	{
		ThrottleInput = 0.0f;
		SteerInput = 0.0f;
		TurnAroundSign = 0.0f;
		bBrakingForTurn = false;
	}
	// 后退输入直接用于刹车和倒车，不将后退解释为掉头
	// 倒车转向仍按车身方向计算

else if (MoveInputRaw.Y < -UE_KINDA_SMALL_NUMBER)
	{
		ThrottleInput = FMath::Clamp(MoveInputRaw.Y, -1.0f, 1.0f);
		SteerInput = FMath::Clamp(MoveInputRaw.X, -1.0f, 1.0f);
		// 后退不是掉头动作，两个锁定都不该留着（否则松开 S 按 W 会沿用上次的侧）
		TurnAroundSign = 0.0f;
		bBrakingForTurn = false;
	}
	else
	{
		// 使用玩家控制的镜头方向换算输入，避免镜头位置滞后影响驾驶响应

float CameraYaw = GetActorRotation().Yaw;
		if (const APlayerController* PC = Cast<APlayerController>(GetController()))
		{
			CameraYaw = PC->GetControlRotation().Yaw;
		}

		// 输入纵轴表示前后，横轴表示左右，据此计算相对镜头的目标方向
		
		
		const float InputYaw = FMath::RadiansToDegrees(FMath::Atan2(MoveInputRaw.X, MoveInputRaw.Y));
		const float DesiredYaw = CameraYaw + InputYaw;

		// 车头到目标方向的带符号夹角（-180~180）；正值表示目标在车头右侧
		const float Error = FRotator::NormalizeAxis(DesiredYaw - GetActorRotation().Yaw);
		const float AbsError = FMath::Abs(Error);

		// 输入强度（摇杆可以推一半）；WASD 是 0/1，这里对两者都成立
		const float Magnitude = FMath::Min(MoveInputRaw.Size(), 1.0f);

		// 大角度转向先锁定一侧，转到侧面后再对准目标
		// 同时锁定是否先刹车，避免方向正负号和速度临界值引起反复切换

float StagedError = Error;
		if (AbsError > CameraRelativeStageSideAngle)
		{
			if (FMath::IsNearlyZero(TurnAroundSign))
			{
				// 仅在进入掉头时确定转向侧，后续保持不变
				
				
				const float ErrorSign = FMath::Sign(Error);
				TurnAroundSign = FMath::IsNearlyZero(ErrorSign) ? 1.0f : ErrorSign;

				// 仅在动作开始时决定是否先刹车，不每帧重新选择
				
				bBrakingForTurn =
				    AbsError > CameraRelativeBrakeAngle && CurrentSpeed > TurnSpeedReference * 0.25f;
			}
			StagedError = TurnAroundSign * CameraRelativeStageSideAngle;
		}
		else
		{
			// 已经转到侧面以内，动作结束，解锁交给常规换算
			TurnAroundSign = 0.0f;
			bBrakingForTurn = false;
		}

		// 降到较低的退出速度后才结束刹车，避免在同一速度附近反复切换
		
		if (bBrakingForTurn && CurrentSpeed > CameraRelativeBrakeExitSpeed)
		{
			// 反向油门用于快速减速
			
			ThrottleInput = -Magnitude;
			// 刹车时不强行扭车头：这时候横向摆车看着像甩尾，不是刹车
			SteerInput = 0.0f;
		}
		else
		{
			// 刹够了就接着掉头，同一个键走完整个动作
			bBrakingForTurn = false;

			SteerInput =
			    FMath::Clamp(StagedError / FMath::Max(CameraRelativeFullSteerAngle, 1.0f), -1.0f, 1.0f);
			// 大角度转向仍保留最低油门，确保车辆移动时能够继续转向
			
			// 按当前分段目标计算油门，不直接使用最终方向误差
			
			const float Align = FMath::Cos(FMath::DegreesToRadians(FMath::Abs(StagedError)));
			const float Scale = FMath::Max(Align, CameraRelativeMinThrottle);
			ThrottleInput = Magnitude * Scale;
		}
	}

	// 换算出来的值每帧都可能变，但只在变化够大时才发，省掉同值重复 RPC
	if (!HasAuthority())
	{
		if (FMath::Abs(ThrottleInput - LastSentThrottle) > 0.02f ||
		    FMath::Abs(SteerInput - LastSentSteer) > 0.02f)
		{
			ServerSetDriveInput(ThrottleInput, SteerInput);
			LastSentThrottle = ThrottleInput;
			LastSentSteer = SteerInput;
		}
	}
}

// 更新手柄视角输入
void ADeliveryMotorbike::LookInput(const FInputActionValue& Value)
{
	if (!bFreeLookCamera)
	{
		return;
	}
	const FVector2D Axis = Value.Get<FVector2D>();
	// 俯仰的上下限走 PlayerCameraManager 的 ViewPitchMin/Max，和角色那边同一套限制
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(Axis.Y);
}

// 按鼠标灵敏度更新视角
void ADeliveryMotorbike::MouseLookInput(const FInputActionValue& Value)
{
	if (!bFreeLookCamera)
	{
		return;
	}
	const FVector2D Axis = Value.Get<FVector2D>() * MouseLookSensitivity;
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(Axis.Y);
}

// 驾驶视角切换

// 切换驾驶视角，并保存对应镜头设置
void ADeliveryMotorbike::ToggleCameraMode()
{
	bFreeLookCamera = !bFreeLookCamera;
	ApplyCameraMode();

	// 切换到车头模式时立即用原始输入重算，避免沿用镜头模式的转向结果
	
	
	if (!UsesCameraRelativeMove())
	{
		// 掉头锁定是镜头相对模式独有的状态，切走就清掉，免得切回来沿用旧侧
		TurnAroundSign = 0.0f;
		bBrakingForTurn = false;
		SteerInput = MoveInputRaw.X;
		ThrottleInput = MoveInputRaw.Y;
		if (!HasAuthority())
		{
			ServerSetDriveInput(ThrottleInput, SteerInput);
			LastSentThrottle = ThrottleInput;
			LastSentSteer = SteerInput;
		}
	}
	Events->Publish(EDeliveryEvent::CameraChanged);

	// 切换时衔接当前镜头方向，避免视角跳变
	
	
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (!bFreeLookCamera)
		{
			SyncControlRotation();
		}
		else
		{
			PC->SetControlRotation(
			    FRotator(CameraPitch, GetActorRotation().Yaw, PC->GetControlRotation().Roll));
		}
	}

	ShowNotice(bFreeLookCamera ? NSLOCTEXT("Delivery", "MotorbikeFreeLook", "自由视角（P 切换）")
	                           : NSLOCTEXT("Delivery", "MotorbikeFixedLook", "固定视角（P 切换）"),
	           1.5f);
}

// 将当前驾驶视角配置应用到镜头
void ADeliveryMotorbike::ApplyCameraMode()
{
	if (!CameraBoom)
	{
		return;
	}

	if (bFreeLookCamera)
	{
		// 自由视角继承玩家控制的水平角和俯仰角，确保两个方向都能操作
		
		CameraBoom->bUsePawnControlRotation = true;
		CameraBoom->bInheritPitch = true;
		CameraBoom->bInheritYaw = true;
		// Roll 仍然不继承——留着这一轴给被撞时的镜头抖动用（见 UpdateImpactReaction）
		CameraBoom->bInheritRoll = false;
		CameraBoom->SetRelativeRotation(FRotator::ZeroRotator);
	}
	else
	{
		// 固定车尾视角：原样还原最早那套
		CameraBoom->bUsePawnControlRotation = false;
		CameraBoom->bInheritPitch = false;
		CameraBoom->bInheritYaw = true;
		CameraBoom->bInheritRoll = false;
		CameraBoom->SetRelativeRotation(FRotator(CameraPitch, 0.0f, 0.0f));
	}
}

// 服务器驾驶请求

void ADeliveryMotorbike::ServerSetDriveInput_Implementation(float InThrottle, float InSteer)
{
	ThrottleInput = FMath::IsFinite(InThrottle) ? FMath::Clamp(InThrottle, -1.0f, 1.0f) : 0.0f;
	SteerInput = FMath::IsFinite(InSteer) ? FMath::Clamp(InSteer, -1.0f, 1.0f) : 0.0f;
}

// 骑车按下取件键时转交驾驶员的交互组件
void ADeliveryMotorbike::PickupPressed(const FInputActionValue& /* 未使用 */)
{
	// 将取件操作转发给驾驶员，复用角色的探测和长按逻辑
	
	
	if (IsValid(Driver))
	{
		Driver->DoPickup();
	}
}

// 骑车松开取件键时取消驾驶员的长按操作
void ADeliveryMotorbike::PickupReleased(const FInputActionValue& /* 未使用 */)
{
	if (IsValid(Driver))
	{
		Driver->DoPickupRelease();
	}
}

// 将驾驶中的交互按键作为下车请求
void ADeliveryMotorbike::InteractPressed(const FInputActionValue& /* 未使用 */)
{
	if (HasAuthority())
	{
		ExitVehicle();
		return;
	}
	ServerRequestExit();
}

// 在服务器处理驾驶员下车请求
void ADeliveryMotorbike::ServerRequestExit_Implementation()
{
	// 驾驶员所属客户端发送下车请求，服务器执行下车流程
	
	ExitVehicle();
}

// 车辆每帧更新

// 更新驾驶状态、车辆运动和车轮骑手表现
void ADeliveryMotorbike::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (HasAuthority() && Driver && !IsValid(Driver))
	{
		Driver = nullptr;
		ApplyDriverPresentation();
	}

	// 服务器持续计算车辆移动，本机驾驶员同时预测以获得即时响应
	
	const bool bSimulate = HasAuthority() || (Driver && IsLocallyControlled());
	if (bSimulate)
	{
		if (Driver)
		{
			// 先换算镜头方向输入，再更新速度和转向
			// 服务器使用客户端发送的换算结果
			
			
			if (IsLocallyControlled())
			{
				ApplyCameraRelativeInput();
			}
			UpdateSpeed(DeltaSeconds);
			UpdateSteering(DeltaSeconds);
		}
		else
		{
			CurrentSpeed = 0.0f;
		}
		UpdateGroundAndMove(DeltaSeconds);
		// 排在移动之后：要扫的是"这一帧车走过的那段路径"，
		// 放在移动之前扫的是上一帧的位置，高速时会整段漏掉
		if (HasAuthority())
		{
			ProcessPedestrianImpacts(DeltaSeconds);
		}
	}

	UpdateTrunkAnimation(DeltaSeconds);
	UpdateImpactReaction(DeltaSeconds);
	UpdateRiderMotion(DeltaSeconds);
	UpdateLean(DeltaSeconds);
	UpdateSteerVisual(DeltaSeconds);
	UpdateWheelSpin(DeltaSeconds);
    // 远端车辆使用复制的速度，本机和服务器使用当前速度
    const float AudioSpeed = !HasAuthority() && !IsLocallyControlled()
        ? ReplicatedRiderMotion.Speed * MaxSpeed : CurrentSpeed;
    VehicleAudio->UpdateEngine(Driver != nullptr, AudioSpeed / FMath::Max(MaxSpeed, 1.0f));

	if (Driver && IsLocallyControlled())
	{
		// 自由视角下控制旋转就是镜头，每帧同步成车头朝向等于把玩家的鼠标抹掉
		if (!bFreeLookCamera)
		{
			SyncControlRotation();
		}

		// 下车提示只在上车后短暂显示，避免长期遮挡画面
		
		if (!bHadDriverLastFrame)
		{
			ShowNotice(ExitPromptText, ExitPromptDuration);
		}
		if (NoticeRemaining > 0.0f)
		{
			NoticeRemaining -= DeltaSeconds;
			PushNotice();
		}
		bHadDriverLastFrame = true;
	}
	else
	{
		bHadDriverLastFrame = false;
		NoticeRemaining = 0.0f;
	}
}

// 加速与制动

// 根据油门、刹车和阻力更新车速
void ADeliveryMotorbike::UpdateSpeed(float DeltaSeconds)
{
	const float Throttle = FMath::Clamp(ThrottleInput, -1.0f, 1.0f);
	if (FMath::IsNearlyZero(Throttle))
	{
		CurrentSpeed = FMath::FInterpConstantTo(CurrentSpeed, 0.0f, DeltaSeconds, CoastDeceleration);
		return;
	}

	// 油门方向和当前车速反着来 = 刹车，用大得多的减速度，手感上按一下就明显慢下来
	const bool bOpposing = (Throttle * CurrentSpeed) < -UE_KINDA_SMALL_NUMBER;
	const float Rate = bOpposing ? BrakeDeceleration : ThrottleAcceleration;
	const float Target = Throttle > 0.0f ? MaxSpeed * Throttle : MaxReverseSpeed * Throttle;
	CurrentSpeed = FMath::FInterpConstantTo(CurrentSpeed, Target, DeltaSeconds, Rate);
}

// 根据转向输入平滑更新转向角
void ADeliveryMotorbike::UpdateSteering(float DeltaSeconds)
{
	if (FMath::IsNearlyZero(SteerInput))
	{
		return;
	}

	// 摩托车靠前进才能转向，停着打把是原地转圈，很出戏
	const float SpeedFactor = FMath::Clamp(FMath::Abs(CurrentSpeed) / TurnSpeedReference, 0.0f, 1.0f);
	const float EffectiveSpeedFactor = bBlockedByWall
	    ? FMath::Max(SpeedFactor, BlockedTurnSpeedFactor) : SpeedFactor;
	const float Direction = CurrentSpeed >= 0.0f ? 1.0f : -1.0f;
	const float YawDelta = SteerInput * MaxTurnRate * EffectiveSpeedFactor * Direction * DeltaSeconds;
	AddActorWorldRotation(FRotator(0.0f, YawDelta, 0.0f));
}

// 地面检测与车辆移动

// 检测地面并执行带碰撞的车辆移动
bool ADeliveryMotorbike::TrySlideAroundObstacle(const FVector& RemainingDelta,
                                               const FHitResult& BlockingHit)
{
	const float RemainingLength = RemainingDelta.Size2D();
	if (RemainingLength <= 1.0f)
	{
		return false;
	}
	const float MinProgress = FMath::Clamp(ObstacleSlideMinProgress, 0.01f, 1.0f);

	// ① 沿表面切向滑：擦到路缘、斜墙、墙角时这一步就够了
	FVector SlideDelta = FVector::VectorPlaneProject(RemainingDelta, BlockingHit.ImpactNormal);
	SlideDelta.Z = 0.0f;
	if (SlideDelta.SizeSquared2D() > FMath::Square(1.0f))
	{
		const FVector SlideStart = GetActorLocation();
		AddActorWorldOffset(SlideDelta, true);
		if (FVector::Dist2D(SlideStart, GetActorLocation()) >= RemainingLength * MinProgress)
		{
			return true;
		}
	}

	if (!bDeflectAroundNarrowObstacles)
	{
		return false;
	}

	// ② 正面顶住：分清"柱子"和"墙"
	//
	// 法线几乎正对车头时①的切向投影接近零，所以**细柱子永远滑不开**。
	// 判据不能靠障碍的体积或类型（路灯、电线杆、消防栓各是各的网格，没有共同标记
	// 可查，按类名或 tag 判迟早漏），只能问几何。
	//
	// **但不能直接试"能不能横着挪"——墙根本不挡横向移动**，贴着墙面横滑永远成功，
	// 于是撞墙也会被当成柱子绕开（第一版就是这么错的，表现是撞墙车会沿墙飘走）。
	// 要问的是：**偏开一个车宽之后，前面还通不通**。柱子通，墙不通。
	UWorld* World = GetWorld();
	if (!World || !CollisionBox)
	{
		return false;
	}
	const FVector Forward = GetActorForwardVector().GetSafeNormal2D();
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward).GetSafeNormal();
	const FVector Extent = CollisionBox->GetScaledBoxExtent();
	const float HalfWidth = FMath::Max(Extent.Y, 10.0f);
	// 一帧最多侧移"这一帧本来要走的距离"，所以绕行不会比正常行驶更快，也不会瞬移
	const float SideStep = FMath::Min(RemainingLength, HalfWidth);

	// 优先往玩家正在打把的那一侧绕；没打把时按撞击点落在车身哪一侧决定——
	// 撞在左前角就往右绕，绕的是更近的那条边
	float PreferredSign = 1.0f;
	if (!FMath::IsNearlyZero(SteerInput))
	{
		PreferredSign = SteerInput > 0.0f ? 1.0f : -1.0f;
	}
	else
	{
		const float ImpactSide =
		    FVector::DotProduct(BlockingHit.ImpactPoint - GetActorLocation(), Right);
		PreferredSign = ImpactSide >= 0.0f ? -1.0f : 1.0f;
	}

	// 用车身自己的碰撞设置试探，而不是挑一个通道：挡得住车的东西才算挡路，
	// 挑通道迟早和车身的响应配置对不上（本仓库在"按通道查恒为空"上栽过三次）
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MotorbikeObstacleSide), false, this);
	Params.AddIgnoredActor(Driver);
	const FCollisionShape Body = FCollisionShape::MakeBox(Extent);
	const FQuat Rotation = GetActorQuat();

	for (int32 Attempt = 0; Attempt < 2; ++Attempt)
	{
		const float Sign = Attempt == 0 ? PreferredSign : -PreferredSign;
		const FVector Aside = GetActorLocation() + Right * Sign * HalfWidth * 2.0f;
		const FVector AheadOfAside = Aside + Forward * (RemainingLength + ObstacleSideProbeAhead);
		if (World->SweepTestByProfile(Aside, AheadOfAside, Rotation,
		                              CollisionBox->GetCollisionProfileName(), Body, Params))
		{
			// 这一侧偏开了也走不了，是墙（或者墙角），换另一侧
			continue;
		}

		const FVector Start = GetActorLocation();
		AddActorWorldOffset(Right * Sign * SideStep, true);
		if (FVector::Dist2D(Start, GetActorLocation()) >= SideStep * MinProgress)
		{
			return true;
		}
		// 侧移本身被挡住（两边都贴着东西），撤回去再试另一侧；
		// 不撤回会把两次试探叠起来
		SetActorLocation(Start, false, nullptr, ETeleportType::TeleportPhysics);
	}

	return false;
}

void ADeliveryMotorbike::UpdateGroundAndMove(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// 上一帧有没有顶着东西：决定这一帧要不要再扣一次撞墙速度惩罚。
	// 不留这个状态就只能每帧都扣，而每帧乘 0.5 在 60 fps 下十来帧就等于归零
	const bool bWasBlockedByWall = bBlockedByWall;
	bBlockedByWall = false;

	// 1) 水平推进；带 sweep，撞墙就掉速而不是穿过去；撞的是矮台阶就先试着跨上去
	if (!FMath::IsNearlyZero(CurrentSpeed))
	{
		const FVector Delta = GetActorForwardVector() * CurrentSpeed * DeltaSeconds;
		FHitResult MoveHit;
		AddActorWorldOffset(Delta, true, &MoveHit);
		if (MoveHit.bBlockingHit)
		{
			// 已经卡进障碍里就先推出来。不推的话后面滑动和绕行的 sweep 起点就在
			// 障碍内部，怎么挪都是 bStartPenetrating，车会一直嵌在花坛里出不来
			if (MoveHit.bStartPenetrating)
			{
				const float Push =
				    FMath::Clamp(MoveHit.PenetrationDepth + 2.0f, 2.0f, ObstaclePushOutMax);
				AddActorWorldOffset(MoveHit.Normal * Push, false);
			}

			// 这一帧还没走完的那一段，跨上去之后要接着走完
			const FVector Remaining = Delta * (1.0f - MoveHit.Time);
			if (!TryStepUp(Remaining, MoveHit))
			{
				bBlockedByWall = !TrySlideAroundObstacle(Remaining, MoveHit);
				if (bBlockedByWall)
				{
					// **只在刚顶上那一帧扣速度**，之后维持余速不再衰减。
					// 原来每帧都乘，顶一下就把车速压成零、玩家还得从 0 重新加速，
					// 那正是"撞到路灯要卡很久"的来源；顶着时 sweep 照样挡住车，
					// 留余速不会穿进去，但绕开的瞬间就能走
					if (!bWasBlockedByWall)
					{
						CurrentSpeed *= WallHitSpeedScale;
					}
					CurrentSpeed =
					    FMath::Clamp(CurrentSpeed, -BlockedScrubSpeed, BlockedScrubSpeed);
				}
			}
		}
	}

	// 2) 竖直贴地；这一步不能用 sweep：sweep 会被地面挡住，和"我要贴到地面上"互相打架
	FVector Location = GetActorLocation();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MotorbikeGround), false, this);
	Params.AddIgnoredActor(Driver);

	FHitResult GroundHit;
	const bool bHitGround = World->LineTraceSingleByChannel(
	    GroundHit, Location + FVector(0.0f, 0.0f, GroundTraceUp),
	    Location - FVector(0.0f, 0.0f, GroundTraceDown), ECC_Visibility, Params);

	// 再往前看一小段；上坡/上台阶时提前抬车，不然等车头撞上坡面才反应就已经掉速了
	// 前瞻距离跟着车速走，停着时为 0——否则停在坡底也会莫名浮起来
	float DesiredZ = bHitGround ? GroundHit.ImpactPoint.Z + HoverHeight : 0.0f;
	if (bHitGround && GroundLookAhead > 0.0f)
	{
		const float AheadDist = FMath::Min(FMath::Abs(CurrentSpeed) * DeltaSeconds * 2.0f, GroundLookAhead);
		if (AheadDist > 1.0f)
		{
			const FVector Ahead = Location + GetActorForwardVector() * AheadDist;
			FHitResult AheadHit;
			if (World->LineTraceSingleByChannel(AheadHit, Ahead + FVector(0.0f, 0.0f, GroundTraceUp),
			                                    Ahead - FVector(0.0f, 0.0f, GroundTraceDown), ECC_Visibility,
			                                    Params))
			{
				// 只提前跟随更高的地面，前方变低时仍由当前地面和重力决定下落
				
				DesiredZ = FMath::Max(DesiredZ, AheadHit.ImpactPoint.Z + HoverHeight);
			}
		}
	}

	if (bHitGround)
	{
		// 离地阈值应高于可跨台阶高度，避免刚跨上台阶就进入下落状态
		
		if (Location.Z <= DesiredZ + FMath::Max(GroundStickTolerance, MaxStepHeight + 10.0f))
		{
			const float SnapSpeed = DesiredZ < Location.Z ? GroundSnapDownSpeed : GroundSnapSpeed;
			float NewZ = FMath::FInterpTo(Location.Z, DesiredZ, DeltaSeconds, SnapSpeed);
			if (DesiredZ > Location.Z)
			{
				// 上坡时保证最低抬升速度，避免车身偏低导致下一帧撞到坡面
				
				const float MinClimbZ = FMath::Min(DesiredZ, Location.Z + MaxClimbRate * DeltaSeconds);
				NewZ = FMath::Max(NewZ, MinClimbZ);
			}
			Location.Z = NewZ;
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

	// 3) 车身跟着坡面抬头/低头；太陡的面（马路牙子、墙根）不跟，跟了会把车竖起来
	if (bHitGround && bWasGrounded)
	{
		const FVector Normal = GroundHit.ImpactNormal;
		const float SlopeAngle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Normal.Z, -1.0f, 1.0f)));
		if (SlopeAngle <= MaxGroundAlignAngle)
		{
			const FVector PlanarForward =
			    FVector::VectorPlaneProject(GetActorForwardVector(), Normal).GetSafeNormal();
			if (!PlanarForward.IsNearlyZero())
			{
				const FRotator Current = GetActorRotation();
				FRotator Aligned =
				    FMath::RInterpTo(Current, FRotationMatrix::MakeFromXZ(PlanarForward, Normal).Rotator(),
					                 DeltaSeconds, GroundAlignSpeed);
				// 朝向只由转向控制，别让地面对齐把车头带跑
				Aligned.Yaw = Current.Yaw;
				SetActorRotation(Aligned);
			}
		}
	}
}

// 尝试越过低矮障碍，并检查上方与前方空间
bool ADeliveryMotorbike::TryStepUp(const FVector& RemainingDelta, const FHitResult& BlockingHit)
{
	if (MaxStepHeight <= 0.0f || RemainingDelta.IsNearlyZero())
	{
		return false;
	}

	// 顶部碰撞时不尝试抬高跨越
	if (BlockingHit.ImpactNormal.Z < -0.1f)
	{
		return false;
	}

	const FVector StartLocation = GetActorLocation();
	const FVector StepUp(0.0f, 0.0f, MaxStepHeight);

	// ① 抬；头顶有东西（低矮的桥洞、车库门）就整体放弃
	FHitResult UpHit;
	AddActorWorldOffset(StepUp, true, &UpHit);
	if (UpHit.bBlockingHit)
	{
		SetActorLocation(StartLocation, false, nullptr, ETeleportType::TeleportPhysics);
		return false;
	}

	// 抬高后继续前移，未达到最小前进比例时撤回，按撞墙处理
	
	
	FHitResult ForwardHit;
	AddActorWorldOffset(RemainingDelta, true, &ForwardHit);
	if (ForwardHit.bBlockingHit && ForwardHit.Time < StepUpMinProgress)
	{
		SetActorLocation(StartLocation, false, nullptr, ETeleportType::TeleportPhysics);
		return false;
	}

	// ③ 落回地面；没落到东西也算成功——那是"从台阶上飞出去"，交给贴地/重力那一步
	FHitResult DownHit;
	AddActorWorldOffset(-StepUp, true, &DownHit);
	if (DownHit.bBlockingHit)
	{
		const float StepSlope =
		    FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(DownHit.ImpactNormal.Z, -1.0f, 1.0f)));
		if (StepSlope > MaxClimbAngle)
		{
			// 落点坡度超过可行驶范围时撤回，避免沿墙角不断向上爬
			
			SetActorLocation(StartLocation, false, nullptr, ETeleportType::TeleportPhysics);
			return false;
		}
	}

	return true;
}

// 车身与车轮表现

// 根据转向和速度更新车身侧倾
void ADeliveryMotorbike::UpdateLean(float DeltaSeconds)
{
	if (!MeshRoot)
	{
		return;
	}

	const bool bRemote = !HasAuthority() && !IsLocallyControlled();
	const float VisualSpeed = bRemote ? ReplicatedRiderMotion.Speed * MaxSpeed : CurrentSpeed;
	const float VisualSteer = bRemote ? ReplicatedRiderMotion.Steer : SteerInput;
	const float SpeedFactor = FMath::Clamp(FMath::Abs(VisualSpeed) / TurnSpeedReference, 0.0f, 1.0f);
	// 远端也用复制的驾驶输入表现车身侧倾；不把这些值写回实际驾驶状态
	const float TargetLean = -VisualSteer * MaxLeanAngle * SpeedFactor;
	CurrentLean = FMath::FInterpTo(CurrentLean, TargetLean, DeltaSeconds, LeanSpeed);
	// 撞击侧倾直接叠加到转弯侧倾，不再平滑削弱撞击瞬间的反馈
	
	MeshRoot->SetRelativeRotation(FRotator(0.0f, 0.0f, CurrentLean + KnockTilt));
}

// 让车把与前轮跟随转向角
void ADeliveryMotorbike::UpdateSteerVisual(float DeltaSeconds)
{
	if (!SteerPivot)
	{
		return;
	}

	// 车把转动不受车速缩放，停车时也能显示转向输入
	
	const float VisualSteer =
	    !HasAuthority() && !IsLocallyControlled() ? ReplicatedRiderMotion.Steer : SteerInput;
	const float Target = FMath::Clamp(VisualSteer, -1.0f, 1.0f) * MaxVisualSteerAngle;
	CurrentVisualSteer = FMath::FInterpTo(CurrentVisualSteer, Target, DeltaSeconds, SteerVisualSpeed);

	// 局部 Yaw：+X 转向 +Y 就是往右打，和 SteerInput>0 = 按 D 对得上
	SteerPivot->SetRelativeRotation(FRotator(0.0f, CurrentVisualSteer, 0.0f));
	if (BarPivot)
	{
		BarPivot->SetRelativeRotation(FRotator(0.0f, CurrentVisualSteer * HandlebarSteerRatio, 0.0f));
	}
	if (RiderPivot)
	{
		// 转身由胸腰骨骼完成，整具骑手不再绕座位转，脚踏目标因而保持稳定
		RiderPivot->SetRelativeRotation(FRotator::ZeroRotator);
	}
}

// 根据实际行驶距离更新车轮旋转
void ADeliveryMotorbike::UpdateWheelSpin(float DeltaSeconds)
{
	if (!bSpinWheels || WheelPivots.Num() == 0 || DeltaSeconds <= 0.0f)
	{
		return;
	}

	// 远端客户端从实际位移估算车速，用于驱动轮子旋转
	
	const FVector Location = GetActorLocation();
	float Speed = CurrentSpeed;
	if (!HasAuthority() && !IsLocallyControlled())
	{
		Speed =
		    bWheelLocationValid
		        ? FVector::DotProduct(Location - LastWheelLocation, GetActorForwardVector()) / DeltaSeconds
		        : 0.0f;
	}
	LastWheelLocation = Location;
	bWheelLocationValid = true;

	/**
	 * 纯滚动：接地点速度为 0，所以角速度 = 线速度 / 半径
	 *
	 * 半径要乘 VisualScale——`WheelRadius` 是导入空间量出来的数，而车轮在世界里
	 * 已经被缩放层缩过了。不乘的话轮子会按原尺寸算，转得比实际快一截，看着像在打滑。
	 */
	const float Radius = FMath::Max(WheelRadius * FMath::Max(VisualScale, 0.01f), 1.0f);
	WheelSpinAngle += FMath::RadiansToDegrees(Speed / Radius) * DeltaSeconds * WheelSpinSign;

	for (USceneComponent* Pivot : WheelPivots)
	{
		if (Pivot)
		{
			// 轮轴只绕局部横轴旋转，其他姿态由父组件控制
			
			Pivot->SetRelativeRotation(FRotator(0.0f, 0.0f, WheelSpinAngle));
		}
	}
}

// 将驾驶视角同步到控制器
void ADeliveryMotorbike::SyncControlRotation()
{
	// 固定镜头模式同步车头方向到控制旋转，使下车后镜头连续
	
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		PC->SetControlRotation(FRotator(CameraPitch, GetActorRotation().Yaw, 0.0f));
	}
}

// 驾驶提示

void ADeliveryMotorbike::ShowNotice(const FText& Text, float Seconds)
{
	NoticeText = Text;
	NoticeRemaining = Seconds;
}

// 显示短时车辆操作提示
void ADeliveryMotorbike::PushNotice()
{
	if (UDeliveryPromptSubsystem* Prompt = UDeliveryPromptSubsystem::Get(this))
	{
		const FVector Anchor =
		    GetActorLocation() + (Interactable ? Interactable->PromptOffset : FVector(0.0f, 0.0f, 180.0f));
		Prompt->PushPrompt(NoticeText, Anchor);
	}
}

UInputAction* ADeliveryMotorbike::GetCameraToggleAction() const
{
    return CameraToggleAction.LoadSynchronous();
}
