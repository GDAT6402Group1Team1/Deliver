// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryObjectiveRingComponent.generated.h"

class UDecalComponent;
class UMaterialInstanceDynamic;
class UDeliveryInteractableComponent;

/**
 * 在任务目的地显示交互范围圆圈
 * 目的地由任务追踪组件提供，圆圈不再单独判断任务阶段
 * 使用贴花贴合地形，并支持打包后的游戏显示
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryObjectiveRingComponent : public UActorComponent
{
	GENERATED_BODY()

  public:
	UDeliveryObjectiveRingComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// 显示开关与材质
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Objective Ring")
	bool bShowObjectiveRing = true;

	/**
	 * 圆圈贴花材质，运行时按需加载
	 * 未配置时仅开发版本使用调试线显示
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Objective Ring")
	TSoftObjectPtr<UMaterialInterface> RingMaterial = TSoftObjectPtr<UMaterialInterface>(
	    FSoftObjectPath(TEXT("/Game/UI/Task/M_ObjectiveRing.M_ObjectiveRing")));

	// 投影与颜色过渡
	/**
	 * 贴花向下投影的深度，单位为厘米
	 * 需要覆盖目标点到地面的高度差，否则圆圈无法显示到地面
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Objective Ring",
	          meta = (ClampMin = "50", Units = "cm"))
	float ProjectionDepth = 600.0f;

	/** 圈的呼吸周期（秒）；0 = 不呼吸 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Objective Ring", meta = (ClampMin = "0"))
	float PulsePeriod = 1.8f;

	/** 呼吸时不透明度在 [1-Amount, 1] 之间走 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Objective Ring",
	          meta = (ClampMin = "0", ClampMax = "1"))
	float PulseAmount = 0.35f;

	/** 还没进范围时的颜色 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Objective Ring")
	FLinearColor FarColor = FLinearColor(1.0f, 0.55f, 0.08f, 1.0f);

	/** 已经进范围、可以按 E 时的颜色 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Objective Ring")
	FLinearColor NearColor = FLinearColor(0.25f, 0.95f, 0.38f, 1.0f);

	/** 远近配色之间的过渡速度（每秒）；直接切换会在边界上闪 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Objective Ring",
	          meta = (ClampMin = "0.1"))
	float ColorBlendSpeed = 6.0f;

  protected:
	// 运行时贴花
	/** 贴花实体；运行时建、本机独有，关卡 Outliner 里看不到它 */
	UPROPERTY(Transient)
	TObjectPtr<UDecalComponent> Decal;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> RingMID;

	/** 当前配色的插值位置，0=远 1=近 */
	float NearBlend = 0.0f;

  private:
	// 任务范围查询
	/** 算出这一帧圈该画在哪、多大；返回 假 表示这一帧不该显示 */
	bool ResolveRing(const APawn* Pawn, FVector& OutCenter, float& OutRadius, bool& bOutInRange) const;

	void HideRing();
};
