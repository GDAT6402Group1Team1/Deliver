#include "DeliveryGroundArrowComponent.h"

#include "DeliveryTaskTrackerComponent.h"
#include "Components/WidgetComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Rendering/DrawElements.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Widgets/Layout/SBox.h"

TSharedRef<SWidget> UDeliveryGroundArrowWidget::RebuildWidget()
{
	return SNew(SBox).WidthOverride(128.f).HeightOverride(128.f);
}

void UDeliveryGroundArrowWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetVisibility(ESlateVisibility::HitTestInvisible);
	ForceVolatile(true);
}

int32 UDeliveryGroundArrowWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const int32 BaseLayer = Super::NativePaint(Args, AllottedGeometry, MyCullingRect,
		OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
	const FVector2D Size = AllottedGeometry.GetLocalSize();
	const auto Point = [&Size](float X, float Y) { return FVector2D(X * Size.X, Y * Size.Y); };
	const TArray<FVector2D> Head = {Point(.2f, .45f), Point(.5f, .15f), Point(.8f, .45f)};
	const TArray<FVector2D> Shaft = {Point(.5f, .18f), Point(.5f, .85f)};
	// Dark border keeps the yellow arrow legible on both bright roads and grass.
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const FLinearColor Color = Pass == 0 ? FLinearColor(.025f, .025f, .015f, 1.f)
			: FLinearColor(1.f, .78f, .035f, 1.f);
		const float Thickness = Size.X * (Pass == 0 ? .16f : .10f);
		FSlateDrawElement::MakeLines(OutDrawElements, BaseLayer + 1 + Pass,
			AllottedGeometry.ToPaintGeometry(), Shaft, ESlateDrawEffect::None, Color, true, Thickness);
		FSlateDrawElement::MakeLines(OutDrawElements, BaseLayer + 1 + Pass,
			AllottedGeometry.ToPaintGeometry(), Head, ESlateDrawEffect::None, Color, true, Thickness);
	}
	return BaseLayer + 2;
}

UDeliveryGroundArrowComponent::UDeliveryGroundArrowComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	SetIsReplicatedByDefault(false);
}

void UDeliveryGroundArrowComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	APlayerController* Controller = Cast<APlayerController>(GetOwner());
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	// OnlyOwnerSee 检查的是镜头 ViewActor 与组件 Owner 的所有权链。
	// 挂在 Controller 上时，正常以 Pawn 为 ViewActor 的镜头会把箭头过滤掉。
	// 保留逻辑组件在 Controller，但把视觉挂在当前 Pawn；上下车时重建视觉。
	if (Arrow && Arrow->GetOwner() != Pawn)
	{
		Arrow->DestroyComponent();
		Arrow = nullptr;
	}
	const UDeliveryTaskTrackerComponent* Tracker = Controller && Controller->IsLocalController()
		? UDeliveryTaskTrackerComponent::FindTracker(Controller) : nullptr;
	FVector Destination;
	FName LocationId;
	if (!bShowGroundArrow || !Pawn || !Tracker || !Tracker->GetTrackedTaskDestination(Destination, LocationId))
	{
		if (Arrow) Arrow->SetVisibility(false);
		return;
	}

	const FVector Origin = Pawn->GetActorLocation();
	const FVector Direction = (Destination - Origin).GetSafeNormal2D();
	if (Direction.IsNearlyZero())
	{
		if (Arrow) Arrow->SetVisibility(false);
		return;
	}

	// Use the currently possessed pawn, so riding follows the bike rather than the hidden character.
	const float Radius = Pawn->IsA<ADeliveryMotorbike>() ? RidingRadius : WalkingRadius;
	FVector Position = Origin + Direction * FMath::Min(Radius, FVector::Dist2D(Origin, Destination));
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DeliveryGroundArrow), false, Pawn);
	TArray<AActor*> Attached;
	Pawn->GetAttachedActors(Attached, true, true);
	Params.AddIgnoredActors(Attached);
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_WorldStatic);
	Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
	FHitResult Ground;
	// Probe near the pawn's level, never from the destination's Z or the top of the world.
	if (!GetWorld()->LineTraceSingleByObjectType(Ground, Position + FVector(0, 0, 100),
		Position - FVector(0, 0, 600), Objects, Params) || Ground.ImpactNormal.Z < .35f)
	{
		if (Arrow) Arrow->SetVisibility(false);
		return;
	}
	Position.Z = Ground.ImpactPoint.Z + GroundClearance;

	if (!Arrow)
	{
		Arrow = NewObject<UWidgetComponent>(Pawn, TEXT("TaskGroundArrowVisual"));
		Arrow->SetWidgetSpace(EWidgetSpace::World);
		Arrow->SetWidgetClass(UDeliveryGroundArrowWidget::StaticClass());
		Arrow->SetDrawSize(FVector2D(128, 128));
		Arrow->SetTwoSided(true);
		Arrow->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Arrow->SetGenerateOverlapEvents(false);
		Arrow->SetCastShadow(false);
		Arrow->SetOwnerPlayer(Controller->GetLocalPlayer());
		Arrow->SetOnlyOwnerSee(true);
		Arrow->SetWindowFocusable(false);
		Arrow->SetTickWhenOffscreen(false);
		Arrow->RegisterComponent();
	}
	// Widget local X is its normal; screen-up is local Z. Keep normal vertical and screen-up toward XY target.
	Arrow->SetWorldLocationAndRotation(Position,
		FRotationMatrix::MakeFromXZ(FVector::UpVector, Direction).ToQuat());
	Arrow->SetWorldScale3D(FVector(FMath::Max(10.f, ArrowSize) / 128.f));
	Arrow->SetVisibility(true);
}

void UDeliveryGroundArrowComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Arrow)
	{
		Arrow->DestroyComponent();
		Arrow = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}
