#include "UI/Controllers/DeliveryPhoneController.h"
#include "UI/HUD/DeliveryHUD.h"
#include "Player/DeliveryPlayerController.h"
#include "Phone/DeliveryPhoneCallQueueComponent.h"
#include "Audio/DeliveryDialogueVoiceComponent.h"
#include "Audio/DeliveryPhoneAudioComponent.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "Engine/World.h"
#include "TimerManager.h"

const FName UDeliveryPhoneController::PanelId(TEXT("DeliveryPhoneUI"));

/// @brief 玩家所在的世界
UWorld* UDeliveryPhoneController::GetWorld() const
{
    return Player.IsValid() ? Player->GetWorld() : nullptr;
}

// ── 生命周期 ──

/// @brief 绑定本机玩家和 HUD，订阅电子语音的逐字通知，再调用 BindPhone
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

// ── 手机面板 ──

/// @brief 客户端可能先收到 HUD，稍后才收到电话队列
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

/// @brief 手机键：播放开关音效，打开或收起面板，再调用蓝图事件 TogglePhoneUI
void UDeliveryPhoneController::Toggle()
{
    if (!Player.IsValid() || !HUD.IsValid()) return;
    if (auto* Audio = Player->FindComponentByClass<UDeliveryPhoneAudioComponent>()) Audio->PlayToggle();
    SetOpen(!IsOpen());
    // 现有手机蓝图通过这个事件显示或移除控件
    Player->TogglePhoneUI();
}

/// @brief 手机面板是否开着
bool UDeliveryPhoneController::IsOpen() const
{
    return HUD.IsValid() && HUD->IsPanelOpen(PanelId);
}

/// @brief 登记手机面板打开或收起
void UDeliveryPhoneController::SetOpen(bool bOpen)
{
    if (HUD.IsValid()) HUD->SetPanelOpen(PanelId, bOpen);
}

/// @brief 接听，转给玩家控制器的 RequestAnswerCall
void UDeliveryPhoneController::Answer()
{
    if (Player.IsValid()) Player->RequestAnswerCall();
}

/// @brief 挂断，转给玩家控制器的 RequestHangUpCall
void UDeliveryPhoneController::HangUp()
{
    if (Player.IsValid()) Player->RequestHangUpCall();
}

/// @brief 通话内容来自电话队列，不在界面里再保存一份任务状态
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

/// @brief 电子语音念出一个字时调用，加到字幕后面显示
void UDeliveryPhoneController::HandleGlyph(const FString& Glyph, int32 GlyphIndex)
{
    if (ActiveCallId <= 0) return;
    Subtitle.Append(Glyph);
    if (auto* Prompt = UDeliveryPromptSubsystem::Get(this))
        Prompt->SetSubtitle(CallerName, FText::FromString(Subtitle));
}

/// @brief 清掉来电人和字幕
void UDeliveryPhoneController::ClearSubtitle()
{
    CallerName = FText::GetEmpty();
    Subtitle.Reset();
    if (auto* Prompt = UDeliveryPromptSubsystem::Get(this)) Prompt->ClearSubtitle();
}

/// @brief HUD 销毁时解除订阅，避免旧处理器继续接收新电话
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
