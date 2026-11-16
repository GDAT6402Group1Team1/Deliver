#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryPhoneController.generated.h"

class ADeliveryHUD;
class ADeliveryPlayerController;
class UDeliveryPhoneCallQueueComponent;
class UDeliveryDialogueVoiceComponent;

/**
 * @brief 手机的界面处理器，HUD 在本机创建
 *
 * 手机键和手机面板按钮调用 Toggle、Answer、HangUp；面板开着没开登记在 HUD 的 DeliveryPhoneUI 面板里
 * 订阅电话队列的状态变化：接通时用电子语音念出这通电话的对白，每念出一个字就在屏幕下方字幕里加上它；挂断时停下并清掉字幕
 */
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryPhoneController : public UObject
{
    GENERATED_BODY()
public:
    /// @brief 绑定本机玩家和 HUD，订阅电子语音的逐字通知，再调用 BindPhone
    void Initialize(ADeliveryPlayerController* InPlayer, ADeliveryHUD* InHUD);

    /// @brief 取消订阅、停止说话、清掉字幕；HUD 销毁时调用
    void Shutdown();

    /// @brief 玩家所在的世界
    virtual UWorld* GetWorld() const override;

    // ── 手机面板 ──

    /// @brief 手机键：播放开关音效，打开或收起面板，再调用蓝图事件 TogglePhoneUI
    UFUNCTION(BlueprintCallable, Category="Phone")
    void Toggle();

    /// @brief 手机面板是否开着
    UFUNCTION(BlueprintPure, Category="Phone")
    bool IsOpen() const;

    /// @brief 登记手机面板打开或收起
    UFUNCTION(BlueprintCallable, Category="Phone")
    void SetOpen(bool bOpen);

    /// @brief 接听，转给玩家控制器的 RequestAnswerCall
    UFUNCTION(BlueprintCallable, Category="Phone")
    void Answer();

    /// @brief 挂断，转给玩家控制器的 RequestHangUpCall
    UFUNCTION(BlueprintCallable, Category="Phone")
    void HangUp();

    /// 手机面板在 HUD 里登记的名字 DeliveryPhoneUI
    static const FName PanelId;
private:
    friend class FDeliveryPhoneControllerTest;

    /// @brief 找到 GameState 上的电话队列并订阅它的状态变化；客户端可能还没收到 GameState，找不到时 0.5 秒后再试
    void BindPhone();

    /// @brief 电话状态变化：接通一通新电话时清掉旧字幕、记下来电人，用电子语音念对白；不在通话中时停下并清掉字幕
    /// 同一通电话可能多次收到通知，ActiveCallId 一样时不重复念
    UFUNCTION()
    void HandleCallChanged(EDeliveryPhoneCallState State, const FDeliveryPhoneCall& Call);

    /// @brief 电子语音念出一个字时调用，加到字幕后面显示
    UFUNCTION()
    void HandleGlyph(const FString& Glyph, int32 GlyphIndex);

    /// @brief 清掉来电人和字幕
    void ClearSubtitle();

    /// 本机玩家
    TWeakObjectPtr<ADeliveryPlayerController> Player;

    /// 本机 HUD
    TWeakObjectPtr<ADeliveryHUD> HUD;

    /// GameState 上的电话队列
    TWeakObjectPtr<UDeliveryPhoneCallQueueComponent> Phone;

    /// 玩家控制器上的电子语音组件
    TWeakObjectPtr<UDeliveryDialogueVoiceComponent> Voice;

    /// 找不到电话队列时 0.5 秒后重试
    FTimerHandle BindTimer;

    /// 正在念的那通电话的编号；同一通电话可能多次收到通知，只念一次
    int32 ActiveCallId = 0;

    /// 来电人名字，显示在字幕前面
    FText CallerName;

    /// 已经念出的字
    FString Subtitle;
};
