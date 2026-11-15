#pragma once
#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "DeliveryHandheldCarrierInterface.generated.h"
class ADeliveryHandheldItem;
class APlayerState;
class UDeliveryInteractableComponent;

UINTERFACE(MinimalAPI,meta=(CannotImplementInterfaceInBlueprint))
class UDeliveryHandheldCarrierInterface : public UInterface
{
    GENERATED_BODY()
};

// 玩家沿用原背包，同伴沿用唯一持物槽，接口不保存第二份库存
class DELIVERY_API IDeliveryHandheldCarrierInterface
{
    GENERATED_BODY()
public:
    virtual ADeliveryHandheldItem* GetCarriedHandheldItem() const = 0;
    virtual APlayerState* GetHandheldOwnerPlayerState() const = 0;
    virtual bool CanHandleHandheldItem() const = 0;
    virtual bool OwnsHandheldItem(const ADeliveryHandheldItem* Item) const = 0;
    virtual bool CanReceiveHandheldItem(ADeliveryHandheldItem* Item) const = 0;
    virtual bool TryReceiveHandheldItem(ADeliveryHandheldItem* Item) = 0;
    virtual bool ReleaseHandheldItem(ADeliveryHandheldItem* Item) = 0;
    virtual void ApplyHandheldItemPresentation(ADeliveryHandheldItem* Item) = 0;
};

namespace DeliveryHandheldTransfer
{
// 三种任务身份任一存在都拒绝，物品明确标注后才属于普通取回范围
DELIVERY_API bool IsOrdinaryItem(const ADeliveryHandheldItem* Item);
DELIVERY_API bool IsFetchableItem(const ADeliveryHandheldItem* Item);
// 查世界中真实可拾取的根网格和 E 入口，发起者权限、距离与遮挡由调用方继续复核
DELIVERY_API const UDeliveryInteractableComponent* FindWorldPickupInteraction(const ADeliveryHandheldItem* Item,const UWorld* World);
// 接近后的真实服务器操作，接受计划或走到附近均不代表拿取成功
// CanContinue 只查询本次安排，交接回调改变安排时回滚同一实物
DELIVERY_API bool TryPickupWorldItem(AActor* Receiver,ADeliveryHandheldItem* Item,FName& Reason,
    const TFunction<bool()>& CanContinue = TFunction<bool()>());
DELIVERY_API bool TryTransfer(AActor* From,AActor* To,ADeliveryHandheldItem* Item,FName& Reason,
    const TFunction<bool()>& CanContinue = TFunction<bool()>());
}
