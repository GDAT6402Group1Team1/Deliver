#include "UI/DeliverySpeechStatusWidget.h"

#include "Animation/WidgetAnimation.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "GameFramework/PlayerController.h"
#include "UI/DeliveryHUD.h"
#include "Speech/DeliverySpeechInputComponent.h"

void UDeliverySpeechStatusWidget::NativeConstruct()
{
    Super::NativeConstruct();
    bConstructed = true;
    SetIsFocusable(false);
    if (VoiceDisableButton)
    {
        VoiceDisableButton->OnClicked.AddUniqueDynamic(this, &UDeliverySpeechStatusWidget::DisableVoice);
        VoiceDisableButton->SetToolTipText(FText::FromString(TEXT("关闭连麦和麦克风 · V")));
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
    if (!bVisible)
    {
        if (VoicePreview) VoicePreview->SetText(FText::GetEmpty());
        return;
    }

    const auto* Npc=Input?Input->GetConnectedNpc():nullptr;
    const FString Name=IsValid(Npc)?Npc->DisplayName.ToString():TEXT("二狗");
    FString Label;
    switch (State)
    {
    case EDeliverySpeechInputState::Preparing: Label = FString::Printf(TEXT("正在连接%s"),*Name); break;
    case EDeliverySpeechInputState::Ready:
    case EDeliverySpeechInputState::Capturing:
    case EDeliverySpeechInputState::Finalizing: Label = Name + TEXT(" · 连麦中"); break;
    case EDeliverySpeechInputState::Error: Label = Input->GetStatusText().ToString(); break;
    case EDeliverySpeechInputState::Off:
        Label=Input->GetStatusText().ToString();
        if (Label.IsEmpty() || Label==TEXT("语音已关闭")) Label=TEXT("V 与二狗连麦");
        break;
    default: break;
    }
    if (State==EDeliverySpeechInputState::Ready)
    {
        const auto* OwnerConversation=Conversation.Get();
        const FName Reason=OwnerConversation?OwnerConversation->GetVoiceInputError():NAME_None;
        if (Reason==TEXT("Busy")) Label=Name+TEXT("正忙，稍后再说");
        else if (Reason==TEXT("Unavailable")) Label=TEXT("暂时无法与")+Name+TEXT("连麦");
        else if (!Reason.IsNone()) Label=TEXT("刚才没听清，可以再说一次");
    }
    if (VoiceLabel) VoiceLabel->SetText(FText::FromString(Label));
    if (VoiceDisableLabel) VoiceDisableLabel->SetText(FText::FromString(Input && Input->IsVoiceEnabled()?TEXT("关闭"):TEXT("连麦")));
    if (VoiceDisableButton) VoiceDisableButton->SetToolTipText(FText::FromString(
        Input && Input->IsVoiceEnabled()?TEXT("关闭连麦和麦克风 · V"):TEXT("与自己的二狗连麦 · V")));
    // 连麦只显示对象与麦克风状态，识别草稿不作为一段输入流程展示
    if (VoicePreview) { VoicePreview->SetText(FText::GetEmpty()); VoicePreview->SetVisibility(ESlateVisibility::Collapsed); }
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
