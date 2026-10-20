#include "DeliveryMinimapWidget.h"
#include "DeliveryCharacter.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"

#include "Components/CanvasPanelSlot.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Layout/SBox.h"

namespace DeliveryMinimapDrawing
{
	// 纹理直接映射到圆形三角扇，没有方形背景漏出，也无需额外遮罩材质。
	void Disc(FSlateWindowElementList& Elements, int32 Layer, const FGeometry& Geometry,
		FVector2f Centre, float Radius, const FSlateBrush& Brush, FColor Color)
	{
		constexpr int32 Segments = 128;
		TArray<FSlateVertex> Vertices;
		TArray<SlateIndex> Indices;
		Vertices.Reserve(Segments + 2);
		Indices.Reserve(Segments * 3);
		const FSlateRenderTransform Transform = Geometry.GetAccumulatedRenderTransform();
		Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Centre, FVector2f(.5f, .5f), Color));
		for (int32 I = 0; I <= Segments; ++I)
		{
			const float Angle = 2.f * PI * I / Segments;
			const FVector2f Unit(FMath::Cos(Angle), FMath::Sin(Angle));
			Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform,
				Centre + Unit * Radius, FVector2f(.5f, .5f) + Unit * .5f, Color));
			if (I > 0)
			{
				Indices.Add(0); Indices.Add(I); Indices.Add(I + 1);
			}
		}
		FSlateDrawElement::MakeCustomVerts(Elements, Layer,
			FSlateApplication::Get().GetRenderer()->GetResourceHandle(Brush), Vertices, Indices, nullptr, 0, 0,
			ESlateDrawEffect::IgnoreTextureAlpha);
	}

	void DirectionMarker(FSlateWindowElementList& Elements, int32 Layer, const FGeometry& Geometry,
		FVector2f Centre, float Heading, float Scale, FColor LeftColor, FColor RightColor)
	{
		const float Radians = FMath::DegreesToRadians(Heading);
		const float Sin = FMath::Sin(Radians), Cos = FMath::Cos(Radians);
		const auto Rotate = [&](float X, float Y)
		{
			return Centre + FVector2f(X * Cos - Y * Sin, X * Sin + Y * Cos) * Scale;
		};
		const FSlateRenderTransform Transform = Geometry.GetAccumulatedRenderTransform();
		TArray<FSlateVertex> Vertices;
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const FColor Color = Side == 0 ? LeftColor : RightColor;
			for (const FVector2f& Point : {Rotate(0, -21), Rotate(Side == 0 ? -13 : 13, 16), Rotate(0, 8)})
			{
				Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Point, FVector2f(.5f, .5f), Color));
			}
		}
		const TArray<SlateIndex> Indices = {0, 1, 2, 3, 5, 4};
		const FSlateBrush& White = *FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
		FSlateDrawElement::MakeCustomVerts(Elements, Layer,
			FSlateApplication::Get().GetRenderer()->GetResourceHandle(White), Vertices, Indices, nullptr, 0, 0);
	}
}

TSharedRef<SWidget> UDeliveryMinimapWidget::RebuildWidget()
{
	// 保留 WBP_Minimap 的引用和主 HUD 插槽；方形 Image 由本类的圆形绘制替代。
	return SNew(SBox)
		.WidthOverride_Lambda([this]() { return MapDiameter; })
		.HeightOverride_Lambda([this]() { return MapDiameter; });
}

void UDeliveryMinimapWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetVisibility(ESlateVisibility::HitTestInvisible);
	ForceVolatile(true);
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
	{
		// 原位置在右上角；加大后仍保留屏幕内边距，圆环不贴着屏幕边缘。
		CanvasSlot->SetAnchors(FAnchors(1.f, 0.f));
		CanvasSlot->SetAlignment(FVector2D(1.f, 0.f));
		CanvasSlot->SetPosition(FVector2D(-24.f, 24.f));
		CanvasSlot->SetAutoSize(true);
	}
	EnsureCapture();
}

void UDeliveryMinimapWidget::EnsureCapture()
{
	APlayerController* Controller = GetOwningPlayer();
	if (Capture || !Controller || !Controller->IsLocalController() || IsDesignTime()) return;

	MapTexture = NewObject<UTextureRenderTarget2D>(this);
	MapTexture->ClearColor = FLinearColor::Black;
	MapTexture->RenderTargetFormat = RTF_RGBA8;
	MapTexture->InitAutoFormat(512, 512);
	MapBrush.SetResourceObject(MapTexture);
	MapBrush.ImageSize = FVector2D(512, 512);
	MapBrush.DrawAs = ESlateBrushDrawType::Image;
	MapBrush.Tiling = ESlateBrushTileType::NoTile;

	Capture = NewObject<USceneCaptureComponent2D>(Controller);
	Capture->ProjectionType = ECameraProjectionMode::Orthographic;
	Capture->OrthoWidth = MapWorldWidth;
	Capture->TextureTarget = MapTexture;
	Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->bAlwaysPersistRenderingState = true;
	Capture->SetWorldRotation(FRotator(-90.f, 0.f, 0.f));
	Capture->RegisterComponent();
	SuspendLegacyCaptures();
}

void UDeliveryMinimapWidget::SuspendLegacyCaptures()
{
	// 旧人物蓝图仍保留组件，但不再重复渲染那张共享 RT；每名本机玩家现在有自己的纹理。
	for (TObjectIterator<USceneCaptureComponent2D> It; It; ++It)
	{
		USceneCaptureComponent2D* Old = *It;
		if (!IsValid(Old) || Old == Capture || Old->GetWorld() != GetWorld() || !Old->TextureTarget
			|| Old->TextureTarget->GetPathName() != TEXT("/Game/Minimap/RT_Minimap.RT_Minimap")) continue;
		if (!LegacyCaptures.ContainsByPredicate([Old](const FLegacyCaptureState& State) { return State.Component == Old; }))
		{
			LegacyCaptures.Add({Old, Old->bCaptureEveryFrame != 0, Old->bCaptureOnMovement != 0});
		}
		Old->bCaptureEveryFrame = false;
		Old->bCaptureOnMovement = false;
	}
}

void UDeliveryMinimapWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	EnsureCapture();
	APawn* Pawn = GetOwningPlayerPawn();
	if (!Capture || !Pawn) return;

	// 朝向取 Pawn 而不是镜头；骑车自由转镜头不会让车头标记指错。
	HeadingYaw = Pawn->GetActorRotation().Yaw;
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Pawn))
	{
		// 主动布娃娃的身体朝向独立于 Pawn 根旋转，和现有出拳瞄准使用同一来源。
		if (const UDeliveryActiveRagdollComponent* Ragdoll = Character->GetActiveRagdoll())
		{
			HeadingYaw = Ragdoll->GetBodyForward().Rotation().Yaw;
		}
	}
	Capture->OrthoWidth = FMath::Max(1000.f, MapWorldWidth);
	Capture->SetWorldLocation(Pawn->GetActorLocation() + FVector(0, 0, CaptureHeight));
	if (LastPawn.Get() != Pawn)
	{
		LastPawn = Pawn;
		CaptureElapsed = 1.f;
	}
	// 中心只画方向符号，避免把真实人物/车模型和标记叠在一起。
	Capture->HiddenActors.Reset();
	Capture->HiddenActors.Add(Pawn);
	TArray<AActor*> Attached;
	Pawn->GetAttachedActors(Attached, true, true);
	for (AActor* Actor : Attached) Capture->HiddenActors.Add(Actor);
	Capture->HiddenActors.Add(GetOwningPlayer());

	LegacyScanElapsed += InDeltaTime;
	if (LegacyScanElapsed >= 1.f)
	{
		LegacyScanElapsed = 0.f;
		SuspendLegacyCaptures();
	}
	CaptureElapsed += InDeltaTime;
	if (CaptureElapsed >= 1.f / FMath::Clamp(CaptureRate, 1.f, 60.f))
	{
		CaptureElapsed = 0.f;
		Capture->CaptureScene();
	}
}

int32 UDeliveryMinimapWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const int32 Base = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements,
		LayerId, InWidgetStyle, bParentEnabled);
	if (!FSlateApplication::IsInitialized()) return Base;
	const FVector2D Size = AllottedGeometry.GetLocalSize();
	const FVector2f Centre(Size.X * .5f, Size.Y * .5f);
	const float Radius = FMath::Min(Size.X, Size.Y) * .5f - 2.f;
	if (Radius <= 0.f) return Base;
	const FSlateBrush& White = *FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
	using namespace DeliveryMinimapDrawing;
	Disc(OutDrawElements, Base + 1, AllottedGeometry, Centre, Radius, White, FColor(15, 20, 25));
	Disc(OutDrawElements, Base + 2, AllottedGeometry, Centre, Radius - 2.f, White, FColor(230, 228, 217));
	const float MapRadius = FMath::Max(1.f, Radius - FMath::Clamp(BorderWidth, 2.f, 20.f));
	Disc(OutDrawElements, Base + 3, AllottedGeometry, Centre, MapRadius, MapTexture ? MapBrush : White,
		MapTexture ? FColor::White : FColor(40, 48, 52));
	// 白描边 + 红色双面尖头，轮廓参考定位标记，尖端代表实际前方。
	const float MarkerScale = FMath::Min(Size.X, Size.Y) / 320.f;
	DirectionMarker(OutDrawElements, Base + 4, AllottedGeometry, Centre + FVector2f(0, 1), HeadingYaw,
		MarkerScale * 1.22f, FColor(20, 20, 20), FColor(20, 20, 20));
	DirectionMarker(OutDrawElements, Base + 5, AllottedGeometry, Centre, HeadingYaw,
		MarkerScale * 1.12f, FColor::White, FColor::White);
	DirectionMarker(OutDrawElements, Base + 6, AllottedGeometry, Centre, HeadingYaw,
		MarkerScale, FColor(239, 58, 44), FColor(183, 32, 26));
	return Base + 6;
}

void UDeliveryMinimapWidget::NativeDestruct()
{
	if (Capture) Capture->DestroyComponent();
	Capture = nullptr;
	MapBrush.SetResourceObject(nullptr);
	MapTexture = nullptr;
	for (const FLegacyCaptureState& State : LegacyCaptures)
	{
		if (USceneCaptureComponent2D* Old = State.Component.Get())
		{
			Old->bCaptureEveryFrame = State.bEveryFrame;
			Old->bCaptureOnMovement = State.bOnMovement;
		}
	}
	LegacyCaptures.Reset();
	LastPawn.Reset();
	Super::NativeDestruct();
}
