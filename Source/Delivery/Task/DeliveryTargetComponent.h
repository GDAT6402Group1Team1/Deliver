// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryTargetComponent.generated.h"

class APlayerState;
class UDeliveryTaskDefinition;

/**
 * 挂在收件人（NPC 或投递点）身上，声明"这里收哪个任务的快递"。
 * 交互系统在玩家按下交付键时调 TryDeliver，交付成功由本组件销毁快递 Actor。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryTargetComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryTargetComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Task")
	TObjectPtr<UDeliveryTaskDefinition> ExpectedTask;

	/** 交付判定距离，从收件人算起。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Task", meta=(ClampMin="0.0", Units="cm"))
	float DeliveryRadius = 100.f;

	/**
	 * 玩家离多近才能取件（cm）。**这是半径唯一的填写位置。**
	 *
	 * 本组件**每次 BeginPlay 都会把它写进可交互组件**，不管那个组件是自动建的
	 * 还是手加的——所以"调了不生效"这件事不可能发生。代价是手加的可交互组件上
	 * 那个 `InteractRadius` 会被盖掉，这是刻意的：之前按"自动建的用这个、手加的用那个"
	 * 分流，结果是同一个概念有三个填写位置（组件上、可交互组件上、现成 Actor 上），
	 * 问一次"怎么调半径"要分三种情况回答。**一个旋钮永远生效 > 三个旋钮各管一段。**
	 *
	 * 半径是从**锚点**（地点组件的位置）算起，不是 Actor 原点。挂在整栋楼上时
	 * 先把地点组件拖到门口再谈半径——起点不对的话调多大都别扭
	 * （调到门口够用时，站楼背面也能按）。`Delivery.Pickup.Dump` 会把
	 * 测量点、实际距离和这个半径一起打出来。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Task", meta=(ClampMin="0.0", Units="cm"))
	float InteractRadius = 180.f;

	/** 手里这件快递现在能不能交给我。UI 用它决定要不要显示交付提示。 */
	UFUNCTION(BlueprintPure, Category="Task")
	bool CanAcceptDelivery(AActor* ItemActor, APlayerState* Player) const;

	/**
	 * 交付判距离时，"收件点在哪"取这个点。
	 *
	 * **必须和可交互组件的测量点是同一个**（锚点 > 根碰撞表面 > Actor 原点）。
	 * 用 Actor 原点的话，挂在整栋楼上时提示浮窗在门口出现、`CanInteract` 也过，
	 * 但走到这一步会按楼心算距离并失败——玩家看到的是
	 * **"提示出来了、按住 E 了、什么都没发生"**，而日志里一句话都没有。
	 * 这和 `DeliveryRadius`/`InteractRadius` 不一致是同一类错，只是走另一条代码路径。
	 */
	UFUNCTION(BlueprintPure, Category="Task")
	FVector GetDeliveryPoint(FVector FromLocation) const;

	/** 服务器：完成交付。成功后销毁快递 Actor，任务对所有玩家结束，奖励结算给 Player。 */
	UFUNCTION(BlueprintCallable, Category="Task")
	bool TryDeliver(AActor* ItemActor, APlayerState* Player);

	/**
	 * 把同一个 Actor 上的可交互组件修正成"按住 E"，并对齐交付半径。
	 *
	 * 和取货点那边同一个坑：手加 `DeliveryInteractableComponent` 时它是
	 * `GeneralF` + `HoldDuration 0`，而交付必须是 `PickupE` + 非零长按，
	 * 漏任一个的表现都是"走过去按 E 没反应"且没有报错。只纠正还停在默认值的那套。
	 *
	 * 顺带把 `DeliveryRadius` 对齐 `InteractRadius`：两个数不一样会出现
	 * "提示出来了但按不动"（提示走前者、交付走后者），而玩家只看得到后半截。
	 */
	void EnsureInteractableConfigured();

protected:

	virtual void BeginPlay() override;
};
