#pragma once
#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "DeliveryHandheldUseExecutor.generated.h"
class ADeliveryHandheldItem;

// 服务器为当次短交接创建，模型与客户端不能提供此对象
UCLASS()
class DELIVERY_API UDeliveryHandheldUseRequest : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(Transient) FGuid UseId;
    UPROPERTY(Transient) int32 CommandRevision=0;
    UPROPERTY(Transient) TWeakObjectPtr<ADeliveryHandheldItem> Item;
    UPROPERTY(Transient) bool bReturnToOwner=false;
};

UINTERFACE(MinimalAPI,meta=(CannotImplementInterfaceInBlueprint))
class UDeliveryHandheldUseExecutor : public UInterface
{
    GENERATED_BODY()
};

// GAS 调用短操作，安排与真实持物仍由既有组件管理
class DELIVERY_API IDeliveryHandheldUseExecutor
{
    GENERATED_BODY()
public:
    virtual bool ExecuteHandheldUse(const UDeliveryHandheldUseRequest& Request,FName& Reason)=0;
    virtual void FinishHandheldUse(FGuid UseId,bool bSucceeded,FName Reason)=0;
};
