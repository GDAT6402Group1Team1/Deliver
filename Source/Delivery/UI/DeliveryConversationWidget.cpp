#include "UI/DeliveryConversationWidget.h"
#include "UI/DeliveryHUD.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Blueprint/WidgetTree.h"
#include "Components/VerticalBox.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"
#include "GameFramework/PlayerController.h"
#include "Input/Reply.h"
#include "InputCoreTypes.h"

TSharedRef<SWidget> UDeliveryConversationWidget::RebuildWidget()
{
    // 在 Slate 取得根控件之前构建，NativeConstruct 才赋 RootWidget 会显示为空面板
    if (WidgetTree->RootWidget) return Super::RebuildWidget();
    auto* Border=WidgetTree->ConstructWidget<UBorder>(); Border->SetBrushColor(FLinearColor(.025f,.035f,.06f,.95f)); Border->SetPadding(FMargin(20));
    WidgetTree->RootWidget=Border; auto* Box=WidgetTree->ConstructWidget<UVerticalBox>(); Border->SetContent(Box);
    Reply=WidgetTree->ConstructWidget<UTextBlock>(); Reply->SetAutoWrapText(true); Box->AddChild(Reply);
    Input=WidgetTree->ConstructWidget<UEditableTextBox>(); Input->SetHintText(FText::FromString(TEXT("说点什么（最多256字）"))); Box->AddChild(Input);
    auto Button=[&](const TCHAR* Text)
    {
        auto* B=WidgetTree->ConstructWidget<UButton>(); auto* Label=WidgetTree->ConstructWidget<UTextBlock>(); Label->SetText(FText::FromString(Text));
        B->AddChild(Label); Box->AddChild(B); return B;
    };
    SendButton=Button(TEXT("发送"));
    ApologizeButton=Button(TEXT("道歉"));
    ProvokeButton=Button(TEXT("拱火"));
    CloseButton=Button(TEXT("结束"));
    return Super::RebuildWidget();
}
void UDeliveryConversationWidget::NativeConstruct()
{
    Super::NativeConstruct();
    SetIsFocusable(true);
    if (SendButton) SendButton->OnClicked.AddUniqueDynamic(this,&UDeliveryConversationWidget::Send);
    if (ApologizeButton) ApologizeButton->OnClicked.AddUniqueDynamic(this,&UDeliveryConversationWidget::Apologize);
    if (ProvokeButton) ProvokeButton->OnClicked.AddUniqueDynamic(this,&UDeliveryConversationWidget::Provoke);
    if (CloseButton) CloseButton->OnClicked.AddUniqueDynamic(this,&UDeliveryConversationWidget::Close);
    RefreshReply();
}
void UDeliveryConversationWidget::NativeDestruct()
{
    if (SendButton) SendButton->OnClicked.RemoveDynamic(this,&UDeliveryConversationWidget::Send);
    if (ApologizeButton) ApologizeButton->OnClicked.RemoveDynamic(this,&UDeliveryConversationWidget::Apologize);
    if (ProvokeButton) ProvokeButton->OnClicked.RemoveDynamic(this,&UDeliveryConversationWidget::Provoke);
    if (CloseButton) CloseButton->OnClicked.RemoveDynamic(this,&UDeliveryConversationWidget::Close);
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
    // 正在等待同一句话的回复时不再发送，避免重复点击不断取消正在生成的回答
    if (bAwaitingReply && Message==LastSubmittedText) return;
    if (!Conversation->Submit(Resident.Get(),Message)) return;
    LastSubmittedText=Message; LastReplyText.Reset();
    SubmittedEventId=Conversation->GetLastSubmissionId();
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
void UDeliveryConversationWidget::Apologize() { SendText(TEXT("对不起，刚才是我太急了，请让我先送货吧")); }
void UDeliveryConversationWidget::Provoke() { SendText(TEXT("他刚才是不是在说你？你们到底谁有理？")); }
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
    if (!Resident.IsValid() || !Conversation || !Conversation->CanTalkTo(Resident.Get())) { Close(); return; }
    RefreshReply();
}

void UDeliveryConversationWidget::RefreshReply()
{
    auto* PC=GetOwningPlayer();
    auto* Conversation=PC?PC->FindComponentByClass<UDeliveryConversationComponent>():nullptr;
    if (!Reply || !Resident.IsValid() || !Conversation) return;
    const auto Speech=Resident->Presentation->GetDisplayedSpeech();
    const FName Result=Conversation->GetLastResult();
    const bool bNewSpeech=SubmittedEventId.IsValid() && Speech.ReplyToEventId==SubmittedEventId && !Speech.Text.IsEmpty();
    const bool bRejected=Result==TEXT("TooFast") || Result==TEXT("Unavailable") || Result==TEXT("NotHeard") || Result==TEXT("Empty");
    if (bNewSpeech && DisplayedTurnId!=Speech.TurnId)
    {
        if (Speech.Sequence==0) LastReplyText.Reset();
        LastReplyText+=Speech.Text;
        DisplayedTurnId=Speech.TurnId;
        UE_LOG(LogTemp,Log,TEXT("NpcLatency speech=%s turn=%s stage=text_display sequence=%d"),
            *SubmittedEventId.ToString(),*Speech.TurnId.ToString(),Speech.Sequence);
    }
    if (bNewSpeech || bRejected)
    {
        bAwaitingReply=false;
        bReplyTimedOut=false;
    }
    else if (bAwaitingReply && FPlatformTime::Seconds()>=ReplyWaitUntil)
    {
        bAwaitingReply=false;
        bReplyTimedOut=true;
    }

    if (SpeakerName) SpeakerName->SetText(Resident->DisplayName);
    FString Text;
    if (!SpeakerName) Text=Resident->DisplayName.ToString()+TEXT("\n");
    if (bReplyTimedOut) Text+=TEXT("暂时没有回应，可以再试一次");
    // 只显示与本次发言编号一致的回复，旧台词和旁人的交谈不会结束本机等待
    else if (!LastReplyText.IsEmpty()) Text+=LastReplyText;
    else if (LastSubmittedText.IsEmpty() && !Speech.Text.IsEmpty()) Text+=Speech.Text;
    if (Result==TEXT("TooFast")) Text+=TEXT("\n说慢一点");
    if (Result==TEXT("Unavailable")) Text+=TEXT("\n请靠近对方，确认中间没有遮挡且对方没有倒地");
    if (Result==TEXT("NotHeard")) Text+=TEXT("\n对方没听到，请换个无遮挡的位置再说");
    if (Reply->GetText().ToString()!=Text)
    {
        Reply->SetText(FText::FromString(Text));
        if (ReplyScroll) ReplyScroll->ScrollToEnd();
    }
}

void UDeliveryConversationWidget::SetResident(ADeliveryNpcCharacter* Value)
{
    Resident=Value;
    LastSubmittedText.Reset(); LastReplyText.Reset(); SubmittedEventId.Invalidate();
    ReplyWaitUntil=0; bAwaitingReply=false; bReplyTimedOut=false;
    RefreshReply();
}
