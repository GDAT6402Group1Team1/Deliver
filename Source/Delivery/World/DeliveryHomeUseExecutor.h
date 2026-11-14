#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "DeliveryHomeUseExecutor.generated.h"

// 行为树确认抵达后由服务器创建，技能只能执行组件已保存的这次家居安排
UCLASS()
class DELIVERY_API UDeliveryHomeUseRequest : public UObject
{
    GENERATED_BODY()
public:
    FGuid ArrangementId;
};

DECLARE_MULTICAST_DELEGATE_ThreeParams(FDeliveryHomeUseFinished,FGuid,bool,FName);

UINTERFACE(MinimalAPI)
class UDeliveryHomeUseExecutor : public UInterface
{
    GENERATED_BODY()
};

// GAS 管理使用生命周期，玩法组件保留已核验的物件、占位和实际执行结果
class DELIVERY_API IDeliveryHomeUseExecutor
{
    GENERATED_BODY()
public:
    // true 只表示开始使用，坐到位、开关和摆放仍由后续实际结果确认
    virtual bool StartHomeUse(const UDeliveryHomeUseRequest& Request)=0;
    virtual void CancelHomeUse(FGuid ArrangementId,FName Reason)=0;
    virtual FDeliveryHomeUseFinished& GetHomeUseFinishedDelegate()=0;
};
