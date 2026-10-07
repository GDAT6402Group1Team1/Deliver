// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DeliveryGuidanceLibrary.generated.h"

class UDeliveryTaskDefinition;

/** 一次指引查询的结果。UI 画屏幕边缘箭头、距离提示需要的量都在这里。 */
USTRUCT(BlueprintType)
struct FDeliveryGuidance
{
	GENERATED_BODY()

	/** 有没有可指引的目的地。false 时下面的字段都不要用。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance")
	bool bValid = false;

	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance")
	FVector WorldLocation = FVector::ZeroVector;

	/** 解析用的地点 ID，调试和日志用。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance")
	FName LocationId;

	/** 玩家到目的地的直线距离（厘米）。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance")
	float Distance = 0.f;

	/**
	 * 目的地相对镜头朝向的水平夹角，−180~180。
	 * 0 = 正前方，正数 = 在右边，负数 = 在左边。
	 * 屏幕边缘箭头直接把这个角度转成旋转量即可，不需要自己做投影。
	 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance")
	float RelativeYaw = 0.f;

	/** 目的地在不在镜头前方（|RelativeYaw| < 90）。决定箭头画在屏幕边缘还是目标上方。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance")
	bool bInFront = false;

	/** 相对高度差（目的地 Z − 玩家 Z）。正数说明目的地在上方，UI 可以提示"在楼上"。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance")
	float HeightOffset = 0.f;
};

/**
 * 顶部水平罗盘条需要的全部数据。
 *
 * Offset 字段都是 -1~1：-1 是罗盘条最左边，0 是镜头正前方，+1 是最右边。
 * UI 把它乘以罗盘条宽度的一半，再写进控件的 Render Translation X 即可。
 */
USTRUCT(BlueprintType)
struct FDeliveryCompassGuidance
{
	GENERATED_BODY()

	/** 能否取得本机镜头。false 时整条罗盘隐藏。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance|Compass")
	bool bValid = false;

	/** 当前镜头航向：0=N、90=E、180=S、270=W。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance|Compass")
	float HeadingDegrees = 0.f;

	/** 当前任务是否有可解析的目的地。false 时只显示 NSEW，不显示目标标记。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance|Compass")
	bool bHasTarget = false;

	/** 目标相对镜头的水平夹角，负数在左，正数在右。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance|Compass")
	float TargetRelativeYaw = 0.f;

	/** 目标在罗盘条上的位置；目标在背后时会夹在左/右边缘。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance|Compass")
	float TargetOffset = 0.f;

	/** 目标是否真的落在罗盘条视野内；false 表示 TargetOffset 已被夹到边缘。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance|Compass")
	bool bTargetInStrip = false;

	/** 已限制到 -1~1；方向在身后时停在左/右边缘。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance|Compass")
	float NorthOffset = 0.f;

	/** 是否位于罗盘真实显示角度内；想让方向始终贴边显示时可以忽略此字段。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance|Compass")
	bool bNorthVisible = false;

	/** 已限制到 -1~1；方向在身后时停在左/右边缘。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance|Compass")
	float EastOffset = 0.f;

	/** 是否位于罗盘真实显示角度内；想让方向始终贴边显示时可以忽略此字段。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance|Compass")
	bool bEastVisible = false;

	/** 已限制到 -1~1；方向在身后时停在左/右边缘。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance|Compass")
	float SouthOffset = 0.f;

	/** 是否位于罗盘真实显示角度内；想让方向始终贴边显示时可以忽略此字段。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance|Compass")
	bool bSouthVisible = false;

	/** 已限制到 -1~1；方向在身后时停在左/右边缘。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance|Compass")
	float WestOffset = 0.f;

	/** 是否位于罗盘真实显示角度内；想让方向始终贴边显示时可以忽略此字段。 */
	UPROPERTY(BlueprintReadOnly, Category="Delivery|Guidance|Compass")
	bool bWestVisible = false;
};

/**
 * 把"该去哪"换算成 UI 直接能用的量。
 *
 * 做成函数库而不是塞进追踪组件：这些是纯几何换算，不持有状态，
 * 而且 UMG 里调用函数库比先拿组件再调方法少一层。
 *
 * 距离和角度都基于**镜头**而不是角色：玩家看的是镜头，骑车时镜头和车头
 * 还可能不一致（摩托车有自由视角/固定视角两套）。用角色朝向算的话，
 * 自由视角下箭头会指错。
 */
UCLASS()
class DELIVERY_API UDeliveryGuidanceLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	/** 当前追踪任务的指引。没有追踪任务、或地点解析不出来时 bValid 为 false。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Guidance", meta=(WorldContext="WorldContextObject"))
	static FDeliveryGuidance GetTrackedTaskGuidance(const UObject* WorldContextObject);

	/** 指定任务的指引。任务列表 UI 里显示各任务距离时用。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Guidance", meta=(WorldContext="WorldContextObject"))
	static FDeliveryGuidance GetTaskGuidance(const UObject* WorldContextObject,
		const UDeliveryTaskDefinition* Task);

	/** 任意世界坐标的指引。地图标记、自定义目标点用。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Guidance", meta=(WorldContext="WorldContextObject"))
	static FDeliveryGuidance MakeGuidanceToLocation(const UObject* WorldContextObject,
		FVector WorldLocation);

	/**
	 * 顶部水平罗盘条数据。VisibleAngle 是整条 UI 覆盖的角度，默认显示镜头前方 180°。
	 * NorthWorldYaw 用来校准地图的北方；默认世界 +X（Yaw 0）为北。
	 */
	UFUNCTION(BlueprintPure, Category="Delivery|Guidance", meta=(WorldContext="WorldContextObject"))
	static FDeliveryCompassGuidance GetTrackedTaskCompass(const UObject* WorldContextObject,
		float VisibleAngle = 180.f, float NorthWorldYaw = 0.f);

	/** 把距离格式化成中文显示文本：1200 → "12 米"，150000 → "1.5 公里"。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Guidance")
	static FText FormatDistance(float Centimeters);
};
