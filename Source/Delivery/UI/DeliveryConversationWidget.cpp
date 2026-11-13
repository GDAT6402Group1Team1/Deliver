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
#include "GameFramework/PlayerController.h"
#include "Input/Reply.h"
#include "InputCoreTypes.h"

void UDeliveryConversationWidget::NativeConstruct()
{
    Super::NativeConstruct();
    SetIsFocusable(true);
    if (SendButton) SendButton->OnClicked.AddUniqueDynamic(this,&UDeliveryConversationWidget::Send);
    if (SuggestionToggle) SuggestionToggle->OnClicked.AddUniqueDynamic(this,&UDeliveryConversationWidget::ToggleSuggestions);
    if (Input) Input->OnTextCommitted.AddUniqueDynamic(this,&UDeliveryConversationWidget::CommitInput);
    if (CloseButton) CloseButton->OnClicked.AddUniqueDynamic(this,&UDeliveryConversationWidget::Close);
    RefreshReply();
}
void UDeliveryConversationWidget::NativeDestruct()
{
    if (SendButton) SendButton->OnClicked.RemoveDynamic(this,&UDeliveryConversationWidget::Send);
    if (SuggestionToggle) SuggestionToggle->OnClicked.RemoveDynamic(this,&UDeliveryConversationWidget::ToggleSuggestions);
    if (Input) Input->OnTextCommitted.RemoveDynamic(this,&UDeliveryConversationWidget::CommitInput);
    if (CloseButton) CloseButton->OnClicked.RemoveDynamic(this,&UDeliveryConversationWidget::Close);
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
    const FString Message=Text.TrimStartAndEnd().Left(256);
    if (Message.IsEmpty()) return;
    // 正在等待同一句话的回复时不再发送，避免重复点击不断取消正在生成的回答
    if (bAwaitingReply && Message==LastSubmittedText)
    {
        InputNotice=TEXT("这句话已经发出");
        InputNoticeUntil=FPlatformTime::Seconds()+1.5;
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
    LastSubmittedText=Message; LastReplyText.Reset();
    SubmittedEventId=Conversation->GetLastSubmissionId();
    DisplayedTurnId.Invalidate();
    SubmissionResult=TEXT("Sending");
    InputNotice.Reset(); InputNoticeUntil=0;
    ReplyWaitUntil=FPlatformTime::Seconds()+30;
    bAwaitingReply=true;
    bReplyTimedOut=false;
    RefreshReply();
}
void UDeliveryConversationWidget::Send()
{
    if (!Input) return;
    const FString Text=Input->GetText().ToString().TrimStartAndEnd();
    if (Text.IsEmpty()) return;
    // 保留输入，距离或遮挡导致失败时玩家可以直接重试
    SendText(Text);
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
    const bool bRejected=Result==TEXT("TooFast") || Result==TEXT("Unavailable") || Result==TEXT("NotHeard") || Result==TEXT("Empty");
    const bool bReplyFailed=Result==TEXT("ReplyFailed");
    const bool bActionReply=Result==TEXT("ActionReply");
    const auto Action=Resident->Agent->GetPublicState();
    const bool bReplyAccepted=Action.Status==EDeliveryNpcActionStatus::Accepted ||
        Action.Status==EDeliveryNpcActionStatus::Running || Action.Status==EDeliveryNpcActionStatus::Succeeded;
    // 首句通过服务器检查后结束本次等待，台词由人物附近的字幕显示
    // 待播查询同时检查本轮动作编号和停止通知，取消后的旧状态不能重新显示首句
    const bool bFirstSentenceReady=SubmittedEventId.IsValid() && Action.bStreamSpeech &&
        Action.ActionId.IsValid() && Action.ReplyToEventId==SubmittedEventId && bReplyAccepted &&
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
    if (bNewSpeech || bFirstSentenceReady || bRejected || bReplyFailed || bActionReply)
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
        const TCHAR* Label=bRejected?TEXT("没送到"):Result==TEXT("Sending")?TEXT("正在发送"):TEXT("你说");
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
    if (bReplyFailed) AppendNotice(TEXT("这次没能回复，可以再试一次"));
    if (FPlatformTime::Seconds()<InputNoticeUntil) AppendNotice(InputNotice);
    const ESlateVisibility SubmittedVisibility=Text.IsEmpty()?ESlateVisibility::Collapsed:ESlateVisibility::Visible;
    if (SubmittedLine && SubmittedLine->GetVisibility()!=SubmittedVisibility) SubmittedLine->SetVisibility(SubmittedVisibility);
    if (Reply->GetText().ToString()!=Text)
    {
        Reply->SetText(FText::FromString(Text));
        if (ReplyScroll) ReplyScroll->ScrollToStart();
    }
}

void UDeliveryConversationWidget::SetResident(ADeliveryNpcCharacter* Value)
{
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
    LastSubmittedText.Reset(); LastReplyText.Reset(); SubmittedEventId.Invalidate(); DisplayedTurnId.Invalidate();
    SubmissionResult=NAME_None; InputNotice.Reset(); InputNoticeUntil=0;
    ReplyWaitUntil=0; bAwaitingReply=false; bReplyTimedOut=false;
    if (Reply) Reply->SetText(FText::GetEmpty());
    if (SubmittedLine) SubmittedLine->SetVisibility(ESlateVisibility::Collapsed);
    if (Input) Input->SetText(FText::GetEmpty());
    if (Suggestions)
    {
        Suggestions->SetText(FText::GetEmpty());
        Suggestions->SetVisibility(ESlateVisibility::Collapsed);
    }
}
