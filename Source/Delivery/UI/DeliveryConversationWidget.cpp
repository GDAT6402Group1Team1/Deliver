#include "UI/DeliveryConversationWidget.h"
#include "UI/DeliveryHUD.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/EditableTextBox.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/ScrollBox.h"
#include "Components/Border.h"
#include "Components/SizeBox.h"
#include "Animation/WidgetAnimation.h"
#include "Speech/DeliverySpeechInputComponent.h"
#include "GameFramework/PlayerController.h"
#include "Input/Reply.h"
#include "InputCoreTypes.h"

void UDeliveryConversationWidget::NativeConstruct()
{
    Super::NativeConstruct();
    if (auto* Previous=BoundConversation.Get())
    {
        Previous->OnSubmitted.RemoveAll(this);
        Previous->OnSubmissionBound.RemoveAll(this);
        Previous->OnVoiceInputError.RemoveAll(this);
        if (Previous->GetOwner()!=GetOwningPlayer()) Previous->ClearConversationTarget(Resident.Get());
    }
    if (auto* PreviousSpeech=BoundSpeech.Get())
        PreviousSpeech->OnDisplayChanged.RemoveDynamic(this,&UDeliveryConversationWidget::RefreshVoice);
    BoundConversation.Reset();
    BoundSpeech.Reset();
    SetIsFocusable(true);
    if (SendButton) SendButton->OnClicked.AddUniqueDynamic(this,&UDeliveryConversationWidget::Send);
    if (SuggestionToggle) SuggestionToggle->OnClicked.AddUniqueDynamic(this,&UDeliveryConversationWidget::ToggleSuggestions);
    if (Input) Input->OnTextCommitted.AddUniqueDynamic(this,&UDeliveryConversationWidget::CommitInput);
    if (CloseButton) CloseButton->OnClicked.AddUniqueDynamic(this,&UDeliveryConversationWidget::Close);
    if (RetryButton) RetryButton->OnClicked.AddUniqueDynamic(this,&UDeliveryConversationWidget::Retry);
    if (VoiceToggleButton) VoiceToggleButton->OnClicked.AddUniqueDynamic(this,&UDeliveryConversationWidget::ToggleVoice);
    if (VoiceHoldButton)
    {
        VoiceHoldButton->OnPressed.RemoveDynamic(this,&UDeliveryConversationWidget::BeginVoice);
        VoiceHoldButton->OnReleased.RemoveDynamic(this,&UDeliveryConversationWidget::FinishVoice);
        VoiceHoldButton->SetVisibility(ESlateVisibility::Collapsed);
    }
    if (auto* PC=GetOwningPlayer())
        if (auto* Conversation=PC->FindComponentByClass<UDeliveryConversationComponent>())
        {
            BoundConversation=Conversation;
            Conversation->OnSubmitted.RemoveAll(this);
            Conversation->OnSubmissionBound.RemoveAll(this);
            Conversation->OnVoiceInputError.RemoveAll(this);
            Conversation->OnSubmitted.AddUObject(this,&UDeliveryConversationWidget::HandleSubmitted);
            Conversation->OnSubmissionBound.AddUObject(this,&UDeliveryConversationWidget::HandleSubmissionBound);
            Conversation->OnVoiceInputError.AddUObject(this,&UDeliveryConversationWidget::HandleVoiceError);
            BoundSpeech=Conversation->GetSpeechInput();
            if (auto* Speech=BoundSpeech.Get())
                Speech->OnDisplayChanged.AddUniqueDynamic(this,&UDeliveryConversationWidget::RefreshVoice);
        }
    RefreshVoice();
    RefreshReply();
}
void UDeliveryConversationWidget::NativeDestruct()
{
    if (SendButton) SendButton->OnClicked.RemoveDynamic(this,&UDeliveryConversationWidget::Send);
    if (SuggestionToggle) SuggestionToggle->OnClicked.RemoveDynamic(this,&UDeliveryConversationWidget::ToggleSuggestions);
    if (Input) Input->OnTextCommitted.RemoveDynamic(this,&UDeliveryConversationWidget::CommitInput);
    if (CloseButton) CloseButton->OnClicked.RemoveDynamic(this,&UDeliveryConversationWidget::Close);
    if (RetryButton) RetryButton->OnClicked.RemoveDynamic(this,&UDeliveryConversationWidget::Retry);
    if (VoiceToggleButton) VoiceToggleButton->OnClicked.RemoveDynamic(this,&UDeliveryConversationWidget::ToggleVoice);
    if (VoiceHoldButton)
    {
        VoiceHoldButton->OnPressed.RemoveDynamic(this,&UDeliveryConversationWidget::BeginVoice);
        VoiceHoldButton->OnReleased.RemoveDynamic(this,&UDeliveryConversationWidget::FinishVoice);
    }
    if (auto* Speech=BoundSpeech.Get()) Speech->OnDisplayChanged.RemoveDynamic(this,&UDeliveryConversationWidget::RefreshVoice);
    if (auto* Conversation=BoundConversation.Get())
    {
        Conversation->ClearConversationTarget(Resident.Get());
        Conversation->OnSubmitted.RemoveAll(this);
        Conversation->OnSubmissionBound.RemoveAll(this);
        Conversation->OnVoiceInputError.RemoveAll(this);
    }
    BoundSpeech.Reset();
    BoundConversation.Reset();
    ResetConversation();
    Resident.Reset();
    Super::NativeDestruct();
}
FReply UDeliveryConversationWidget::NativeOnPreviewKeyDown(const FGeometry& Geometry,const FKeyEvent& KeyEvent)
{
    if (KeyEvent.GetKey()==EKeys::Escape)
    {
        Close();
        return FReply::Handled();
    }
    return Super::NativeOnPreviewKeyDown(Geometry,KeyEvent);
}
void UDeliveryConversationWidget::SendText(const FString& Text)
{
    auto* PC=GetOwningPlayer();
    auto* Conversation=PC?PC->FindComponentByClass<UDeliveryConversationComponent>():nullptr;
    if (!Resident.IsValid() || !Conversation) return;
    RefreshReply();
    const FString Message=Text.TrimStartAndEnd();
    if (Message.IsEmpty()) return;
    if (Message.Len()>256)
    {
        InputNotice=TEXT("这句话有点长，请缩短到 256 字以内");
        InputNoticeUntil=FPlatformTime::Seconds()+3;
        RefreshReply();
        return;
    }
    // 正在等待同一句话的回复时不再发送，避免重复点击不断取消正在生成的回答
    if (bAwaitingReply && Message==LastSubmittedText)
    {
        if (SendFeedback) PlayAnimation(SendFeedback);
        RefreshReply();
        return;
    }
    if (!Conversation->Submit(Resident.Get(),Message))
    {
        InputNotice=Conversation->CanSendNow()?TEXT("这句话没能发出，请重新选择交谈对象"):TEXT("刚说完一句，请稍后再发");
        InputNoticeUntil=FPlatformTime::Seconds()+1.5;
        RefreshReply();
        return;
    }
}
void UDeliveryConversationWidget::HandleSubmitted(ADeliveryNpcCharacter* Npc,FGuid MessageId,const FString& Text)
{
    if (Npc!=Resident.Get() || !MessageId.IsValid()) return;
    LastSubmittedText=Text; LastReplyText.Reset();
    SubmittedEventId=MessageId;
    DisplayedTurnId.Invalidate();
    SubmissionResult=TEXT("Sending");
    InputNotice.Reset(); InputNoticeUntil=0;
    ReplyWaitUntil=FPlatformTime::Seconds()+30;
    bAwaitingReply=true;
    bReplyTimedOut=false;
    RefreshReply();
    if (SendFeedback) PlayAnimation(SendFeedback);
    if (MessageArrive) PlayAnimation(MessageArrive);
}
void UDeliveryConversationWidget::HandleSubmissionBound(FGuid ClientId,FGuid MessageId)
{
    if (SubmittedEventId==ClientId && MessageId.IsValid()) SubmittedEventId=MessageId;
}
void UDeliveryConversationWidget::HandleVoiceError(FName Reason)
{
    if (Reason==TEXT("Busy")) InputNotice=TEXT("对方正听另一位玩家说话，稍后再试");
    else if (Reason==TEXT("Expired")) InputNotice=TEXT("刚才没听清，可以再说一次");
    else InputNotice=TEXT("暂时无法与二狗连麦，请稍后再试");
    InputNoticeUntil=FPlatformTime::Seconds()+3;
    RefreshReply();
}
void UDeliveryConversationWidget::ToggleVoice()
{
    if (auto* Speech=BoundSpeech.Get())
    {
        if (Speech->IsVoiceEnabled()) Speech->DisableVoice();
        else
        {
            const auto* PC=GetOwningPlayer();
            auto* HUD=PC?Cast<ADeliveryHUD>(PC->GetHUD()):nullptr;
            if (!HUD || !HUD->HasSpeechStatusWidget())
            {
                InputNotice=TEXT("语音界面尚未准备好，请重新打开游戏");
                InputNoticeUntil=FPlatformTime::Seconds()+3;
                RefreshReply();
                return;
            }
            if (Speech->EnableVoice())
            {
                HUD->CloseResidentConversation();
                return;
            }
        }
    }
    RefreshVoice();
}
void UDeliveryConversationWidget::BeginVoice()
{
    // 保留旧蓝图函数入口，连续连麦不再从按住按钮创建轮次
}
void UDeliveryConversationWidget::FinishVoice()
{
    // 松开按钮不影响由语音活动检测维护的当前句子
}
void UDeliveryConversationWidget::RefreshVoice()
{
    const auto* Speech=BoundSpeech.Get();
    const bool bEnabled=Speech && Speech->IsVoiceEnabled();
    if (VoiceToggleButton) VoiceToggleButton->SetIsEnabled(Speech!=nullptr);
    if (VoiceToggleLabel)
        VoiceToggleLabel->SetText(FText::FromString(bEnabled?TEXT("关闭连麦"):TEXT("与二狗连麦")));
    if (VoiceHoldButton)
    {
        VoiceHoldButton->SetVisibility(ESlateVisibility::Collapsed);
        VoiceHoldButton->SetIsEnabled(false);
    }
}
void UDeliveryConversationWidget::Retry()
{
    if (!RetryButton || RetryButton->GetVisibility()!=ESlateVisibility::Visible ||
        !RetryButton->GetIsEnabled() || LastSubmittedText.IsEmpty()) return;
    // 重试发送消息区的原话，输入框中的下一句保持原样
    const FString Message=LastSubmittedText;
    SendText(Message);
    FocusInput();
}
void UDeliveryConversationWidget::Send()
{
    if (!Input) return;
    const FString Draft=Input->GetText().ToString();
    const FString Text=Draft.TrimStartAndEnd();
    if (Text.IsEmpty()) return;
    const FGuid PreviousSubmission=SubmittedEventId;
    SendText(Text);
    // 请求被接收后立即腾空草稿，服务器回执只更新上方原话，不改玩家正在写的下一句
    if (SubmittedEventId!=PreviousSubmission && Input->GetText().ToString()==Draft)
        Input->SetText(FText::GetEmpty());
    FocusInput();
}
void UDeliveryConversationWidget::CommitInput(const FText&,ETextCommit::Type Method)
{
    if (Method==ETextCommit::OnEnter) Send();
}
bool UDeliveryConversationWidget::IsSuggestionsVisible() const
{
    return Suggestions && Suggestions->GetVisibility()==ESlateVisibility::Visible;
}
void UDeliveryConversationWidget::SetAvailableWidth(float Width)
{
    if (!ConversationWidth || !FMath::IsFinite(Width) || Width<=0) return;
    if (!ConversationWidth->IsWidthOverride() || !FMath::IsNearlyEqual(ConversationWidth->GetWidthOverride(),Width))
        ConversationWidth->SetWidthOverride(Width);
}
void UDeliveryConversationWidget::FocusInput()
{
    // 界面加入视口并切换输入模式后，直接把键盘交给输入框
    if (Input) Input->SetUserFocus(GetOwningPlayer());
    else SetUserFocus(GetOwningPlayer());
}
void UDeliveryConversationWidget::ToggleSuggestions()
{
    if (Suggestions) Suggestions->SetVisibility(IsSuggestionsVisible()?ESlateVisibility::Collapsed:ESlateVisibility::Visible);
}
void UDeliveryConversationWidget::Close()
{
    if (auto* PC=GetOwningPlayer())
        if (auto* HUD=Cast<ADeliveryHUD>(PC->GetHUD())) HUD->CloseResidentConversation();
}
void UDeliveryConversationWidget::NativeTick(const FGeometry& Geometry,float Dt)
{
    Super::NativeTick(Geometry,Dt);
    auto* PC=GetOwningPlayer();
    auto* Conversation=PC?PC->FindComponentByClass<UDeliveryConversationComponent>():nullptr;
    // 遮挡或距离变化时保留原话与发送结果，目标销毁或玩家离开控制器才关闭
    if (!Resident.IsValid() || !Conversation || !PC->GetPawn()) { Close(); return; }
    RefreshReply();
}

void UDeliveryConversationWidget::RefreshReply()
{
    auto* PC=GetOwningPlayer();
    auto* Conversation=PC?PC->FindComponentByClass<UDeliveryConversationComponent>():nullptr;
    if (!Reply || !Resident.IsValid() || !Conversation) return;
    const auto Speech=Resident->Presentation->GetDisplayedSpeech();
    const FName ReceivedResult=Conversation->GetSubmissionResult(SubmittedEventId);
    if (!ReceivedResult.IsNone()) SubmissionResult=ReceivedResult;
    const FName Result=SubmissionResult;
    const bool bNewSpeech=SubmittedEventId.IsValid() && Speech.bActive &&
        Speech.ReplyToEventId==SubmittedEventId && !Speech.Text.IsEmpty();
    const bool bRejected=Result==TEXT("TooFast") || Result==TEXT("Unavailable") || Result==TEXT("NotHeard") ||
        Result==TEXT("Empty") || Result==TEXT("Busy") || Result==TEXT("Expired");
    const bool bReplyFailed=Result==TEXT("ReplyFailed");
    const bool bActionReply=Result==TEXT("ActionReply");
    const auto Action=Resident->Agent->GetPublicState();
    // 首句通过服务器检查后结束本次等待，台词由人物附近的字幕显示
    // 待播查询同时检查本轮动作编号和停止通知，取消后的旧状态不能重新显示首句
    const bool bFirstSentenceReady=SubmittedEventId.IsValid() && Action.bStreamSpeech &&
        Action.GetReplyId().IsValid() && Action.ReplyToEventId==SubmittedEventId &&
        !Action.Speech.IsEmpty() && !bRejected && !bReplyFailed &&
        Resident->Presentation->IsWaitingForSpeechPlayback();
    if (bFirstSentenceReady && LastReplyText.IsEmpty() && !DisplayedTurnId.IsValid())
    {
        LastReplyText=Action.Speech;
        UE_LOG(LogTemp,Log,TEXT("NpcLatency speech=%s action=%s stage=reply_text_ready sequence=0"),
            *SubmittedEventId.ToString(),*Action.ActionId.ToString());
    }
    if (bNewSpeech && DisplayedTurnId!=Speech.TurnId)
    {
        // 首句可能已经显示；晚到的第零句也不能清掉后面的句子或退回旧编号
        if (Speech.Sequence>0 || !DisplayedTurnId.IsValid())
        {
            if (Speech.Sequence>0 || LastReplyText.IsEmpty()) LastReplyText+=Speech.Text;
            DisplayedTurnId=Speech.TurnId;
            UE_LOG(LogTemp,Log,TEXT("NpcLatency speech=%s turn=%s stage=reply_sentence_received sequence=%d"),
                *SubmittedEventId.ToString(),*Speech.TurnId.ToString(),Speech.Sequence);
        }
    }
    if (bNewSpeech || bFirstSentenceReady || bRejected || bReplyFailed || bActionReply || Result==TEXT("Interrupted"))
    {
        bAwaitingReply=false;
        bReplyTimedOut=false;
    }
    else if (bAwaitingReply && FPlatformTime::Seconds()>=ReplyWaitUntil)
    {
        bAwaitingReply=false;
        bReplyTimedOut=true;
    }

    const FText TargetName=FText::Format(NSLOCTEXT("DeliveryConversation","SpeakingTo","对 {0} 说"),Resident->DisplayName);
    if (SpeakerName && !SpeakerName->GetText().EqualTo(TargetName)) SpeakerName->SetText(TargetName);
    const bool bCanSend=Conversation->CanSendNow();
    if (SendButton && SendButton->GetIsEnabled()!=bCanSend) SendButton->SetIsEnabled(bCanSend);
    FString Text;
    if (!LastSubmittedText.IsEmpty())
    {
        const TCHAR* Label=bRejected?TEXT("没送到"):TEXT("你说");
        Text=FString::Printf(TEXT("%s：%s"),Label,*LastSubmittedText);
    }
    auto AppendNotice=[&Text](const FString& Notice)
    {
        if (!Text.IsEmpty()) Text+=TEXT("\n");
        Text+=Notice;
    };
    if (bReplyTimedOut) AppendNotice(TEXT("暂时没有回应，可以再试一次"));
    // 下方只保留玩家原话和必要反馈，NPC 台词不在这里重复显示
    if (Result==TEXT("TooFast")) AppendNotice(TEXT("刚才发得太快，这句话没有送到。稍后可以重试"));
    if (Result==TEXT("Unavailable")) AppendNotice(TEXT("请靠近对方，确认中间没有遮挡且对方没有倒地"));
    if (Result==TEXT("NotHeard")) AppendNotice(TEXT("对方没听到，请换个无遮挡的位置再说"));
    if (Result==TEXT("Empty")) AppendNotice(TEXT("没有发送文字，请重新输入"));
    if (Result==TEXT("Busy")) AppendNotice(TEXT("对方正听另一位玩家说话，稍后可以重试"));
    if (Result==TEXT("Expired")) AppendNotice(TEXT("这句话已取消，可以再试一次"));
    if (bReplyFailed) AppendNotice(TEXT("这次没能回复，可以再试一次"));
    if (FPlatformTime::Seconds()<InputNoticeUntil) AppendNotice(InputNotice);
    const ESlateVisibility SubmittedVisibility=Text.IsEmpty()?ESlateVisibility::Collapsed:ESlateVisibility::Visible;
    if (SubmittedLine && SubmittedLine->GetVisibility()!=SubmittedVisibility) SubmittedLine->SetVisibility(SubmittedVisibility);
    if (Reply->GetText().ToString()!=Text)
    {
        Reply->SetText(FText::FromString(Text));
        if (ReplyScroll) ReplyScroll->ScrollToStart();
    }
    RefreshFeedback(!LastSubmittedText.IsEmpty() && (bRejected || bReplyFailed || bReplyTimedOut));
}

void UDeliveryConversationWidget::RefreshFeedback(bool bCanRetry)
{
    if (RetryButton)
    {
        const auto RetryVisibility=bCanRetry?ESlateVisibility::Visible:ESlateVisibility::Collapsed;
        if (RetryButton->GetVisibility()!=RetryVisibility) RetryButton->SetVisibility(RetryVisibility);
        if (auto* PC=GetOwningPlayer())
            if (const auto* Conversation=PC->FindComponentByClass<UDeliveryConversationComponent>())
                RetryButton->SetIsEnabled(Conversation->CanSendNow());
    }
    if (!WaitingIndicator) return;
    const auto WaitingVisibility=bAwaitingReply?ESlateVisibility::HitTestInvisible:ESlateVisibility::Collapsed;
    if (WaitingIndicator->GetVisibility()!=WaitingVisibility) WaitingIndicator->SetVisibility(WaitingVisibility);
    if (!WaitingPulse) return;
    if (bAwaitingReply)
    {
        if (!IsAnimationPlaying(WaitingPulse)) PlayAnimation(WaitingPulse,0.f,0);
    }
    else if (IsAnimationPlaying(WaitingPulse)) StopAnimation(WaitingPulse);
}

void UDeliveryConversationWidget::SetResident(ADeliveryNpcCharacter* Value)
{
    if (auto* PC=GetOwningPlayer())
        if (auto* Conversation=PC->FindComponentByClass<UDeliveryConversationComponent>())
        {
            if (Resident.IsValid()) Conversation->ClearConversationTarget(Resident.Get());
            Conversation->SetConversationTarget(Value);
        }
    ResetConversation();
    Resident=Value;
    // 建议只是可复制的文字，玩家粘贴和修改后，通过输入框发送
    if (auto* PC=GetOwningPlayer(); PC && Suggestions && Resident.IsValid())
        if (auto* Conversation=PC->FindComponentByClass<UDeliveryConversationComponent>())
            Suggestions->SetText(FText::FromString(Conversation->GetProvokeText(Resident.Get())+
                TEXT("\n\n对不起，刚才是我太急了，请让我先送货吧\n\n先别动手，你俩把话说清楚。")));
    RefreshReply();
}

void UDeliveryConversationWidget::ResetConversation()
{
    ButtonVoiceToken={};
    StopAllAnimations();
    LastSubmittedText.Reset(); LastReplyText.Reset(); SubmittedEventId.Invalidate(); DisplayedTurnId.Invalidate();
    SubmissionResult=NAME_None; InputNotice.Reset(); InputNoticeUntil=0;
    ReplyWaitUntil=0; bAwaitingReply=false; bReplyTimedOut=false;
    if (Reply) Reply->SetText(FText::GetEmpty());
    if (SubmittedLine) SubmittedLine->SetVisibility(ESlateVisibility::Collapsed);
    if (Input) Input->SetText(FText::GetEmpty());
    if (RetryButton) RetryButton->SetVisibility(ESlateVisibility::Collapsed);
    if (WaitingIndicator) WaitingIndicator->SetVisibility(ESlateVisibility::Collapsed);
    if (SubmittedLine)
    {
        SubmittedLine->SetRenderOpacity(1.f);
        SubmittedLine->SetRenderTranslation(FVector2D::ZeroVector);
    }
    if (SendButton) SendButton->SetRenderScale(FVector2D(1.f,1.f));
    if (Suggestions)
    {
        Suggestions->SetText(FText::GetEmpty());
        Suggestions->SetVisibility(ESlateVisibility::Collapsed);
    }
}
