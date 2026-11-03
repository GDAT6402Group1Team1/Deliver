// Copyright Epic Games, Inc. All Rights Reserved.

#include "Task/Tracking/DeliveryObjectiveRingComponent.h"

#include "World/DeliveryLocationRegistry.h"
#include "Task/Tracking/DeliveryTaskTrackerComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/DecalComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Materials/MaterialInstanceDynamic.h"

#if !UE_BUILD_SHIPPING
#include "DrawDebugHelpers.h"
#endif

namespace
{
/** 没有材质时的退路，只在非 Shipping 有效 */
constexpr int32 FallbackRingSegments = 48;
constexpr float FallbackRingLift = 4.0f;
/** 备用调试线的存活时间必须大于组件更新间隔，避免两次重画之间出现闪烁 */
constexpr float FallbackRingLifetime = 0.2f;

constexpr float RingTickInterval = 0.1f;
}

UDeliveryObjectiveRingComponent::UDeliveryObjectiveRingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// 圈不需要每帧更新：目的地是任务状态驱动的，玩家走动带来的只有颜色过渡，
	// 而那条过渡本来就是按 DeltaTime 插值的，10Hz 看不出来
	PrimaryComponentTick.TickInterval = RingTickInterval;
	SetIsReplicatedByDefault(false);
}

// 角色或控制器结束运行时移除任务地面圈
void UDeliveryObjectiveRingComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Decal)
	{
		Decal->DestroyComponent();
		Decal = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

// 找到或创建地面圈使用的显示组件
bool UDeliveryObjectiveRingComponent::ResolveRing(const APawn* Pawn, FVector& OutCenter, float& OutRadius,
                                                  bool& bOutInRange) const
{
	// 不能是 const：FindTracker 收的是非 const AActor*
	APlayerController* Controller = Cast<APlayerController>(GetOwner());
	if (!Pawn || !Controller || !Controller->IsLocalController())
	{
		return false;
	}

	// 目的地统一从任务追踪器取得，避免不同指引使用不同任务阶段
	
	const UDeliveryTaskTrackerComponent* Tracker = UDeliveryTaskTrackerComponent::FindTracker(Controller);
	FVector Destination = FVector::ZeroVector;
	FName LocationId = NAME_None;
	if (!Tracker || !Tracker->GetTrackedTaskDestination(Destination, LocationId))
	{
		return false;
	}

	// 圈要画多大，得问那个点自己的交互半径——不是写死一个数
	// 地点 ID → Actor → 可交互组件，这条链和玩家按 E 时走的是同一套数据
	const UDeliveryLocationRegistry* Registry = UDeliveryLocationRegistry::Get(this);
	const AActor* DestinationActor = Registry ? Registry->ResolveActor(LocationId) : nullptr;
	const UDeliveryInteractableComponent* Interactable =
	    UDeliveryInteractableComponent::FindOn(DestinationActor);
	if (!Interactable)
	{
		// 地点标记在、但上面没挂可交互组件：玩家走过去按 E 是没反应的
		// 这种情况不画圈——画了等于承诺"走到这儿能按"
		// `Delivery.Task.Validate` 会把这类配置问题列出来
		return false;
	}

	const FVector PawnLocation = Pawn->GetActorLocation();

	// 圆心走和距离判定同一个函数，不另算一遍
	OutCenter = Interactable->GetInteractionPoint(PawnLocation);

	/**
	 * 圆圈半径需计入玩家胶囊的半径
	 * 交互按双方碰撞表面的距离判断，加入补偿后圆圈才与实际可操作范围一致
	 */
	float ViewerSurfaceOffset = 0.0f;
	if (Interactable->InteractionKey == EDeliveryInteractionKey::PickupE)
	{
		if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(Pawn->GetRootComponent()))
		{
			ViewerSurfaceOffset = Capsule->GetScaledCapsuleRadius();
		}
	}
	OutRadius = Interactable->InteractRadius + ViewerSurfaceOffset;

	// 进范围的判据也走现成的那句，和提示浮窗、服务器复核用的是同一个数
	const float Distance = Interactable->GetInteractionDistance(Pawn);
	bOutInRange = Distance >= 0.0f && Distance <= Interactable->InteractRadius;

	return true;
}

// 隐藏当前地面圈
void UDeliveryObjectiveRingComponent::HideRing()
{
	if (Decal)
	{
		Decal->SetVisibility(false);
	}
}

// 取得当前目的地并更新地面圈的位置、方向和可见性
void UDeliveryObjectiveRingComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                                    FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;

	FVector Center = FVector::ZeroVector;
	float Radius = 0.0f;
	bool bInRange = false;
	if (!bShowObjectiveRing || !ResolveRing(Pawn, Center, Radius, bInRange))
	{
		HideRing();
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 远近配色做插值而不是直接切：玩家站在边界上来回挪半步时，直接切会闪
	NearBlend = FMath::FInterpTo(NearBlend, bInRange ? 1.0f : 0.0f, DeltaTime, ColorBlendSpeed);
	FLinearColor Color = FMath::Lerp(FarColor, NearColor, NearBlend);

	float Opacity = 1.0f;
	if (PulsePeriod > KINDA_SMALL_NUMBER)
	{
		const float Phase = 2.0f * PI * World->GetTimeSeconds() / PulsePeriod;
		Opacity = 1.0f - PulseAmount * 0.5f * (1.0f - FMath::Cos(Phase));
	}

	UMaterialInterface* Material = RingMaterial.LoadSynchronous();
	if (Material)
	{
		if (!Decal)
		{
			Decal = NewObject<UDecalComponent>(GetOwner());
			Decal->SetupAttachment(nullptr);
			Decal->RegisterComponent();
			// 目标圆圈只在本机创建，不需要额外过滤其他玩家
			
			Decal->SetRelativeRotation(FRotator(-90.0f, 0.0f, 0.0f));
		}
		if (!RingMID || RingMID->Parent != Material)
		{
			RingMID = UMaterialInstanceDynamic::Create(Material, this);
			Decal->SetDecalMaterial(RingMID);
		}

		RingMID->SetVectorParameterValue(TEXT("RingColor"), Color);
		RingMID->SetScalarParameterValue(TEXT("RingOpacity"), Opacity);

		// 贴花沿自身 -X 投影，所以朝下要 pitch -90；Y/Z 才是圈的半宽半高
		Decal->SetWorldLocationAndRotation(Center, FRotator(-90.0f, 0.0f, 0.0f));
		Decal->DecalSize = FVector(ProjectionDepth, Radius, Radius);
		Decal->MarkRenderStateDirty();
		Decal->SetVisibility(true);
		return;
	}

#if !UE_BUILD_SHIPPING
	// 材质还没导入时的退路：至少能看见范围，不至于以为功能没做
	// 逐点探地，圈才会跟着坡和台阶走；整圈共用圆心高度的话稍有坡度就一半埋地里
	HideRing();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DeliveryObjectiveRingFallback), false);
	Params.AddIgnoredActor(Pawn);

	const FColor LineColor = Color.ToFColor(true);
	FVector Previous = FVector::ZeroVector;
	for (int32 Step = 0; Step <= FallbackRingSegments; ++Step)
	{
		const float Angle = 2.0f * PI * Step / FallbackRingSegments;
		FVector Point(Center.X + Radius * FMath::Cos(Angle), Center.Y + Radius * FMath::Sin(Angle), Center.Z);

		FHitResult Hit;
		if (World->LineTraceSingleByObjectType(
		        Hit, Point + FVector(0.0f, 0.0f, 300.0f), Point - FVector(0.0f, 0.0f, ProjectionDepth),
		        FCollisionObjectQueryParams(FCollisionObjectQueryParams::InitType::AllStaticObjects), Params))
		{
			Point.Z = Hit.ImpactPoint.Z;
		}
		Point.Z += FallbackRingLift;

		if (Step > 0)
		{
			DrawDebugLine(World, Previous, Point, LineColor, false, FallbackRingLifetime, 0, 4.0f);
		}
		Previous = Point;
	}
#endif
}
