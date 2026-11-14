#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "DeliveryObjectCarryProvider.generated.h"

class AActor;

enum class EDeliveryObjectCarryReleaseReason : uint8
{
    Cancelled,
    ProviderUnavailable,
    InvalidPose,
    ObjectUnavailable,
    ReleasedAll,
    ObjectEndPlay
};

UINTERFACE(MinimalAPI, meta=(CannotImplementInterfaceInBlueprint))
class UDeliveryObjectCarryProvider : public UInterface
{
    GENERATED_BODY()
};

// 动画 NPC 只提供当前托举目标，实物由 Grabbable 在服务器上统一扫掠移动
class DELIVERY_API IDeliveryObjectCarryProvider
{
    GENERATED_BODY()
public:
    // 查询必须无副作用，取消、受击、上车、失去主人或物体不匹配时返回 false
    // Location 是实物根组件的期望位置，Forward 提供水平朝向，不允许修改实物
    virtual bool GetObjectCarryPose(const AActor* Object, const FGuid& CarryId,
        FVector& OutLocation, FVector& OutForward) const = 0;

    // Grabbable 清掉本次引用并恢复实物后通知，接收方须核对 CarryId，不能取消新的搬运
    // Provider 自身的 EndPlay、受击和命令取消应显式注销，失效查询仍由 Grabbable 兜底
    virtual void OnObjectCarryReleased(AActor* Object, const FGuid& CarryId,
        EDeliveryObjectCarryReleaseReason Reason) = 0;
};
