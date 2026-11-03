#include "UI/Controllers/DeliveryPhoneController.h"
#include "UI/DeliveryHUD.h"
#include "Player/DeliveryPlayerController.h"
#include "Phone/DeliveryPhoneCallQueueComponent.h"
#include "Audio/DeliveryDialogueVoiceComponent.h"
#include "Audio/DeliveryPhoneAudioComponent.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "Engine/World.h"
#include "TimerManager.h"

const FName UDeliveryPhoneController::PanelId(TEXT("DeliveryPhoneUI"));

UWorld* UDeliveryPhoneController::GetWorld() const
{
    return Player.IsValid() ? Player->GetWorld() : nullptr;
}

void UDeliveryPhoneController::Initialize(ADeliveryPlayerController* InPlayer, ADeliveryHUD* InHUD)
{
    Shutdown();
    if (!InPlayer || !InPlayer->IsLocalController()) return;
    Player = InPlayer;
    HUD = InHUD;
    Voice = InPlayer->GetDialogueVoice();
    if (Voice.IsValid()) Voice->OnGlyphRevealed.AddUniqueDynamic(this, &UDeliveryPhoneController::HandleGlyph);
    BindPhone();
}

// 客户端可能先收到 HUD，稍后才收到电话队列
void UDeliveryPhoneController::BindPhone()
{
    if (!GetWorld()) return;
    Phone = UDeliveryPhoneCallQueueComponent::Get(this);
    if (!Phone.IsValid())
    {
        GetWorld()->GetTimerManager().SetTimer(BindTimer, this, &UDeliveryPhoneController::BindPhone, 0.5f, false);
        return;
    }
    GetWorld()->GetTimerManager().ClearTimer(BindTimer);
    Phone->OnPhoneStateChanged.AddUniqueDynamic(this, &UDeliveryPhoneController::HandleCallChanged);
    FDeliveryPhoneCall Call;
    if (Phone->GetCurrentCall(Call)) HandleCallChanged(Phone->GetCallState(), Call);
}

void UDeliveryPhoneController::Toggle()
{
    if (!Player.IsValid() || !HUD.IsValid()) return;
    if (auto* Audio = Player->FindComponentByClass<UDeliveryPhoneAudioComponent>()) Audio->PlayToggle();
    SetOpen(!IsOpen());
    // 现有手机蓝图通过这个事件显示或移除控件
    Player->TogglePhoneUI();
}

bool UDeliveryPhoneController::IsOpen() const
{
    return HUD.IsValid() && HUD->IsPanelOpen(PanelId);
}

void UDeliveryPhoneController::SetOpen(bool bOpen)
{
    if (HUD.IsValid()) HUD->SetPanelOpen(PanelId, bOpen);
}

void UDeliveryPhoneController::Answer()
{
    if (Player.IsValid()) Player->RequestAnswerCall();
}

void UDeliveryPhoneController::HangUp()
{
    if (Player.IsValid()) Player->RequestHangUpCall();
}

// 通话内容来自电话队列，不在界面里再保存一份任务状态
void UDeliveryPhoneController::HandleCallChanged(EDeliveryPhoneCallState State, const FDeliveryPhoneCall& Call)
{
    if (State != EDeliveryPhoneCallState::InCall)
    {
        if (Voice.IsValid()) Voice->StopSpeaking();
        ActiveCallId = 0;
        ClearSubtitle();
        return;
    }
    if (Call.CallId <= 0 || Call.CallId == ActiveCallId) return;
    ActiveCallId = Call.CallId;
    ClearSubtitle();
    const FDeliveryPhoneCallContent Content = UDeliveryPhoneCallQueueComponent::GetCallContent(Call);
    CallerName = Content.CallerName;
    if (Voice.IsValid()) Voice->SpeakText(Content.Dialogue);
}

void UDeliveryPhoneController::HandleGlyph(const FString& Glyph, int32 GlyphIndex)
{
    if (ActiveCallId <= 0) return;
    Subtitle.Append(Glyph);
    if (auto* Prompt = UDeliveryPromptSubsystem::Get(this))
        Prompt->SetSubtitle(CallerName, FText::FromString(Subtitle));
}

void UDeliveryPhoneController::ClearSubtitle()
{
    CallerName = FText::GetEmpty();
    Subtitle.Reset();
    if (auto* Prompt = UDeliveryPromptSubsystem::Get(this)) Prompt->ClearSubtitle();
}

// HUD 销毁时解除订阅，避免旧处理器继续接收新电话
void UDeliveryPhoneController::Shutdown()
{
    if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(BindTimer);
    if (Phone.IsValid()) Phone->OnPhoneStateChanged.RemoveDynamic(this, &UDeliveryPhoneController::HandleCallChanged);
    if (Voice.IsValid())
    {
        Voice->OnGlyphRevealed.RemoveDynamic(this, &UDeliveryPhoneController::HandleGlyph);
        Voice->StopSpeaking();
    }
    if (Player.IsValid()) ClearSubtitle();
    ActiveCallId = 0;
    Phone.Reset();
    Voice.Reset();
    HUD.Reset();
    Player.Reset();
}
