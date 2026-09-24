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

	/** 把距离格式化成中文显示文本：1200 → "12 米"，150000 → "1.5 公里"。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Guidance")
	static FText FormatDistance(float Centimeters);
};
