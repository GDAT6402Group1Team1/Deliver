// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryPickupPointComponent.generated.h"

class ADeliveryCharacter;
class ADeliveryPackageItem;
class UDeliveryTaskDefinition;

/**
 * 根据地点编号查找待领取任务，按需生成对应快递
 * 快递丢失后可重新领取，同一取货点也能先后服务多个任务
 * 生成成功后交给角色接收，由角色决定放在手上还是车上
 */
class APawn;

UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryPickupPointComponent : public UActorComponent
{
    GENERATED_BODY()

  public:
    // 长按交互的目标实现
    bool CanPickup(APawn *Interactor, bool bCheckTaskAvailability) const;
    bool Pickup(APawn *Interactor);

    UDeliveryPickupPointComponent();

    // 快递生成与交互配置
    /**
     * 发什么快递；留空时退回内置的测试快递——摆了取货点却一件都领不到、
     * 又没有任何报错，是最难查的那种空配置，所以这里宁可发个方块也不发空
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Pickup")
    TSubclassOf<ADeliveryPackageItem> PackageClass;

    /**
     * 生成位置相对取货点的偏移
     *
     * 生成出来之后马上就被 `ReceiveTaskPackage` 收走（进手里或上车），
     * 所以这个偏移只影响"收走之前那一瞬间它在哪"；抬高一点是为了避免
     * 生成在地面里被物理弹一下——虽然看不见，但 Chaos 会报穿透
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Pickup")
    FVector SpawnOffset = FVector(0.f, 0.f, 60.f);

    /**
     * 玩家可取件的距离，单位为厘米，从地点组件的位置测量
     * 进入游戏时统一写入交互组件，取件半径只在这里配置
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Delivery|Pickup",
              meta = (ClampMin = "0.0", Units = "cm"))
    float InteractRadius = 180.f;

    // 任务匹配与领取
    /** 这个点此刻该发哪个任务的快递；没有就返回 空指针 */
    UFUNCTION(BlueprintPure, Category = "Delivery|Pickup")
    UDeliveryTaskDefinition *ResolvePendingTask() const;

    /** 现在能不能领；UI 和交互门槛都问这一句 */
    UFUNCTION(BlueprintPure, Category = "Delivery|Pickup")
    bool CanDispense() const;

    /**
     * 不能领的原因，空文本表示可以领
     *
     * 和 `UDeliveryItemComponent::GetPickupBlockedReason()` 一样的约定：提示浮窗
     * 拿它替掉默认提示词；"按了没反应"在游戏里完全无法自查，必须说出原因
     */
    FText GetBlockedReason() const;

    /** 服务器：生成一件快递交给 Receiver；成功返回 真 */
    UFUNCTION(BlueprintCallable, Category = "Delivery|Pickup")
    bool TryDispense(ADeliveryCharacter *Receiver);

    // 地点查询与组件配置
    /** 取 Actor 身上第一个取货点组件；服务器复核客户端请求时用 */
    static UDeliveryPickupPointComponent *FindOn(const AActor *Actor);

    /** 本点的地点 ID；取自同一个 Actor 上的 `UDeliveryLocationComponent` */
    UFUNCTION(BlueprintPure, Category = "Delivery|Pickup")
    FName GetLocationId() const;

    /**
     * 配置长按 E 键取件
     * 交互组件仍使用默认 F 键和立即触发时，改为取件所需的长按配置
     */
    void EnsureInteractableConfigured();

  protected:
    virtual void BeginPlay() override;
};
