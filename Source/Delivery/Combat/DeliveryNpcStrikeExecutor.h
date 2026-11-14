#pragma once
#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "DeliveryNpcStrikeExecutor.generated.h"

// 服务器为一次出拳创建，技能只读取这次请求，不从客户端目标或共享技能默认对象取值
UCLASS()
class DELIVERY_API UDeliveryNpcStrikeRequest : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY() TObjectPtr<AActor> Target;
    FGuid ActionId;
    FGuid PunchId;
    bool bFightStrike=false;
};

DECLARE_MULTICAST_DELEGATE_ThreeParams(FDeliveryNpcStrikeFinished,FGuid,FGuid,bool);

UINTERFACE(MinimalAPI)
class UDeliveryNpcStrikeExecutor : public UInterface
{
    GENERATED_BODY()
};

// GAS 管理单拳生命周期，执行者保留骨骼接触、结果与复制，技能不依赖具体 NPC 类
class DELIVERY_API IDeliveryNpcStrikeExecutor
{
    GENERATED_BODY()
public:
    // true 表示这一拳已开始，不表示命中或整轮交战完成
    virtual bool StartNpcStrike(const UDeliveryNpcStrikeRequest& Request)=0;
    virtual bool GetNpcStrikeResult(FGuid PunchId,bool& bOutHit) const=0;
    // 两个编号都匹配才取消，旧技能的清理不能结束后续拳头
    virtual void CancelNpcStrike(FGuid ActionId,FGuid PunchId)=0;
    // 参数依次为动作编号、拳头编号、是否取消，结果已保存后才通知
    virtual FDeliveryNpcStrikeFinished& GetNpcStrikeFinishedDelegate()=0;
};
