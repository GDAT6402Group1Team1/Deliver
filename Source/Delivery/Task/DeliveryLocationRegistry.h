// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DeliveryLocationRegistry.generated.h"

class UDeliveryLocationComponent;

/**
 * 把任务定义里的地点 ID 解析成关卡里的实际 Actor。
 *
 * 为什么需要这一层：DeliveryTaskDefinition 里的 PickupLocationId /
 * DeliveryLocationId / ReceiverNpcId 都是裸的 FName，是策划表里的外部编号，
 * 和关卡里的东西没有任何连接。没有解析器的话，"这个任务要送到哪"这个问题
 * 在代码里是答不出来的——地图指引、方向箭头、距离提示全都无从做起。
 *
 * 查找走注册表而不是每次遍历关卡：
 *   - 可交互物、任务点这类东西数量很少，但查询会发生在每帧的 UI 刷新里
 *   - 更要紧的是，这个项目已经被"碰撞查询静默失效"坑过一次
 *     （交通组件的前车探测通道配错、恒为 false，肉眼完全看不出来）。
 *     注册表没有这个失败模式：没注册就是查不到，一目了然。
 *
 * 同一个 ID 允许注册多个 Actor（比如一个收件点由几个触发体组成），
 * 解析时返回最先注册的那个；要全部拿到用 ResolveAll。
 */
UCLASS()
class DELIVERY_API UDeliveryLocationRegistry : public UWorldSubsystem
{
	GENERATED_BODY()

public:

	/** 从任意 WorldContext 拿到本关的注册表。拿不到返回 nullptr，调用方要判。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Location", meta=(WorldContext="WorldContextObject"))
	static UDeliveryLocationRegistry* Get(const UObject* WorldContextObject);

	/** 注册一个地点。由 UDeliveryLocationComponent 在 BeginPlay 时自己调，一般不用手动调。 */
	void Register(FName LocationId, UDeliveryLocationComponent* Component);

	/** 注销。EndPlay 时调，Actor 被销毁/关卡卸载都会走到。 */
	void Unregister(FName LocationId, UDeliveryLocationComponent* Component);

	/** 解析成 Actor。找不到返回 nullptr。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Location")
	AActor* ResolveActor(FName LocationId) const;

	/**
	 * 解析成世界坐标。
	 *
	 * 取的是地点组件自己的世界位置（组件可以带相对偏移），不是 Actor 原点——
	 * 门口的收件点挂在整栋楼的 Actor 上时，Actor 原点可能在楼中心甚至地下。
	 */
	UFUNCTION(BlueprintPure, Category="Delivery|Location")
	bool ResolveLocation(FName LocationId, FVector& OutLocation) const;

	/** 同一个 ID 下的所有地点。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Location")
	TArray<AActor*> ResolveAllActors(FName LocationId) const;

	/** 关卡里现在注册了哪些 ID。调试和"策划表里的 ID 有没有对应的点"这类校验用。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Location")
	TArray<FName> GetRegisteredIds() const;

private:

	/**
	 * 用弱引用：地点组件的生命周期归它自己的 Actor 管，注册表不该把它钉住。
	 * 正常路径上 EndPlay 会注销，但关卡切换、Actor 被强制销毁这些路径不保证
	 * 一定走到，弱引用让漏掉的条目自然失效而不是变成悬垂指针。
	 */
	TMap<FName, TArray<TWeakObjectPtr<UDeliveryLocationComponent>>> Locations;

	/** 顺手清掉已经失效的弱引用。解析时调用，不需要单独的清理时机。 */
	void CompactEntries(FName LocationId);
};
