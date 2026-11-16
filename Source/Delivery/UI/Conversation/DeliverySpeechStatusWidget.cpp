#include "UI/Conversation/DeliverySpeechStatusWidget.h"

#include "Animation/WidgetAnimation.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "Components/Button.h"
#include "Components/WidgetSwitcher.h"
#include "GameFramework/PlayerController.h"
#include "UI/HUD/DeliveryHUD.h"
#include "Speech/DeliverySpeechInputComponent.h"

// ── 生命周期 ──
// NativeConstruct 绑定按钮并调用 SetSpeechInput；NativeDestruct 取消所有订阅

/// @brief 控件建好：绑定 VoiceDisableButton 的点击；没指定语音输入组件时从拥有者玩家控制器上找，然后调用 SetSpeechInput
void UDeliverySpeechStatusWidget::NativeConstruct()
{
    Super::NativeConstruct();
    bConstructed = true;
    SetIsFocusable(false);
    if (VoiceDisableButton)
    {
        VoiceDisableButton->OnClicked.AddUniqueDynamic(this, &UDeliverySpeechStatusWidget::DisableVoice);
    }
    auto* Input = SpeechInput.Get();
    if (!Input && GetOwningPlayer())
        Input = GetOwningPlayer()->FindComponentByClass<UDeliverySpeechInputComponent>();
    SetSpeechInput(Input);
}

// ── 绑定 ──

/// @brief 换成显示 Value：先取消旧组件的订阅，再订阅新组件的 OnDisplayChanged 和交谈组件的 OnVoiceInputError，最后调用 RefreshStatus
void UDeliverySpeechStatusWidget::SetSpeechInput(UDeliverySpeechInputComponent* Value)
{
    if (auto* Previous = SpeechInput.Get())
        Previous->OnDisplayChanged.RemoveDynamic(this, &UDeliverySpeechStatusWidget::RefreshStatus);
    UnbindConversation();

    // Value 必须属于拥有这个控件的本机玩家控制器、而且在同一个世界里，否则当作空；其他 PIE 窗口和其他玩家的录音不显示
    const auto* Player = GetOwningPlayer();
    SpeechInput = IsValid(Player) && Player->IsLocalController() && IsValid(Value) &&
        Value->GetWorld() == GetWorld() && Value->GetOwner() == Player
        ? Value : nullptr;
    if (!bConstructed) return;
    if (auto* Input = SpeechInput.Get())
    {
        Input->OnDisplayChanged.AddUniqueDynamic(this, &UDeliverySpeechStatusWidget::RefreshStatus);
        auto* OwnerConversation = Input->GetOwner()->FindComponentByClass<UDeliveryConversationComponent>();
        if (OwnerConversation)
        {
            Conversation = OwnerConversation;
            ConversationErrorHandle = OwnerConversation->OnVoiceInputError.AddUObject(
                this, &UDeliverySpeechStatusWidget::HandleVoiceInputError);
        }
    }
    RefreshStatus();
}

// ── 刷新 ──

/// @brief 没有语音输入组件时隐藏；有就显示，State 为 Capturing 时播放呼吸动画，图标按连麦是否开着切换
void UDeliverySpeechStatusWidget::RefreshStatus()
{
    const auto* Input = SpeechInput.Get();
    const auto State = Input ? Input->GetState() : EDeliverySpeechInputState::Off;
    const bool bVisible = Input != nullptr;
    SetVisibility(bVisible ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
    SetPulseEnabled(State == EDeliverySpeechInputState::Capturing);
    if (!bVisible) return;

    const bool bVoiceEnabled = Input->IsVoiceEnabled();
    if (VoiceIcons) VoiceIcons->SetActiveWidgetIndex(bVoiceEnabled ? 1 : 0);
    if (!VoiceDisableButton) return;

    // 按钮悬停提示：平时写开或关连麦；正在准备或出错时写语音输入组件的状态文字；
    // 准备好了但交谈组件报过错时，按错误原因 Busy、Unavailable 或其他写对应的提示
    FText Tooltip = bVoiceEnabled
        ? NSLOCTEXT("DeliverySpeech", "DisableVoiceTooltip", "关闭连麦和麦克风")
        : NSLOCTEXT("DeliverySpeech", "EnableVoiceTooltip", "与二狗连麦");
    if (State == EDeliverySpeechInputState::Preparing || State == EDeliverySpeechInputState::Error)
    {
        const FText Status=Input->GetStatusText();
        if (!Status.IsEmpty()) Tooltip = Status;
    }
    if (State == EDeliverySpeechInputState::Ready)
    {
        const auto* OwnerConversation = Conversation.Get();
        const FName Reason = OwnerConversation ? OwnerConversation->GetVoiceInputError() : NAME_None;
        if (Reason == TEXT("Busy"))
            Tooltip = NSLOCTEXT("DeliverySpeech", "VoiceBusyTooltip", "二狗正忙，稍后再说");
        else if (Reason == TEXT("Unavailable"))
            Tooltip = NSLOCTEXT("DeliverySpeech", "VoiceUnavailableTooltip", "暂时无法与二狗连麦");
        else if (!Reason.IsNone())
            Tooltip = NSLOCTEXT("DeliverySpeech", "VoiceUnclearTooltip", "刚才没听清，可以再说一次");
    }
    VoiceDisableButton->SetToolTipText(Tooltip);
}

/// @brief 交谈组件的 OnVoiceInputError 回调，调用 RefreshStatus
void UDeliverySpeechStatusWidget::HandleVoiceInputError(FName)
{
    RefreshStatus();
}

/// @brief 取消订阅交谈组件的 OnVoiceInputError 并清空记录
void UDeliverySpeechStatusWidget::UnbindConversation()
{
    if (auto* OwnerConversation = Conversation.Get())
        OwnerConversation->OnVoiceInputError.Remove(ConversationErrorHandle);
    Conversation.Reset();
    ConversationErrorHandle.Reset();
}

/// @brief 按钮点击：连麦开着时调用 DisableVoice 关掉；关着时调用 EnableVoice，打开成功后调用 HUD 的 CloseResidentConversation 关闭交谈面板
void UDeliverySpeechStatusWidget::DisableVoice()
{
    if (auto* Input = SpeechInput.Get())
    {
        if (Input->IsVoiceEnabled()) Input->DisableVoice();
        else if (Input->EnableVoice())
            if (auto* PC=GetOwningPlayer())
                if (auto* HUD=Cast<ADeliveryHUD>(PC->GetHUD())) HUD->CloseResidentConversation();
    }
}

/// @brief bEnabled 为 true 时循环播放 VoiceStatusPulse，false 时停止；和当前状态一样时直接返回
void UDeliverySpeechStatusWidget::SetPulseEnabled(bool bEnabled)
{
    if (bEnabled == bPulsePlaying) return;
    bPulsePlaying = bEnabled && VoiceStatusPulse != nullptr;
    if (!VoiceStatusPulse) return;
    if (bPulsePlaying) PlayAnimation(VoiceStatusPulse, 0.f, 0, EUMGSequencePlayMode::Forward);
    else StopAnimation(VoiceStatusPulse);
}

// ── 生命周期 ──

/// @brief 控件销毁：取消语音输入组件、交谈组件和按钮的订阅，停止呼吸动画
void UDeliverySpeechStatusWidget::NativeDestruct()
{
    bConstructed = false;
    if (auto* Input = SpeechInput.Get())
        Input->OnDisplayChanged.RemoveDynamic(this, &UDeliverySpeechStatusWidget::RefreshStatus);
    SpeechInput.Reset();
    UnbindConversation();
    if (VoiceDisableButton)
        VoiceDisableButton->OnClicked.RemoveDynamic(this, &UDeliverySpeechStatusWidget::DisableVoice);
    SetPulseEnabled(false);
    Super::NativeDestruct();
}
