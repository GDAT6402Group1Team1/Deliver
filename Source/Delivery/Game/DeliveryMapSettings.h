// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "DeliveryMapSettings.generated.h"

/**
 * 统一配置地图朝向，供小地图和罗盘共用
 * 在项目设置的游戏分类中修改，配置保存到默认游戏配置文件
 * 当前设置对整个项目生效，不按关卡分别保存
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Delivery 地图"))

class DELIVERY_API UDeliveryMapSettings : public UDeveloperSettings
{
	GENERATED_BODY()

  public:
	UDeliveryMapSettings();

	// 地图朝向配置
	/**
	 * 这张地图的"北"对应世界的哪个 yaw（度）；0 = 世界 +X 朝北
	 * 
	 * 导入进来的地图整体转过 90° 时填 −90（或 90，转反了取负）
	 * 小地图的朝上方向和罗盘的 N/E/S/W 都读这一个值
	 */
	UPROPERTY(config, EditAnywhere, Category = "朝向",
	          meta = (ClampMin = "-180", ClampMax = "180", Units = "deg"))
	float MapNorthWorldYaw = 0.f;

	// 共用配置查询
	/** 取当前值，已归一化到 (−180, 180]；C++ 和蓝图都走这一句 */
	UFUNCTION(BlueprintPure, Category = "Delivery|Map")
	static float GetMapNorthWorldYaw();
};
