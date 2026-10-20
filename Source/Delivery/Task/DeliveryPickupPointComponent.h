// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryPickupPointComponent.generated.h"

class ADeliveryCharacter;
class ADeliveryPackageItem;
class UDeliveryTaskDefinition;

/**
 * 取货点：按住 E 从这里**领**一件快递，而不是去捡关卡里预先摆好的那个箱子。
 *
 * —— 为什么改成从取货点领 ——
 *
 * 旧流程是把快递 Actor 手摆在取货点地上，玩家走过去捡。两个问题：
 *   1. **掉出世界就死锁**。快递 Actor 一销毁，`UDeliveryItemComponent::EndPlay`
 *      上报丢件、任务退回待取件——但地上那件已经没了，玩家回到取货点什么都捡不到，
 *      这一局再也推不动。现在快递是**按需生成**的，退回待取件之后回来再领一件就行。
 *   2. 一个取货点只能服务一个任务（摆的是哪件就是哪件）。现在这里不填任务，
 *      而是**按地点 ID 反查**：登记在册、状态是待取件、且 `PickupLocationId`
 *      等于本点 ID 的那个任务。所以同一个取货点能在一局里先后发好几个任务的快递，
 *      和 `Tasks.csv` 里"每个任务各自写取货点"的数据形状正好对上。
 *
 * 领到之后放哪由 `ADeliveryCharacter::ReceiveTaskPackage()` 决定——步行拿手上、
 * 骑车绑车上。本组件不关心这件事，它只负责"该不该给"和"生成一件给出去"。
 *
 * 配套的可交互组件由宿主 Actor 提供（`ADeliveryPickupPoint` 已经装好了）。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryPickupPointComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryPickupPointComponent();

	/**
	 * 发什么快递。留空时退回内置的测试快递——摆了取货点却一件都领不到、
	 * 又没有任何报错，是最难查的那种空配置，所以这里宁可发个方块也不发空。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Pickup")
	TSubclassOf<ADeliveryPackageItem> PackageClass;

	/**
	 * 生成位置相对取货点的偏移。
	 *
	 * 生成出来之后马上就被 `ReceiveTaskPackage` 收走（进手里或上车），
	 * 所以这个偏移只影响"收走之前那一瞬间它在哪"。抬高一点是为了避免
	 * 生成在地面里被物理弹一下——虽然看不见，但 Chaos 会报穿透。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Pickup")
	FVector SpawnOffset = FVector(0.f, 0.f, 60.f);

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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Delivery|Pickup", meta=(ClampMin="0.0", Units="cm"))
	float InteractRadius = 180.f;

	/** 这个点此刻该发哪个任务的快递；没有就返回 null。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Pickup")
	UDeliveryTaskDefinition* ResolvePendingTask() const;

	/** 现在能不能领。UI 和交互门槛都问这一句。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Pickup")
	bool CanDispense() const;

	/**
	 * 不能领的原因，空文本表示可以领。
	 *
	 * 和 `UDeliveryItemComponent::GetPickupBlockedReason()` 一样的约定：提示浮窗
	 * 拿它替掉默认提示词。"按了没反应"在游戏里完全无法自查，必须说出原因。
	 */
	FText GetBlockedReason() const;

	/** 服务器：生成一件快递交给 Receiver。成功返回 true。 */
	UFUNCTION(BlueprintCallable, Category="Delivery|Pickup")
	bool TryDispense(ADeliveryCharacter* Receiver);

	/** 取 Actor 身上第一个取货点组件。服务器复核客户端请求时用。 */
	static UDeliveryPickupPointComponent* FindOn(const AActor* Actor);

	/** 本点的地点 ID。取自同一个 Actor 上的 `UDeliveryLocationComponent`。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Pickup")
	FName GetLocationId() const;

	/**
	 * 把同一个 Actor 上的可交互组件修正成"按住 E"。
	 *
	 * **为什么要自己改**：手加 `DeliveryInteractableComponent` 时它用的是组件自己的
	 * 默认值（`GeneralF` + `HoldDuration 0`），而取货点必须是 `PickupE` +
	 * 非零长按——`HoldDuration` 为 0 时走的是"点一下"那条路，**而那条路只认
	 * `ADeliveryHandheldItem`**，取货点会被静默跳过。两个值漏任一个的表现都是
	 * "走过去按 E 完全没反应"，而且没有任何报错。这种只能从代码里消掉，
	 * 不能靠摆点的人记住。
	 *
	 * **只改还停在默认值的**（`GeneralF` 且 `HoldDuration == 0`）——那个组合
	 * 永远不可能是一个能用的取货点，所以改它不会覆盖任何有意的配置。
	 * 和摩托车 `AutoConfigureFromMeshes()` 的"只填空的"是同一条规矩。
	 */
	void EnsureInteractableConfigured();

protected:

	virtual void BeginPlay() override;
};
