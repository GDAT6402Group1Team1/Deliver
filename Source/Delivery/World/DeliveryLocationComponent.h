// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "DeliveryLocationComponent.generated.h"

/**
 * 为关卡位置登记地点编号，供取件、交付和导航查询
 * 组件可单独调整位置，因此能把目标设在门口而不是模型原点
 * 交付规则由收件组件处理，此处只负责位置
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryLocationComponent : public USceneComponent
{
	GENERATED_BODY()

  public:
	UDeliveryLocationComponent();

	// 地点配置
	/**
	 * 策划表里的地点编号，要和 Tasks.csv 里 PickupLocationId / DeliveryLocationId /
	 * ReceiverNpcId 填的值一致；留空则不注册
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Delivery|Location")
	FName LocationId;

	/** 给策划/关卡看的名字，只用于调试输出和编辑器里辨认，不参与任何逻辑 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Delivery|Location")
	FText DisplayName;

  protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

  private:
	// 登记记录
	/** 记下注册时用的 ID：运行中有人改了 LocationId 的话，注销要用旧值才摘得干净 */
	FName RegisteredId;
};
