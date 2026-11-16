// 实现带任务身份的手持快递物品
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Inventory/Items/DeliveryHandheldItem.h"
#include "DeliveryPackageItem.generated.h"

class UDeliveryItemComponent;
class UDeliveryInventoryComponent;
class ADeliveryCaterpillarItem;
class ADeliveryCompanionCharacter;
class APlayerState;
class UDeliveryCompanionComponent;

/** 保存任务身份、实物携带表现和小绿的投放结果 */
UCLASS(Blueprintable)

class DELIVERY_API ADeliveryPackageItem : public ADeliveryHandheldItem
{
    GENERATED_BODY()

  public:
    ADeliveryPackageItem();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void SetOwner(AActor* NewOwner) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual bool CanPickup(APawn *Interactor, bool bCheckTaskAvailability) const override;
    virtual bool Pickup(APawn *Interactor) override;
    // 归属复制和槽位复制可能先后到达，始终按物品当前Owner选择携带表现
    virtual void RefreshCarrierPresentation() override;
    virtual void OnRep_Owner() override;

    UFUNCTION(BlueprintPure, Category = "Delivery|Caterpillar")
    bool ContainsCaterpillar() const { return bContainsCaterpillar; }
    bool CanInsertCaterpillar(const APawn* Interactor) const;
    // 二狗核对当前手持方或主人登记的地上原件，接触时复核距离和遮挡
    bool CanAcceptCompanionCaterpillar(const ADeliveryCompanionCharacter* Companion,const APlayerState* Requester,
        bool bCheckReach,bool bCompanionHoldsPackage=false) const;
    bool TryInsertCompanionCaterpillar(ADeliveryCompanionCharacter* Companion,ADeliveryCaterpillarItem* Worm,
        const APlayerState* Requester,TFunctionRef<bool()> IsCurrent);
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
    friend class UDeliveryInventoryComponent;
    friend class UDeliveryCompanionComponent;
    // 只在二狗伸手碰到真实地面小绿时提交，包裹始终留在其同一手持槽
    bool TryInsertGroundCaterpillarIntoHeldPackage(ADeliveryCompanionCharacter* Companion,ADeliveryCaterpillarItem* Worm,
        const APlayerState* Requester,TFunctionRef<bool()> IsStillAtContact);
    // 原件转手或销毁时通知放下它的物品栏，避免再次落地后仍提示旧持有者
    TWeakObjectPtr<UDeliveryInventoryComponent> RecoveryInventory;
    void ClearRecoveryInventory();
    bool bHadCarrierPresentation = false;
    // 状态属于这份外卖，丢弃、换手和装车都保留；销毁后不污染下一份外卖
    UPROPERTY(ReplicatedUsing = OnRep_Caterpillar)
    bool bContainsCaterpillar = false;
    bool IsPrankPackageAvailable() const;
    bool HasCaterpillarInsertionReach(const APawn* Interactor,const AActor* Worm) const;
    void CommitCaterpillar(ADeliveryCaterpillarItem* Worm);
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
