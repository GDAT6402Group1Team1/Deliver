#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryCarryComponent.generated.h"

class ADeliveryPackageItem;
class ADeliveryMotorbike;
class APlayerState;

/** 协调角色背包与车辆货架，统一查询和接收任务快递 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryCarryComponent : public UActorComponent
{
    GENERATED_BODY()
  public:
    // 携带状态
    /** 骑车时先查询货架，否则查询手持槽位 */
    UFUNCTION(BlueprintPure, Category = "Carry")
    ADeliveryPackageItem *GetPackage() const;
    /** 返回角色正在驾驶的车辆 */
    UFUNCTION(BlueprintPure, Category = "Carry")
    ADeliveryMotorbike *GetVehicle() const;
    /** 上车后仍能找到用于任务结算的玩家状态 */
    APlayerState *GetPlayerState() const;

    // 服务器操作
    /** 交付成功后移除携带关系，物品销毁仍由交付点负责 */
    bool ReleasePackage(ADeliveryPackageItem *ExpectedPackage);

    /** 接收失败时保留原来的携带状态 */
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Carry")
    bool TryReceivePackage(ADeliveryPackageItem *Package, bool bPlayFeedback = true);
};
