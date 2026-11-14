#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "DeliveryPackageCarrierInterface.generated.h"

class ADeliveryPackageItem;
class APlayerState;

UINTERFACE(MinimalAPI, meta=(CannotImplementInterfaceInBlueprint))
class UDeliveryPackageCarrierInterface : public UInterface
{
    GENERATED_BODY()
};

// 玩家与同伴共享持件契约，任务和奖励仍由现有任务对象确认
class DELIVERY_API IDeliveryPackageCarrierInterface
{
    GENERATED_BODY()
public:
    virtual ADeliveryPackageItem* GetCarriedTaskPackage() const = 0;
    virtual APlayerState* GetTaskPlayerState() const = 0;
    virtual bool CanHandleTaskPackage() const = 0;
    // 默认使用角色位置，车辆改用实际后备箱交接点
    virtual FVector GetTaskPackageTransferPoint() const;
    // 查询接收条件，不会释放原持有者或挤掉接收者的旧手持物
    virtual bool CanReceiveTaskPackage(ADeliveryPackageItem* Package) const = 0;
    virtual bool TryReceiveTaskPackage(ADeliveryPackageItem* Package) = 0;
    // 仅清除指定实物的持有关系，完成任务后也必须能够释放
    virtual bool ReleaseTaskPackage(ADeliveryPackageItem* Package) = 0;
    virtual void ApplyTaskPackagePresentation(ADeliveryPackageItem* Package) = 0;
};

namespace DeliveryPackageTransfer
{
// 服务器同步交接同一实物，成功要求持件指针与 Owner 均已转移
// 失败尽量归还原持有者；回滚失败保留实物并返回明确原因
DELIVERY_API bool TryTransfer(AActor* From, AActor* To, FName& Reason,
    ADeliveryPackageItem* ExpectedPackage = nullptr, TFunction<bool()> CanContinue = {});
}
