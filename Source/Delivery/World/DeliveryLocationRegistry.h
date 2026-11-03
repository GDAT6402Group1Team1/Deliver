// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DeliveryLocationRegistry.generated.h"

class UDeliveryLocationComponent;

/**
 * 通过地点编号查找关卡中的对象，避免界面每次更新都遍历关卡
 * 同一编号可登记多个对象，默认返回最先登记的对象，也可查询全部结果
 */
UCLASS()

class DELIVERY_API UDeliveryLocationRegistry : public UWorldSubsystem
{
	GENERATED_BODY()

  public:
	/** 从任意 WorldContext 拿到本关的注册表；拿不到返回 空指针，调用方要判 */
	UFUNCTION(BlueprintPure, Category = "Delivery|Location", meta = (WorldContext = "WorldContextObject"))
	static UDeliveryLocationRegistry* Get(const UObject* WorldContextObject);

	// 地点登记与注销
	/** 注册一个地点；由 UDeliveryLocationComponent 在 BeginPlay 时自己调，一般不用手动调 */
	void Register(FName LocationId, UDeliveryLocationComponent* Component);

	/** 注销；EndPlay 时调，Actor 被销毁/关卡卸载都会走到 */
	void Unregister(FName LocationId, UDeliveryLocationComponent* Component);

	// 位置与对象查询
	/** 解析成 Actor；找不到返回 空指针 */
	UFUNCTION(BlueprintPure, Category = "Delivery|Location")
	AActor* ResolveActor(FName LocationId) const;

	/**
	 * 解析成世界坐标
	 * 
	 * 取的是地点组件自己的世界位置（组件可以带相对偏移），不是 Actor 原点——
	 * 门口的收件点挂在整栋楼的 Actor 上时，Actor 原点可能在楼中心甚至地下
	 */
	UFUNCTION(BlueprintPure, Category = "Delivery|Location")
	bool ResolveLocation(FName LocationId, FVector& OutLocation) const;

	/** 同一个 ID 下的所有地点 */
	UFUNCTION(BlueprintPure, Category = "Delivery|Location")
	TArray<AActor*> ResolveAllActors(FName LocationId) const;

	/** 同一个 ID 下的所有地点组件；任务目标高亮要从组件读取它对应的建筑 Actor */
	UFUNCTION(BlueprintPure, Category = "Delivery|Location")
	TArray<UDeliveryLocationComponent*> ResolveAllComponents(FName LocationId) const;

	/** 关卡里现在注册了哪些 ID；调试和"策划表里的 ID 有没有对应的点"这类校验用 */
	UFUNCTION(BlueprintPure, Category = "Delivery|Location")
	TArray<FName> GetRegisteredIds() const;

  private:
	// 登记数据与失效清理
	/**
	 * 用弱引用：地点组件的生命周期归它自己的 Actor 管，注册表不该把它钉住
	 * 正常路径上 EndPlay 会注销，但关卡切换、Actor 被强制销毁这些路径不保证
	 * 一定走到，弱引用让漏掉的条目自然失效而不是变成悬垂指针
	 */
	TMap<FName, TArray<TWeakObjectPtr<UDeliveryLocationComponent>>> Locations;

	/** 顺手清掉已经失效的弱引用；解析时调用，不需要单独的清理时机 */
	void CompactEntries(FName LocationId);
};
