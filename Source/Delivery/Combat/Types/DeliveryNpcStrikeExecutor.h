#pragma once
#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "DeliveryNpcStrikeExecutor.generated.h"

/**
 * @brief NPC 挥一拳的请求，服务器为每一拳新建一个，作为 Event.Npc.Strike 事件的 OptionalObject 交给 GA_NpcStrike
 *
 * 技能只读这个对象里的目标和编号，不读客户端传来的目标，也不读技能默认对象上的值
 */
UCLASS()
class DELIVERY_API UDeliveryNpcStrikeRequest : public UObject
{
    GENERATED_BODY()
public:
    /// 打谁
    UPROPERTY() TObjectPtr<AActor> Target;
    /// 这一拳所属的动作编号
    FGuid ActionId;
    /// 这一拳的编号
    FGuid PunchId;
    /// 是交战 Fight 动作里的一拳（轻拳），不是单独的 StrikeOnce
    bool bFightStrike=false;
};

/// 一拳结束的通知，参数依次为动作编号、这一拳的编号、是否被取消
DECLARE_MULTICAST_DELEGATE_ThreeParams(FDeliveryNpcStrikeFinished,FGuid,FGuid,bool);

UINTERFACE(MinimalAPI)
class UDeliveryNpcStrikeExecutor : public UInterface
{
    GENERATED_BODY()
};

/**
 * @brief NPC 挥拳的执行接口，ADeliveryNpcCharacter 实现，GA_NpcStrike 调用
 *
 * 技能管一拳从开始到结束；拳头碰没碰到、结果保存和同步给客户端由实现方（NPC 的战斗组件）负责，技能不依赖具体的 NPC 类
 */
class DELIVERY_API IDeliveryNpcStrikeExecutor
{
    GENERATED_BODY()
public:
    /// @brief 开始挥这一拳；返回 true 只表示开始了，不表示打中
    virtual bool StartNpcStrike(const UDeliveryNpcStrikeRequest& Request)=0;
    /// @brief 查编号为 PunchId 的那一拳：已经结束时返回 true，打没打中写进 bOutHit；还没结束或编号不对时返回 false
    virtual bool GetNpcStrikeResult(FGuid PunchId,bool& bOutHit) const=0;
    /// @brief 取消一拳；动作编号和拳头编号都对得上才取消，旧技能结束时不会误取消后面的一拳
    virtual void CancelNpcStrike(FGuid ActionId,FGuid PunchId)=0;
    /// @brief 一拳结束的通知；广播时结果已经存好，收到后可以马上调用 GetNpcStrikeResult
    virtual FDeliveryNpcStrikeFinished& GetNpcStrikeFinishedDelegate()=0;
};
