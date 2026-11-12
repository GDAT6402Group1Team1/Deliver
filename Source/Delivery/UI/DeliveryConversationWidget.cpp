#include "UI/DeliveryConversationWidget.h"
#include "UI/DeliveryHUD.h"
#include "AI/Residents/DeliveryConversationComponent.h"
#include "AI/Residents/DeliveryResidentCharacter.h"
#include "AI/Residents/DeliveryNpcPresentationComponent.h"
#include "Blueprint/WidgetTree.h"
#include "Components/VerticalBox.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/EditableTextBox.h"
#include "GameFramework/PlayerController.h"

TSharedRef<SWidget> UDeliveryConversationWidget::RebuildWidget()
{
    // 在 Slate 取得根控件之前构建，NativeConstruct 才赋 RootWidget 会显示为空面板
    if (WidgetTree->RootWidget) return Super::RebuildWidget();
    auto* Border=WidgetTree->ConstructWidget<UBorder>(); Border->SetBrushColor(FLinearColor(.025f,.035f,.06f,.95f)); Border->SetPadding(FMargin(20));
    WidgetTree->RootWidget=Border; auto* Box=WidgetTree->ConstructWidget<UVerticalBox>(); Border->SetContent(Box);
    Reply=WidgetTree->ConstructWidget<UTextBlock>(); Reply->SetAutoWrapText(true); Box->AddChild(Reply);
    Input=WidgetTree->ConstructWidget<UEditableTextBox>(); Input->SetHintText(FText::FromString(TEXT("说点什么（最多256字）"))); Box->AddChild(Input);
    auto Button=[&](const TCHAR* Text,auto Callback)
    {
        auto* B=WidgetTree->ConstructWidget<UButton>(); auto* Label=WidgetTree->ConstructWidget<UTextBlock>(); Label->SetText(FText::FromString(Text));
        B->AddChild(Label); Box->AddChild(B); B->OnClicked.Add(Callback);
    };
    FScriptDelegate D; D.BindUFunction(this,TEXT("Send")); Button(TEXT("发送"),D);
    D.BindUFunction(this,TEXT("Apologize")); Button(TEXT("道歉：对不起，刚才是我太急了"),D);
    D.BindUFunction(this,TEXT("Provoke")); Button(TEXT("拱火：他刚才是不是在说你？"),D);
    D.BindUFunction(this,TEXT("Close")); Button(TEXT("结束交谈"),D);
    return Super::RebuildWidget();
}
void UDeliveryConversationWidget::SendText(const FString& Text)
{ if (Resident.IsValid()) if (auto* C=GetOwningPlayer()->FindComponentByClass<UDeliveryConversationComponent>()) C->Submit(Resident.Get(),Text); }
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
{ if (auto* HUD=Cast<ADeliveryHUD>(GetOwningPlayer()->GetHUD())) HUD->CloseResidentConversation(); }
void UDeliveryConversationWidget::NativeTick(const FGeometry& Geometry,float Dt)
{
    Super::NativeTick(Geometry,Dt);
    auto* C=GetOwningPlayer()->FindComponentByClass<UDeliveryConversationComponent>();
    if (!Resident.IsValid() || !C || !C->CanTalkTo(Resident.Get())) { Close(); return; }
    const auto Speech=Resident->Presentation->GetSpeech();
    // 对话框保留最近一句实际台词，后台请求与排队状态不作为角色台词显示
    FString Text=Resident->DisplayName.ToString();
    if (!Speech.Text.IsEmpty()) Text+=TEXT("\n")+Speech.Text;
    if (C->GetLastResult()==TEXT("TooFast")) Text+=TEXT("\n说慢一点");
    if (C->GetLastResult()==TEXT("Unavailable")) Text+=TEXT("\n请靠近对方，确认中间没有遮挡且对方没有倒地");
    if (C->GetLastResult()==TEXT("NotHeard")) Text+=TEXT("\n对方没听到，请换个无遮挡的位置再说") ;
    Reply->SetText(FText::FromString(Text));
}

void UDeliveryConversationWidget::SetResident(ADeliveryResidentCharacter* Value) { Resident=Value; }
