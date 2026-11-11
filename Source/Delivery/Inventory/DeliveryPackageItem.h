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
    virtual void BeginPlay() override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual bool CanPickup(APawn *Interactor, bool bCheckTaskAvailability) const override;
    virtual bool Pickup(APawn *Interactor) override;

    UFUNCTION(BlueprintPure, Category = "Delivery|Caterpillar")
    bool ContainsCaterpillar() const { return bContainsCaterpillar; }
    bool CanInsertCaterpillar(const APawn* Interactor) const;
    FText GetPackagePrompt(const APawn* Interactor) const;

    // 快递任务信息
    UFUNCTION(BlueprintPure, Category = "Delivery")

    UDeliveryItemComponent *GetDeliveryItemComponent() const
    {
        return DeliveryItemComponent;
    }

  protected:
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UDeliveryItemComponent> DeliveryItemComponent;

    /** 只显示已放入的毛毛虫，不参与物理与交互；模型由蓝图配置 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UStaticMeshComponent> CaterpillarVisual;

private:
    // 状态属于这份外卖，丢弃、换手和装车都保留；销毁后不污染下一份外卖
    UPROPERTY(ReplicatedUsing = OnRep_Caterpillar)
    bool bContainsCaterpillar = false;
    UFUNCTION() void OnRep_Caterpillar();
    bool TryInsertCaterpillar(APawn* Interactor);
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
