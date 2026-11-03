// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "World/DeliveryLocationMarker.h"
#include "DeliveryDropOffPoint.generated.h"

class UDeliveryInteractableComponent;
class UDeliveryTargetComponent;
class UDeliveryTaskDefinition;

/**
 * 收件点，集中配置地点、交互和收件组件
 * 在关卡中填写地点编号和指定任务后，即可接收对应快递
 */
UCLASS()

class DELIVERY_API ADeliveryDropOffPoint : public ADeliveryLocationMarker
{
	GENERATED_BODY()

  public:
	ADeliveryDropOffPoint();

	// 收件配置
	/** 这里收哪个任务的快递；转发给内部的 TargetComponent，所以在 Actor 详情里就能填 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Delivery|Location")
	TObjectPtr<UDeliveryTaskDefinition> ExpectedTask;

	/** 玩家离多近才能交付（cm）；转发给收件组件，而它会把这个值写进可交互组件 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Delivery|Location",
	          meta = (ClampMin = "0.0", Units = "cm"))
	float InteractRadius = 180.f;

	// 组件查询
	UDeliveryTargetComponent* GetTarget() const
	{
		return Target;
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
	TObjectPtr<UDeliveryTargetComponent> Target;
};
