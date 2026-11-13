#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundAttenuation.h"

namespace
{
void RememberReaction(TArray<FGuid>& Events,FGuid EventId)
{
    if (!EventId.IsValid() || Events.Contains(EventId)) return;
    Events.Add(EventId);
    if (Events.Num()>32) Events.RemoveAt(0);
}
}

bool UDeliveryNpcPresentationComponent::PlayReaction(EDeliveryNpcVoiceReaction Reaction,FGuid EventId)
{
    if (bReactionsEnded || !GetOwner()->HasAuthority() || !EventId.IsValid() || !VoiceSet || !VoiceSet->GetSound(Reaction) ||
        !FMath::IsFinite(VoiceSet->MaxDuration) || !FMath::IsFinite(VoiceSet->Volume) || !FMath::IsFinite(VoiceSet->Pitch) ||
        SeenReactionEvents.Contains(EventId) || CancelledReactionEvents.Contains(EventId) ||
        GetDisplayedSpeech().bActive || Speech.bActive) return false;
    MulticastPlayReaction(Reaction,EventId,ServerTime());
    return true;
}

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
    // 换短音时先停止上一事件，迟到的旧通知不能把它重新播出来
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
    // 每次播放都读取人物配置，保证更换或清空声音配置后不会沿用旧范围
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
    if (GetWorld()->AllowAudioPlayback())
    {
        ReactionAudio->Play(float(Elapsed));
        UE_LOG(LogTemp,Log,TEXT("NpcLatency reaction=%s stage=reaction_play_requested kind=%d role=%d late_ms=%d"),
            *EventId.ToString(),int32(Reaction),int32(GetOwner()->GetLocalRole()),int32(Elapsed*1000));
    }
}

void UDeliveryNpcPresentationComponent::StopReaction()
{
    if (GetOwner()->HasAuthority() && ReactionEventId.IsValid()) MulticastStopReaction(ReactionEventId);
    else MulticastStopReaction_Implementation(ReactionEventId);
}

void UDeliveryNpcPresentationComponent::MulticastStopReaction_Implementation(FGuid EventId)
{
    // 即使停止通知先到，也要记下编号，拒绝随后迟到的播放通知
    RememberReaction(CancelledReactionEvents,EventId);
    if (EventId!=ReactionEventId) return;
    if (ReactionAudio) { ReactionAudio->Stop();ReactionAudio->SetSound(nullptr); }
    ReactionEventId.Invalidate();ReactionEndsAt=0;
}

void UDeliveryNpcPresentationComponent::TickReaction()
{
    if (ReactionEventId.IsValid() && FPlatformTime::Seconds()>=ReactionEndsAt) StopReaction();
}
