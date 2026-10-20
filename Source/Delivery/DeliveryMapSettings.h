// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "DeliveryMapSettings.generated.h"

/**
 * 整张地图共享的朝向设置。
 *
 * —— 为什么要专门立一个设置 ——
 *
 * "这张地图的北在世界哪个方向"是**一个事实**，但它原本被抄在两个地方：
 * 小地图控件的 `MapUpWorldYaw`、罗盘节点上的 `NorthWorldYaw`。
 * 两处都填 −90 时一切正常，而**改了一个忘了另一个**的表现是
 * "小地图正的、罗盘偏 90°"——两个数单看都像是对的，对不上这件事
 * 只能靠人肉比对发现。所以收成一处。
 *
 * 放 `UDeveloperSettings` 而不是关卡 Actor 或 GameState：
 *   * 不用在每张图里记得摆一个 Actor（漏摆的表现是静默退回 0）
 *   * 存在 `Config/DefaultGame.ini` 里，是**文本**——这个项目被 `git pull` 的
 *     autostash 冲掉过两次，而二进制 `.uasset` 冲突只能整份取舍、文本能合并
 *   * 编辑器里在 项目设置 → 游戏 → Delivery 地图 下，改完即生效
 *
 * **代价**：它是整个项目一份，不是每张关卡一份。以后真出现"两张图朝向不同"
 * 再改成按关卡覆盖（WorldSettings 子类），现在没有这个需求。
 */
UCLASS(config=Game, defaultconfig, meta=(DisplayName="Delivery 地图"))
class DELIVERY_API UDeliveryMapSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:

	UDeliveryMapSettings();

	/**
	 * 这张地图的"北"对应世界的哪个 yaw（度）。0 = 世界 +X 朝北。
	 *
	 * 导入进来的地图整体转过 90° 时填 −90（或 90，转反了取负）。
	 * 小地图的朝上方向和罗盘的 N/E/S/W **都读这一个值**。
	 */
	UPROPERTY(config, EditAnywhere, Category="朝向",
		meta=(ClampMin="-180", ClampMax="180", Units="deg"))
	float MapNorthWorldYaw = 0.f;

	/** 取当前值，已归一化到 (−180, 180]。C++ 和蓝图都走这一句。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Map")
	static float GetMapNorthWorldYaw();
};
