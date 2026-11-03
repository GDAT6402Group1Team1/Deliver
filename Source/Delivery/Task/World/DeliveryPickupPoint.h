// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "World/DeliveryLocationMarker.h"
#include "DeliveryPickupPoint.generated.h"

class ADeliveryPackageItem;
class UDeliveryInteractableComponent;
class UDeliveryPickupPointComponent;

/**
 * 可直接放入关卡的取货点，填写地点编号后按当前任务发放快递
 * 默认包含地点、交互和发货组件，现有模型也可通过配置这些组件实现取货
 */
UCLASS()

class DELIVERY_API ADeliveryPickupPoint : public ADeliveryLocationMarker
{
	GENERATED_BODY()

  public:
	ADeliveryPickupPoint();

	// 快递生成配置
	/**
	 * 要生成的快递类型，转交给内部发货组件
	 * 可直接在取货点对象的详情面板中配置
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Delivery|Pickup")
	TSubclassOf<ADeliveryPackageItem> PackageClass;

	/** 快递生成位置相对本点的偏移；同样转发给组件，免得两个面板来回切 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Delivery|Pickup")
	FVector SpawnOffset = FVector(0.f, 0.f, 60.f);

	/** 玩家离多近才能取件（cm）；转发给组件，而组件会把它写进可交互组件 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Delivery|Pickup",
	          meta = (ClampMin = "0.0", Units = "cm"))
	float InteractRadius = 180.f;

	// 组件查询
	UDeliveryPickupPointComponent* GetPickupPoint() const
	{
		return PickupPoint;
	}

	UDeliveryInteractableComponent* GetInteractable() const
	{
		return Interactable;
	}

  protected:
	// 编辑器配置同步
	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UDeliveryInteractableComponent> Interactable;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UDeliveryPickupPointComponent> PickupPoint;
};
