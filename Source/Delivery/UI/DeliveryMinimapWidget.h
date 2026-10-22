#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DeliveryMinimapWidget.generated.h"

class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

/** WBP_Minimap 的原生父类：圆形实时地图、边框和居中方向标记。 */
UCLASS()
class DELIVERY_API UDeliveryMinimapWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Slate 单位；面板默认放大至 320。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Minimap", meta=(ClampMin="120", ClampMax="600"))
	float MapDiameter = 320.f;

	/** 横跨地图的世界距离，cm。原来 24000，缩到 18000 后附近建筑更大。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Minimap", meta=(ClampMin="1000", Units="cm"))
	float MapWorldWidth = 18000.f;

	// 地图朝上的方向不在这里存：读 UDeliveryMapSettings::MapNorthWorldYaw，
	// 和罗盘的 N/E/S/W 共用同一个值（见那个文件开头的说明）。
	// 原来这里有个 MapUpWorldYaw，和罗盘节点上的 NorthWorldYaw 是同一个事实的两份抄件

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Minimap", meta=(ClampMin="1000", Units="cm"))
	float CaptureHeight = 20000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Minimap", meta=(ClampMin="1", ClampMax="60"))
	float CaptureRate = 30.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Minimap", meta=(ClampMin="2", ClampMax="20"))
	float BorderWidth = 6.f;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	void EnsureCapture();
	void SuspendLegacyCaptures();

	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> Capture;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> MapTexture;

	struct FLegacyCaptureState
	{
		TWeakObjectPtr<USceneCaptureComponent2D> Component;
		bool bEveryFrame = false;
		bool bOnMovement = false;
	};
	TArray<FLegacyCaptureState> LegacyCaptures;
	FSlateBrush MapBrush;
	float HeadingYaw = 0.f;

	/**
	 * 当前任务目的地相对玩家的偏移，**地图空间、单位 cm**（X=图上右，Y=图上上）。
	 *
	 * 在 Tick 里算好存下来，不在 NativePaint 里查任务系统：Paint 是 const、
	 * 每帧可能被调多次（还有预通道），把 gameplay 查询放进去既浪费也容易出意外。
	 * 现有的 HeadingYaw 也是这个路子。
	 */
	FVector2f TargetMapOffsetCm = FVector2f::ZeroVector;
	bool bHasMapTarget = false;
	float CaptureElapsed = 1.f;
	float LegacyScanElapsed = 1.f;
	TWeakObjectPtr<APawn> LastPawn;
};
