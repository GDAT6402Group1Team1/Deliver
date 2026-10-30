// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryObjectiveRingComponent.generated.h"

class UDecalComponent;
class UMaterialInstanceDynamic;
class UDeliveryInteractableComponent;

/**
 * 在**当前任务目的地**的地面上画一个圈，告诉玩家走到多近才能按 E。
 *
 * —— 为什么在 Task/ 而不是 Interaction/ ——
 *
 * 它和脚边箭头、罗盘图钉、小地图标记是同一类东西：**都回答"该去哪"**，
 * 也都必须读同一个 `GetTrackedTaskDestination()`。放进 Interaction 模块的话，
 * 一是通用交互框架要反向依赖任务系统，二是多出"第二处判任务状态"——
 * CLAUDE.md 记过那种不一致长什么样：「箭头指送达点、图钉还是箱子」。
 *
 * 目的地按任务阶段自动切换（待取件→取货点，进行中→收件点）由追踪器负责，
 * 这里一个 if 都不写。
 *
 * —— 为什么用 Decal 不用画线 ——
 *
 * 第一版用 `DrawDebugLine` 逐点打射线贴地，两个问题：
 *   * **`DrawDebug` 在 Shipping 里是空的**。圈一旦只在当前目标上显示，
 *     它就从"摆点时的调试辅助"变成了玩家指引，打包版里没有等于功能没做。
 *   * 48 条射线/帧只为了贴合地形，而 Decal 投影天生就贴合地形、台阶和斜坡。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryObjectiveRingComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryObjectiveRingComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Objective Ring")
	bool bShowObjectiveRing = true;

	/**
	 * 圈用的贴花材质。留空时退回调试画线（非 Shipping），**不会什么都不显示**。
	 *
	 * 软引用而不是 `ConstructorHelpers`：构造函数只在模块加载时跑一次，
	 * 脚本新建的资产那一次会话里永远解析不到（摩托车的 `InteractAction` 同理）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Objective Ring")
	TSoftObjectPtr<UMaterialInterface> RingMaterial =
		TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/UI/Task/M_ObjectiveRing.M_ObjectiveRing")));

	/**
	 * 贴花往下投影多深（cm）。
	 *
	 * **不能给小了**：目的地的测量点常常不在地面上（门口锚点可能抬高、
	 * 挂在整栋楼上时甚至在楼心），投影深度不够圈就整个打不到地上、什么都看不见，
	 * 而那和"没启用"长得一模一样。给足深度的代价只是贴花盒子变高。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Objective Ring", meta=(ClampMin="50", Units="cm"))
	float ProjectionDepth = 600.0f;

	/** 圈的呼吸周期（秒）。0 = 不呼吸。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Objective Ring", meta=(ClampMin="0"))
	float PulsePeriod = 1.8f;

	/** 呼吸时不透明度在 [1-Amount, 1] 之间走。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Objective Ring", meta=(ClampMin="0", ClampMax="1"))
	float PulseAmount = 0.35f;

	/** 还没进范围时的颜色。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Objective Ring")
	FLinearColor FarColor = FLinearColor(1.0f, 0.55f, 0.08f, 1.0f);

	/** 已经进范围、可以按 E 时的颜色。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Objective Ring")
	FLinearColor NearColor = FLinearColor(0.25f, 0.95f, 0.38f, 1.0f);

	/** 远近配色之间的过渡速度（每秒）。直接切换会在边界上闪。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Objective Ring", meta=(ClampMin="0.1"))
	float ColorBlendSpeed = 6.0f;

protected:

	/** 贴花实体。运行时建、本机独有，关卡 Outliner 里看不到它。 */
	UPROPERTY(Transient)
	TObjectPtr<UDecalComponent> Decal;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> RingMID;

	/** 当前配色的插值位置，0=远 1=近。 */
	float NearBlend = 0.0f;

private:

	/** 算出这一帧圈该画在哪、多大。返回 false 表示这一帧不该显示。 */
	bool ResolveRing(const APawn* Pawn, FVector& OutCenter, float& OutRadius,
		bool& bOutInRange) const;

	void HideRing();
};
