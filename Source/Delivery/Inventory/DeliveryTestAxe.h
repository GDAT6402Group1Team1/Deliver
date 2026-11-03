// 创建用于验证拾取和斧头动作的测试物品
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "DeliveryTestAxe.generated.h"

class UStaticMeshComponent;

/** 由代码创建的测试斧头，用于验证拾取、切换和丢弃 */
UCLASS(Blueprintable)

class DELIVERY_API ADeliveryTestAxe : public ADeliveryHandheldItem
{
	GENERATED_BODY()

  public:
	ADeliveryTestAxe();
	virtual void BeginPlay() override;

  protected:
	// 测试斧刃模型
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> AxeHead;
};
