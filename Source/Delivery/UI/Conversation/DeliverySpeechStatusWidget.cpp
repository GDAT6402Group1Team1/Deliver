#include "UI/Conversation/DeliverySpeechStatusWidget.h"

#include "Animation/WidgetAnimation.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "Components/Button.h"
#include "Components/WidgetSwitcher.h"
#include "GameFramework/PlayerController.h"
#include "UI/HUD/DeliveryHUD.h"
#include "Speech/DeliverySpeechInputComponent.h"

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

void UDeliverySpeechStatusWidget::SetSpeechInput(UDeliverySpeechInputComponent* Value)
{
    if (auto* Previous = SpeechInput.Get())
        Previous->OnDisplayChanged.RemoveDynamic(this, &UDeliverySpeechStatusWidget::RefreshStatus);
    UnbindConversation();
    // 只绑定创建此界面的玩家，不能读取另一个 PIE 世界或其他玩家的录音
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

    // 界面只显示图标和按键，悬停仍能查看连麦失败或繁忙的原因
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

void UDeliverySpeechStatusWidget::HandleVoiceInputError(FName)
{
    RefreshStatus();
}

void UDeliverySpeechStatusWidget::UnbindConversation()
{
    if (auto* OwnerConversation = Conversation.Get())
        OwnerConversation->OnVoiceInputError.Remove(ConversationErrorHandle);
    Conversation.Reset();
    ConversationErrorHandle.Reset();
}

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

void UDeliverySpeechStatusWidget::SetPulseEnabled(bool bEnabled)
{
    if (bEnabled == bPulsePlaying) return;
    bPulsePlaying = bEnabled && VoiceStatusPulse != nullptr;
    if (!VoiceStatusPulse) return;
    if (bPulsePlaying) PlayAnimation(VoiceStatusPulse, 0.f, 0, EUMGSequencePlayMode::Forward);
    else StopAnimation(VoiceStatusPulse);
}

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
