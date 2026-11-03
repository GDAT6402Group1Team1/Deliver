// 实现带任务身份的手持快递物品
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "DeliveryPackageItem.generated.h"

class UDeliveryItemComponent;

/** 带任务信息的快递物体，只能放在手持槽中 */
UCLASS(Blueprintable)

class DELIVERY_API ADeliveryPackageItem : public ADeliveryHandheldItem
{
    GENERATED_BODY()

  public:
    ADeliveryPackageItem();
    virtual bool CanPickup(APawn *Interactor, bool bCheckTaskAvailability) const override;
    virtual bool Pickup(APawn *Interactor) override;

    // 快递任务信息
    UFUNCTION(BlueprintPure, Category = "Delivery")

    UDeliveryItemComponent *GetDeliveryItemComponent() const
    {
        return DeliveryItemComponent;
    }

  protected:
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UDeliveryItemComponent> DeliveryItemComponent;
};

/** 由代码创建的测试快递，用于验证完整任务流程 */
UCLASS(Blueprintable)

class DELIVERY_API ADeliveryTestPackage : public ADeliveryPackageItem
{
    GENERATED_BODY()

  public:
    ADeliveryTestPackage();
    virtual void BeginPlay() override;
};
