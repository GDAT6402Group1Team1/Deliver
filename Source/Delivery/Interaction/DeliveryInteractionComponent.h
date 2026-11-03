#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryInteractionComponent.generated.h"

UENUM()
enum class EDeliveryInteractionRequest : uint8
{
    Interact,
    Use,
    BeginHold,
    CompleteHold,
    CancelHold
};

/** 管理一次交互的目标、长按和服务器检查，具体行为由目标提供 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryInteractionComponent : public UActorComponent
{
    GENERATED_BODY()
  public:
    // 本机操作与查询
    /** 请求执行当前选中的通用交互 */
    void Interact();
    /** 根据目标配置立即执行或开始长按 */
    void BeginUse();
    /** 清除本机进度，并按需通知服务器取消 */
    void CancelUse(bool bNotifyServer = true);
    /** 返回长按进度，没有操作该目标时返回 -1 */
    float GetProgress(const AActor *Target) const;
    /** 同时检查角色状态、距离和目标自身的条件 */
    bool CanUse(const AActor *Target, bool bCheckTaskAvailability = true) const;

    // 服务器入口，先检查权限，再执行目标行为
    void HandleRequest(EDeliveryInteractionRequest Request, AActor *Target);
    void ExecuteInteract(AActor *Target);
    void ExecuteUse(AActor *Target);
    void BeginServerHold(AActor *Target);
    void CompleteServerHold(AActor *Target);
    void CancelServerHold(AActor *Target);
    bool ValidateTarget(AActor *Target) const;

  protected:
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

  private:
    // 长按期间检查目标是否仍然可用
    void UpdateHold();
    void SendRequest(EDeliveryInteractionRequest Request, AActor *Target);
    // 本机长按状态
    bool bUseHeld = false;
    /** 本机长按开始时间，仅用于进度显示和发送完成请求 */
    float UseHoldStartedAt = 0.0f;
    /** 目标要求的长按时长，单位为秒 */
    float UseHoldDuration = 0.0f;
    TWeakObjectPtr<AActor> UseHoldTarget;
    FTimerHandle UseHoldTimer;
    // 服务器长按记录
    TWeakObjectPtr<AActor> ServerUseHoldTarget;
    /** 服务器独立记录的长按开始时间，不使用客户端上报的耗时 */
    float ServerUseHoldStartedAt = -1000.0f;
};
