#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DeliveryMinimapWidget.generated.h"

class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class ADeliveryCompanionCharacter;

/**
 * @brief 右上角圆形小地图 WBP_Minimap 的父类，只在本机运行
 *
 * 每个本机玩家有自己的俯拍镜头 Capture 和纹理 MapTexture，镜头挂在玩家控制器上、按 CaptureRate 跟着当前角色拍
 * NativeTick 算出身体朝向、任务目的地和二狗在地图上的位置；NativePaint 画圆形地图、边框、中间的方向箭头，
 * 目的地和二狗在范围内画圆点，超出时夹到圆边画成朝外的三角
 * 地图朝北的方向读项目设置 UDeliveryMapSettings::MapNorthWorldYaw，和罗盘共用一个值
 */
UCLASS()

class DELIVERY_API UDeliveryMinimapWidget : public UUserWidget
{
	GENERATED_BODY()

  public:

	// ── 配置 ──

	/// 地图直径，Slate 单位
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Minimap",
	          meta = (ClampMin = "120", ClampMax = "600"))
	float MapDiameter = 320.f;

	/// 地图直径对应的世界宽度，cm；原来 24000，缩到 18000 后附近建筑显得更大
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Minimap",
	          meta = (ClampMin = "1000", Units = "cm"))
	float MapWorldWidth = 18000.f;

	// 地图朝哪个方向为上不存在这里，读 UDeliveryMapSettings::MapNorthWorldYaw，和罗盘的 N/E/S/W 共用一个值
	// 原来这里的 MapUpWorldYaw 和罗盘节点上的 NorthWorldYaw 是同一个值的两份副本，已经删掉

	/// 俯拍镜头离角色多高，cm
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Minimap",
	          meta = (ClampMin = "1000", Units = "cm"))
	float CaptureHeight = 20000.f;

	/// 每秒拍几次
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Minimap",
	          meta = (ClampMin = "1", ClampMax = "60"))
	float CaptureRate = 30.f;

	/// 圆形边框宽度
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Minimap",
	          meta = (ClampMin = "2", ClampMax = "20"))
	float BorderWidth = 6.f;

	/// 二狗标记的填充颜色；比例尺和超出范围时的画法和任务目的地一样
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Minimap|Companion")
	FColor CompanionMarkerColor = FColor(50, 214, 255);

  protected:
	// ── 生命周期 ──

	/// @brief 保留 WBP_Minimap 在主 HUD 里的位置，方形 Image 不显示，改由 NativePaint 画圆形
	virtual TSharedRef<SWidget> RebuildWidget() override;

	/// @brief 摆到右上角，调用 EnsureCapture 建好镜头
	virtual void NativeConstruct() override;

	/// @brief 销毁镜头和纹理，恢复被停用的旧镜头
	virtual void NativeDestruct() override;

	/// @brief 每帧：读身体朝向，把任务目的地换算到地图坐标，更新二狗标记，镜头跟到角色上方并按 CaptureRate 拍一次
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/// @brief 画圆形地图、边框、中间的方向箭头，以及任务目的地和二狗的标记
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	                          const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	                          int32 LayerId, const FWidgetStyle& InWidgetStyle,
	                          bool bParentEnabled) const override;

  private:
	// ── 镜头 ──

	/// @brief 在玩家控制器上建好俯拍镜头和 512×512 的纹理；已经建过时直接返回
	void EnsureCapture();

	/// @brief 旧人物蓝图里还有一个往共享纹理拍的镜头，停掉它，避免重复渲染；控件销毁时恢复
	void SuspendLegacyCaptures();

	/// @brief 找到自己的二狗（没找到时每 0.5 秒找一次），把它相对玩家的位置换算到地图坐标
	void UpdateCompanionMarker(float DeltaTime);

	/// 本机的俯拍镜头
	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> Capture;

	/// 镜头拍到的纹理
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> MapTexture;

	/// 被停用的旧镜头和它原来的设置，销毁时还原
	struct FLegacyCaptureState
	{
		TWeakObjectPtr<USceneCaptureComponent2D> Component;
		bool bEveryFrame = false;
		bool bOnMovement = false;
	};

	TArray<FLegacyCaptureState> LegacyCaptures;

	// ── 绘制用的数据 ──
	// NativeTick 算好存下，NativePaint 只读

	/// 地图纹理的画刷
	FSlateBrush MapBrush;

	/// 身体朝向（世界 Yaw），步行时取布娃娃身体的朝向，骑车时取车的朝向
	float HeadingYaw = 0.f;

	/**
	 * 任务目标相对玩家的地图偏移，单位为厘米，横轴向右、纵轴向上
	 * 更新时缓存结果，绘制时直接读取，避免重复查询任务系统
	 */
	FVector2f TargetMapOffsetCm = FVector2f::ZeroVector;

	/// 当前有任务目的地要画
	bool bHasMapTarget = false;

	/// 二狗相对玩家的地图偏移，cm，横轴向右、纵轴向上
	FVector2f CompanionMapOffsetCm = FVector2f::ZeroVector;

	/// 自己的二狗
	TWeakObjectPtr<ADeliveryCompanionCharacter> MapCompanion;

	/// 距上次找二狗过了多久
	float CompanionSearchElapsed = .5f;

	/// 当前有二狗要画
	bool bHasMapCompanion = false;

	/// 距上次拍过了多久
	float CaptureElapsed = 1.f;

	/// 距上次找旧镜头过了多久，每秒找一次
	float LegacyScanElapsed = 1.f;

	/// 上一帧跟着的角色；换了角色（例如上下车）时下一次检查就立刻拍
	TWeakObjectPtr<APawn> LastPawn;
};
