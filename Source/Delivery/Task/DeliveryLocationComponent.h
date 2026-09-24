// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "DeliveryLocationComponent.generated.h"

/**
 * 给关卡里的一个点挂上"策划表里的地点编号"。
 *
 * 取件点、收件点、收件人 NPC 都用这一个组件，因为任务定义里那三个字段
 * （PickupLocationId / DeliveryLocationId / ReceiverNpcId）在数据上是同一种东西：
 * 一个需要被解析成世界坐标的外部编号。做成三种组件只会让注册表分三张表，
 * 而查询方（地图指引）其实不关心这个点是干什么用的。
 *
 * 继承 SceneComponent 而不是 ActorComponent，是为了能带相对偏移：
 * 收件点挂在整栋楼上时，楼的原点可能在中心甚至地下，指引箭头该指门口。
 *
 * 和 UDeliveryTargetComponent 的分工：那个管"能不能在这里交差"（规则判定），
 * 这个管"这个编号在世界的哪里"（位置解析）。收件点通常两个都挂。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryLocationComponent : public USceneComponent
{
	GENERATED_BODY()

public:

	UDeliveryLocationComponent();

	/**
	 * 策划表里的地点编号，要和 Tasks.csv 里 PickupLocationId / DeliveryLocationId /
	 * ReceiverNpcId 填的值一致。留空则不注册。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|Location")
	FName LocationId;

	/** 给策划/关卡看的名字，只用于调试输出和编辑器里辨认，不参与任何逻辑。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|Location")
	FText DisplayName;

protected:

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

	/** 记下注册时用的 ID：运行中有人改了 LocationId 的话，注销要用旧值才摘得干净。 */
	FName RegisteredId;
};
