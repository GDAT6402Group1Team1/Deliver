#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DeliverySpeechStatusWidget.generated.h"

class UDeliverySpeechInputComponent;
class UDeliveryConversationComponent;
class UButton;
class UWidgetAnimation;
class UWidgetSwitcher;

/**
 * @brief 本机语音状态图标 WBP 的父类，HUD 创建它
 *
 * 显示麦克风是否开着、是否正在收音（收音时播放呼吸动画 VoiceStatusPulse），
 * 点按钮 VoiceDisableButton 开关和二狗的连麦；按钮的悬停提示写出连麦失败或繁忙的原因
 * 控件树和动画存在蓝图里，这里只负责读 UDeliverySpeechInputComponent 的状态并刷新
 */
UCLASS()
class DELIVERY_API UDeliverySpeechStatusWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    // ── 绑定 ──

    /// @brief 指定要显示的语音输入组件；只接受本机玩家自己的组件，其他的当作空
    /// 同时订阅它的 OnDisplayChanged 和对应交谈组件的 OnVoiceInputError，变化时调用 RefreshStatus
    UFUNCTION(BlueprintCallable, Category="Delivery|Speech")
    void SetSpeechInput(UDeliverySpeechInputComponent* Value);

protected:
    // ── 生命周期 ──
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    // ── 控件 ──

    /// 两个图标：第 0 个是连麦关着，第 1 个是连麦开着
    UPROPERTY(meta=(BindWidgetOptional))
    TObjectPtr<UWidgetSwitcher> VoiceIcons;

    /// 开关连麦的按钮，悬停提示显示当前状态或出错原因
    UPROPERTY(meta=(BindWidgetOptional))
    TObjectPtr<UButton> VoiceDisableButton;

    /// 正在收音时循环播放的呼吸动画
    UPROPERTY(Transient, meta=(BindWidgetAnimOptional))
    TObjectPtr<UWidgetAnimation> VoiceStatusPulse;

private:
    // ── 刷新 ──

    /// @brief 按语音输入组件的状态刷新：显示或隐藏、图标、呼吸动画、按钮悬停提示
    UFUNCTION()
    void RefreshStatus();

    /// @brief 按钮点击：连麦开着时关掉；关着时打开，打开成功后关闭交谈面板
    UFUNCTION()
    void DisableVoice();

    /// @brief 开始或停止播放呼吸动画；和当前状态一样时直接返回
    void SetPulseEnabled(bool bEnabled);

    /// @brief 交谈组件报告连麦出错时调用 RefreshStatus，更新悬停提示
    void HandleVoiceInputError(FName Reason);

    /// @brief 取消订阅交谈组件的 OnVoiceInputError
    void UnbindConversation();

    // ── 状态 ──

    /// 正在显示的语音输入组件
    TWeakObjectPtr<UDeliverySpeechInputComponent> SpeechInput;

    /// SpeechInput 所在玩家控制器上的交谈组件，读它的连麦错误原因
    TWeakObjectPtr<UDeliveryConversationComponent> Conversation;
    FDelegateHandle ConversationErrorHandle;

    /// NativeConstruct 之后为 true；之前调用 SetSpeechInput 只记下组件，不订阅也不刷新
    bool bConstructed = false;

    /// 呼吸动画是否正在播放
    bool bPulsePlaying = false;
    friend class FDeliveryConversationVoiceKeyTest;
};
