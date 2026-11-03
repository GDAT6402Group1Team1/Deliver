// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeliveryHandheldItem.generated.h"

class APawn;
class UDeliveryInteractableComponent;
class UDeliveryInventoryItemComponent;
class UStaticMeshComponent;

/** 可在手中、背包和地面之间转移的物理物品 */
UCLASS(Blueprintable)

class DELIVERY_API ADeliveryHandheldItem : public AActor
{
    GENERATED_BODY()

  public:
    // 普通道具提供默认拾取行为，任务快递覆盖这两个方法
    virtual bool CanPickup(APawn *Interactor, bool bCheckTaskAvailability) const;
    virtual bool Pickup(APawn *Interactor);

    ADeliveryHandheldItem();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

    // 物品使用
    /** 优先执行代码中的斧头动作，不依赖蓝图重写使用事件 */
    void UseFromInventory(APawn *User);

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Item|Attack")
    bool bUsesAxeSwing = false;

    /** 没有配置斧头动作时，由蓝图实现物品的左键使用行为 */
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Item")
    void Use(APawn *User);
    virtual void Use_Implementation(APawn *User);

    // 手持、背包与地面显示
    void SetInventoryPresentation(bool bInHand, bool bInInventory, USceneComponent *HandParent);
    void DropFromInventory(const FVector &WorldLocation, const FVector &InitialVelocity);

    /**
     * 焊到载具的货架上
     *
     * 和手上那套的区别是不走每帧跟随：`HeldAnchor` 那条路是为了过滤 Chaos 的
     * 手部抖动，而载具是运动学的、位置由代码直接写，一点抖都没有；硬附着不但更省，
     * 还天然没有"快递落后车半个身位"的拖影——指数跟随在高速时必然欠一截
     */
    void SetVehicleCargoPresentation(USceneComponent *CargoParent);

    UFUNCTION(BlueprintPure, Category = "Item")

    UDeliveryInventoryItemComponent *GetItemComponent() const
    {
        return ItemComponent;
    }

    UFUNCTION(BlueprintPure, Category = "Item")

    UStaticMeshComponent *GetItemMesh() const
    {
        return ItemMesh;
    }

  protected:
    // 模型与物品组件
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UStaticMeshComponent> ItemMesh;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UDeliveryInventoryItemComponent> ItemComponent;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UDeliveryInteractableComponent> InteractableComponent;

    // 手持位置与跟随参数
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Item|Holding")
    FName HandSocket = TEXT("RightHand");

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Item|Holding")
    FTransform HeldRelativeTransform = FTransform(FRotator(0.0f, 0.0f, 90.0f), FVector::ZeroVector);

    /** 位置和旋转的平滑跟随速度，数值越大越贴近手部，也越容易带出物理抖动 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Item|Holding", meta = (ClampMin = "0.1"))
    float HeldPositionFollowSpeed = 28.0f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Item|Holding", meta = (ClampMin = "0.1"))
    float HeldRotationFollowSpeed = 24.0f;

  private:
    // 本机握持更新
    void UpdateHeldTransform(float DeltaSeconds, bool bSnap);

    TWeakObjectPtr<USceneComponent> HeldAnchor;
    bool bHeldPresentationActive = false;

    UFUNCTION()
    void HandleInteract(APawn *Interactor);
};
