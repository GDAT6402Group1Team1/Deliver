#include "UI/DeliveryMinimapWidget.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"

#include "Components/CanvasPanelSlot.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Game/DeliveryMapSettings.h"
#include "Task/Tracking/DeliveryTaskTrackerComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Layout/SBox.h"

namespace DeliveryMinimapDrawing
{
FVector2f ProjectOffset(const FVector& Delta)
{
	const double North = FMath::DegreesToRadians(UDeliveryMapSettings::GetMapNorthWorldYaw());
	const double Cos = FMath::Cos(North), Sin = FMath::Sin(North);
	return FVector2f(Delta.Y * Cos - Delta.X * Sin, Delta.X * Cos + Delta.Y * Sin);
}

// 纹理直接映射到圆形三角扇，没有方形背景漏出，也无需额外遮罩材质
void Disc(FSlateWindowElementList& Elements, int32 Layer, const FGeometry& Geometry, FVector2f Centre,
          float Radius, const FSlateBrush& Brush, FColor Color)
{
	constexpr int32 Segments = 128;
	TArray<FSlateVertex> Vertices;
	TArray<SlateIndex> Indices;
	Vertices.Reserve(Segments + 2);
	Indices.Reserve(Segments * 3);
	const FSlateRenderTransform Transform = Geometry.GetAccumulatedRenderTransform();
	Vertices.Add(
	    FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Centre, FVector2f(.5f, .5f), Color));
	for (int32 I = 0; I <= Segments; ++I)
	{
		const float Angle = 2.f * PI * I / Segments;
		const FVector2f Unit(FMath::Cos(Angle), FMath::Sin(Angle));
		Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(
		    Transform, Centre + Unit * Radius, FVector2f(.5f, .5f) + Unit * .5f, Color));
		if (I > 0)
		{
			Indices.Add(0);
			Indices.Add(I);
			Indices.Add(I + 1);
		}
	}
	FSlateDrawElement::MakeCustomVerts(
	    Elements, Layer, FSlateApplication::Get().GetRenderer()->GetResourceHandle(Brush), Vertices, Indices,
	    nullptr, 0, 0, ESlateDrawEffect::IgnoreTextureAlpha);
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
			Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Point,
			                                                                FVector2f(.5f, .5f), Color));
		}
	}
	const TArray<SlateIndex> Indices = {0, 1, 2, 3, 5, 4};
	const FSlateBrush& White = *FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
	FSlateDrawElement::MakeCustomVerts(Elements, Layer,
	                                   FSlateApplication::Get().GetRenderer()->GetResourceHandle(White),
	                                   Vertices, Indices, nullptr, 0, 0);
}

void WorldMarker(FSlateWindowElementList& Elements, int32 Layer, const FGeometry& Geometry,
                 FVector2f Centre, FVector2f OffsetCm, float MapRadius, float MapWorldWidth,
                 float MarkerScale, FColor Fill, bool bDiamond)
{
	const float PixelsPerCm = 2.f * MapRadius / MapWorldWidth;
	FVector2f Offset(OffsetCm.X * PixelsPerCm, -OffsetCm.Y * PixelsPerCm);
	const float Dot = (bDiamond ? 7.5f : 5.5f) * MarkerScale;
	const float MaxLength = FMath::Max(1.f, MapRadius - 15.f * MarkerScale - 2.f);
	const float Length = Offset.Size();
	const FColor Edge(2, 12, 30);
	if (Length > MaxLength)
	{
		Offset *= MaxLength / Length;
		const float Heading = FMath::RadiansToDegrees(FMath::Atan2(Offset.X, -Offset.Y));
		DirectionMarker(Elements, Layer, Geometry, Centre + Offset, Heading, MarkerScale * .62f, Edge, Edge);
		DirectionMarker(Elements, Layer + 1, Geometry, Centre + Offset, Heading, MarkerScale * .48f, Fill, Fill);
		return;
	}
	const FVector2f At = Centre + Offset;
	const FSlateBrush& White = *FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
	if (bDiamond)
	{
		const auto Diamond = [&](int32 DrawLayer, float Extent, FColor Color)
		{
			const TArray<FVector2f> Points = {At + FVector2f(0, -Extent), At + FVector2f(Extent, 0),
				At + FVector2f(0, Extent), At + FVector2f(-Extent, 0), At + FVector2f(0, -Extent)};
			FSlateDrawElement::MakeLines(Elements, DrawLayer, Geometry.ToPaintGeometry(), Points,
				ESlateDrawEffect::None, FLinearColor(Color), true, 2.5f * MarkerScale);
		};
		Diamond(Layer, Dot + 2.f * MarkerScale, Edge);
		Diamond(Layer + 1, Dot, Fill);
	}
	else
	{
		Disc(Elements, Layer, Geometry, At, Dot + 1.8f, White, Edge);
		Disc(Elements, Layer + 1, Geometry, At, Dot, White, Fill);
	}
}
}

// 创建小地图控件并准备显示材质
TSharedRef<SWidget> UDeliveryMinimapWidget::RebuildWidget()
{
	// 保留 WBP_Minimap 的引用和主 HUD 插槽；方形 Image 由本类的圆形绘制替代
	return SNew(SBox)
	    .WidthOverride_Lambda(
	        [this]()
	        {
		        return MapDiameter;
	        })
	    .HeightOverride_Lambda(
	        [this]()
	        {
		        return MapDiameter;
	        });
}

// 初始化小地图显示与本机数据
void UDeliveryMinimapWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetVisibility(ESlateVisibility::HitTestInvisible);
	ForceVolatile(true);
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
	{
		// 原位置在右上角；加大后仍保留屏幕内边距，圆环不贴着屏幕边缘
		CanvasSlot->SetAnchors(FAnchors(1.f, 0.f));
		CanvasSlot->SetAlignment(FVector2D(1.f, 0.f));
		CanvasSlot->SetPosition(FVector2D(-24.f, 24.f));
		CanvasSlot->SetAutoSize(true);
	}
	EnsureCapture();
}

// 准备小地图使用的场景捕获组件
void UDeliveryMinimapWidget::EnsureCapture()
{
	APlayerController* Controller = GetOwningPlayer();
	if (Capture || !Controller || !Controller->IsLocalController() || IsDesignTime())
		return;

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
	// 俯拍：Pitch −90 看正下方；此时 Yaw 绕的就是视线轴，所以它直接等于"图像转多少度"
	// 这里设一次是为了创建时就对，真正每帧生效的是下面更新里那一句——
	// 改 WBP 默认值不用重编，靠的就是每帧重设
	Capture->SetWorldRotation(FRotator(-90.f, UDeliveryMapSettings::GetMapNorthWorldYaw(), 0.f));
	Capture->RegisterComponent();
	SuspendLegacyCaptures();
}

// 停用旧捕获组件，避免重复渲染同一张地图
void UDeliveryMinimapWidget::SuspendLegacyCaptures()
{
	// 旧人物蓝图仍保留组件，但不再重复渲染那张共享 RT；每名本机玩家现在有自己的纹理
	for (TObjectIterator<USceneCaptureComponent2D> It; It; ++It)
	{
		USceneCaptureComponent2D* Old = *It;
		if (!IsValid(Old) || Old == Capture || Old->GetWorld() != GetWorld() || !Old->TextureTarget ||
		    Old->TextureTarget->GetPathName() != TEXT("/Game/Minimap/RT_Minimap.RT_Minimap"))
			continue;
		if (!LegacyCaptures.ContainsByPredicate(
		        [Old](const FLegacyCaptureState& State)
		        {
			        return State.Component == Old;
		        }))
		{
			LegacyCaptures.Add({Old, Old->bCaptureEveryFrame != 0, Old->bCaptureOnMovement != 0});
		}
		Old->bCaptureEveryFrame = false;
		Old->bCaptureOnMovement = false;
	}
}

// 更新地图捕获位置、缩放和任务标记
void UDeliveryMinimapWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	EnsureCapture();
	APawn* Pawn = GetOwningPlayerPawn();
	bHasMapCompanion = false;
	bHasMapTarget = false;
	if (!Capture || !Pawn)
		return;

	// 朝向取 Pawn 而不是镜头；骑车自由转镜头不会让车头标记指错
	HeadingYaw = Pawn->GetActorRotation().Yaw;
	if (const ADeliveryCharacter* Character = Cast<ADeliveryCharacter>(Pawn))
	{
		// 主动布娃娃的身体朝向独立于 Pawn 根旋转，和现有出拳瞄准使用同一来源
		if (const UDeliveryActiveRagdollComponent* Ragdoll = Character->GetActiveRagdoll())
		{
			HeadingYaw = Ragdoll->GetBodyForward().Rotation().Yaw;
		}
	}
	// —— 任务目的地在地图上的位置 ——
	
	// 换到地图空间再存：图上的"上"是世界 yaw MapNorthWorldYaw，"右"是它 +90
	// 把世界偏移投影到这两根轴上，Paint 里就只剩"乘个比例尺 + 夹到圆内"，
	// 不用再碰朝向——朝向这件事只在这里算一次，和相机读的是同一个值
	if (const UDeliveryTaskTrackerComponent* Tracker =
	        UDeliveryTaskTrackerComponent::FindTracker(GetOwningPlayer()))
	{
		FVector Destination;
		FName LocationId;
		if (Tracker->GetTrackedTaskDestination(Destination, LocationId))
		{
			TargetMapOffsetCm = DeliveryMinimapDrawing::ProjectOffset(Destination - Pawn->GetActorLocation());
			bHasMapTarget = true;
		}
	}
	UpdateCompanionMarker(InDeltaTime);

	Capture->OrthoWidth = FMath::Max(1000.f, MapWorldWidth);
	// 每帧重设旋转：改项目设置里的 MapNorthWorldYaw 立刻生效，不用重编也不用重开关卡
	Capture->SetWorldRotation(FRotator(-90.f, UDeliveryMapSettings::GetMapNorthWorldYaw(), 0.f));
	Capture->SetWorldLocation(Pawn->GetActorLocation() + FVector(0, 0, CaptureHeight));
	if (LastPawn.Get() != Pawn)
	{
		LastPawn = Pawn;
		CaptureElapsed = 1.f;
	}
	// 中心只画方向符号，避免把真实人物/车模型和标记叠在一起
	Capture->HiddenActors.Reset();
	Capture->HiddenActors.Add(Pawn);
	TArray<AActor*> Attached;
	Pawn->GetAttachedActors(Attached, true, true);
	for (AActor* Actor : Attached)
		Capture->HiddenActors.Add(Actor);
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

void UDeliveryMinimapWidget::UpdateCompanionMarker(float DeltaTime)
{
	const auto* Player = GetOwningPlayer();
	const auto* PlayerState = Player ? Player->PlayerState.Get() : nullptr;
	const auto* Pawn = GetOwningPlayerPawn();
	const auto IsOwnedCompanion = [&](const ADeliveryCompanionCharacter* Npc)
	{
		return IsValid(Npc) && !Npc->IsActorBeingDestroyed() && Npc->GetWorld() == GetWorld() &&
			Npc->Companion->GetOwnerPlayerState() == PlayerState;
	};
	if (!IsValid(PlayerState) || PlayerState->IsInactive())
	{
		MapCompanion.Reset();
		return;
	}
	if (!IsOwnedCompanion(MapCompanion.Get()))
	{
		MapCompanion.Reset();
		CompanionSearchElapsed += DeltaTime;
		if (CompanionSearchElapsed < .5f) return;
		CompanionSearchElapsed = 0.f;
		for (TActorIterator<ADeliveryCompanionCharacter> It(GetWorld()); It; ++It)
		{
			if (!IsOwnedCompanion(*It)) continue;
			MapCompanion = *It;
			break;
		}
	}
	if (const auto* Npc = MapCompanion.Get())
	{
		// 骑乘显示替身时真实二狗随座椅复制位置，沿用同一个标记
		CompanionMapOffsetCm = DeliveryMinimapDrawing::ProjectOffset(Npc->GetActorLocation() - Pawn->GetActorLocation());
		bHasMapCompanion = true;
	}
}

// 绘制小地图中的玩家与任务方向标记
int32 UDeliveryMinimapWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
                                          const FSlateRect& MyCullingRect,
                                          FSlateWindowElementList& OutDrawElements, int32 LayerId,
                                          const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const int32 Base = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId,
	                                      InWidgetStyle, bParentEnabled);
	if (!FSlateApplication::IsInitialized())
		return Base;
	const FVector2D Size = AllottedGeometry.GetLocalSize();
	const FVector2f Centre(Size.X * .5f, Size.Y * .5f);
	const float Radius = FMath::Min(Size.X, Size.Y) * .5f - 2.f;
	if (Radius <= 0.f)
		return Base;
	const FSlateBrush& White = *FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
	using namespace DeliveryMinimapDrawing;
	Disc(OutDrawElements, Base + 1, AllottedGeometry, Centre, Radius, White, FColor(15, 20, 25));
	Disc(OutDrawElements, Base + 2, AllottedGeometry, Centre, Radius - 2.f, White, FColor(230, 228, 217));
	const float MapRadius = FMath::Max(1.f, Radius - FMath::Clamp(BorderWidth, 2.f, 20.f));
	Disc(OutDrawElements, Base + 3, AllottedGeometry, Centre, MapRadius, MapTexture ? MapBrush : White,
	     MapTexture ? FColor::White : FColor(40, 48, 52));
	// 分层绘制人物方向标记的轮廓和填充
	const float MarkerScale = FMath::Min(Size.X, Size.Y) / 320.f;
	// 人物朝向减去地图朝向，得到箭头在地图中的角度
	const float MarkerHeading = HeadingYaw - UDeliveryMapSettings::GetMapNorthWorldYaw();

	DirectionMarker(OutDrawElements, Base + 4, AllottedGeometry, Centre + FVector2f(0, 1), MarkerHeading,
	                MarkerScale * 1.22f, FColor(20, 20, 20), FColor(20, 20, 20));
	DirectionMarker(OutDrawElements, Base + 5, AllottedGeometry, Centre, MarkerHeading, MarkerScale * 1.12f,
	                FColor::White, FColor::White);
	DirectionMarker(OutDrawElements, Base + 6, AllottedGeometry, Centre, MarkerHeading, MarkerScale,
	                FColor(239, 58, 44), FColor(183, 32, 26));

	const float WorldWidth = FMath::Max(1000.f, MapWorldWidth);
	if (bHasMapTarget)
		WorldMarker(OutDrawElements, Base + 7, AllottedGeometry, Centre, TargetMapOffsetCm, MapRadius,
			WorldWidth, MarkerScale, FColor(255, 150, 30), false);
	if (bHasMapCompanion)
		WorldMarker(OutDrawElements, Base + 9, AllottedGeometry, Centre, CompanionMapOffsetCm, MapRadius,
			WorldWidth, MarkerScale, CompanionMarkerColor, true);
	return Base + 10;
}

// 释放地图显示资源并恢复需要恢复的场景状态
void UDeliveryMinimapWidget::NativeDestruct()
{
	if (Capture)
		Capture->DestroyComponent();
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
	MapCompanion.Reset();
	bHasMapCompanion = false;
	CompanionSearchElapsed = .5f;
	Super::NativeDestruct();
}
