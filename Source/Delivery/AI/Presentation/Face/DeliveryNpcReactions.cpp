#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundAttenuation.h"

// ── 反应 ──
// NPC 听到玩家说话、被打、被吓到时，会先发出一声短反应音（例如嗯、笑声、哎哟），声音来自人物配置 VoiceSet
// 服务器调用 PlayReaction，它调用 MulticastPlayReaction 让所有机器播放；每件事用 EventId 区分，同一件事只播一次
// 正在说台词时不播反应音；播放超过 VoiceSet->MaxDuration 后 TickReaction 调用 StopReaction 停下
namespace
{
/// @brief 把 EventId 记进列表，最多保留最近 32 个，用来认出已经播过或已经取消的事件
void RememberReaction(TArray<FGuid>& Events,FGuid EventId)
{
    if (!EventId.IsValid() || Events.Contains(EventId)) return;
    Events.Add(EventId);
    if (Events.Num()>32) Events.RemoveAt(0);
}
}

/// @brief 服务器：为 EventId 这件事播放一声 Reaction 类型的短反应音
/// 这件事已经播过或取消过、VoiceSet 里没有这种声音、NPC 正在说台词时返回 false
/// 否则调用 MulticastPlayReaction，并带上服务器当前时间，晚收到的机器从中间开始播
bool UDeliveryNpcPresentationComponent::PlayReaction(EDeliveryNpcVoiceReaction Reaction,FGuid EventId)
{
    if (bReactionsEnded || !GetOwner()->HasAuthority() || !EventId.IsValid() || !VoiceSet || !VoiceSet->GetSound(Reaction) ||
        !FMath::IsFinite(VoiceSet->MaxDuration) || !FMath::IsFinite(VoiceSet->Volume) || !FMath::IsFinite(VoiceSet->Pitch) ||
        SeenReactionEvents.Contains(EventId) || CancelledReactionEvents.Contains(EventId) ||
        GetDisplayedSpeech().bActive || Speech.bActive) return false;
    MulticastPlayReaction(Reaction,EventId,ServerTime());
    return true;
}

/// @brief 每台机器：收到播放通知，记下 EventId，再播放这声反应音
/// 收到时已经晚过 MaxDuration（最多 3 秒）就不播；专用服务器只记录时间不出声
void UDeliveryNpcPresentationComponent::MulticastPlayReaction_Implementation(EDeliveryNpcVoiceReaction Reaction,FGuid EventId,double StartTime)
{
    if (bReactionsEnded || !EventId.IsValid() || SeenReactionEvents.Contains(EventId) || CancelledReactionEvents.Contains(EventId)) return;
    RememberReaction(SeenReactionEvents,EventId);
    if (!VoiceSet || !FMath::IsFinite(StartTime) || !FMath::IsFinite(VoiceSet->MaxDuration) ||
        !FMath::IsFinite(VoiceSet->Volume) || !FMath::IsFinite(VoiceSet->Pitch)) return;
    auto* Sound=VoiceSet->GetSound(Reaction);
    if (!Sound || GetDisplayedSpeech().bActive) return;
    const double Duration=FMath::Clamp(VoiceSet->MaxDuration,.1f,3.f);
    const double Elapsed=FMath::Max(0.0,ServerTime()-StartTime);
    if (Elapsed>=Duration) return;

    // 先调用 MulticastStopReaction_Implementation 停掉上一声反应音；它的 EventId 记进 CancelledReactionEvents，之后迟到的播放通知不会再播它
    MulticastStopReaction_Implementation(ReactionEventId);
    ReactionEventId=EventId;ReactionEndsAt=FPlatformTime::Seconds()+Duration-Elapsed;
    if (GetNetMode()==NM_DedicatedServer) return;
    if (!ReactionAudio)
    {
        ReactionAudio=NewObject<UAudioComponent>(GetOwner());
        ReactionAudio->bAutoActivate=false;
        ReactionAudio->SetupAttachment(GetOwner()->GetRootComponent());
        ReactionAudio->RegisterComponent();
    }

    // 每次播放都从 VoiceSet 重新读衰减配置：ReactionAttenuation 为空时改用默认值，150 cm 内不衰减，之后 800 cm 内衰减到听不见
    ReactionAudio->bAllowSpatialization=true;
    ReactionAudio->AttenuationSettings=VoiceSet->ReactionAttenuation;
    ReactionAudio->bOverrideAttenuation=ReactionAudio->AttenuationSettings==nullptr;
    if (ReactionAudio->bOverrideAttenuation)
    {
        ReactionAudio->AttenuationOverrides=FSoundAttenuationSettings();
        ReactionAudio->AttenuationOverrides.bAttenuate=true;
        ReactionAudio->AttenuationOverrides.bSpatialize=true;
        ReactionAudio->AttenuationOverrides.AttenuationShapeExtents=FVector(150,0,0);
        ReactionAudio->AttenuationOverrides.FalloffDistance=800;
    }
    ReactionAudio->SetSound(Sound);
    ReactionAudio->SetVolumeMultiplier(FMath::Clamp(VoiceSet->Volume,0.f,2.f));
    ReactionAudio->SetPitchMultiplier(FMath::Clamp(VoiceSet->Pitch,.5f,1.5f));

    // 从 Elapsed 秒处开始播，晚收到通知的机器和别的机器播到同一个位置
    if (GetWorld()->AllowAudioPlayback())
    {
        ReactionAudio->Play(float(Elapsed));
        UE_LOG(LogTemp,Log,TEXT("NpcLatency reaction=%s stage=reaction_play_requested kind=%d role=%d late_ms=%d"),
            *EventId.ToString(),int32(Reaction),int32(GetOwner()->GetLocalRole()),int32(Elapsed*1000));
    }
}

/// @brief 停掉当前这声反应音：服务器调用 MulticastStopReaction 通知所有机器，客户端只停自己的
void UDeliveryNpcPresentationComponent::StopReaction()
{
    if (GetOwner()->HasAuthority() && ReactionEventId.IsValid()) MulticastStopReaction(ReactionEventId);
    else MulticastStopReaction_Implementation(ReactionEventId);
}

/// @brief 每台机器：停止 EventId 这声反应音，清掉 ReactionEventId 和 ReactionEndsAt
void UDeliveryNpcPresentationComponent::MulticastStopReaction_Implementation(FGuid EventId)
{
    // 先把 EventId 记进 CancelledReactionEvents：停止通知比播放通知先到时，之后的播放通知看到它就不播
    RememberReaction(CancelledReactionEvents,EventId);
    if (EventId!=ReactionEventId) return;
    if (ReactionAudio) { ReactionAudio->Stop();ReactionAudio->SetSound(nullptr); }
    ReactionEventId.Invalidate();ReactionEndsAt=0;
}

/// @brief 每帧检查：当前反应音播到 ReactionEndsAt 就调用 StopReaction
void UDeliveryNpcPresentationComponent::TickReaction()
{
    if (ReactionEventId.IsValid() && FPlatformTime::Seconds()>=ReactionEndsAt) StopReaction();
}
