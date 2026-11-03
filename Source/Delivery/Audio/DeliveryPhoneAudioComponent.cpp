#include "Audio/DeliveryPhoneAudioComponent.h"
#include "Phone/DeliveryPhoneCallQueueComponent.h"
#include "Components/AudioComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "Engine/World.h"
#include "TimerManager.h"

UDeliveryPhoneAudioComponent::UDeliveryPhoneAudioComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

bool UDeliveryPhoneAudioComponent::IsLocalPlayer() const
{
    const auto* Player = Cast<APlayerController>(GetOwner());
    return Player && Player->IsLocalController();
}

void UDeliveryPhoneAudioComponent::BeginPlay()
{
    Super::BeginPlay();
    BindPhone();
}

void UDeliveryPhoneAudioComponent::BindPhone()
{
    if (!IsLocalPlayer()) return;
    Phone = UDeliveryPhoneCallQueueComponent::Get(this);
    if (!Phone.IsValid())
    {
        GetWorld()->GetTimerManager().SetTimer(BindTimer, this, &UDeliveryPhoneAudioComponent::BindPhone, 0.5f, false);
        return;
    }
    Phone->OnPhoneStateChanged.AddUniqueDynamic(this, &UDeliveryPhoneAudioComponent::HandleCallChanged);
    GetWorld()->GetTimerManager().ClearTimer(BindTimer);
    FDeliveryPhoneCall Call;
    if (Phone->GetCurrentCall(Call)) HandleCallChanged(Phone->GetCallState(), Call);
}

void UDeliveryPhoneAudioComponent::PlayToggle()
{
    if (IsLocalPlayer() && PhoneToggleSound) UGameplayStatics::PlaySound2D(this, PhoneToggleSound);
}

void UDeliveryPhoneAudioComponent::HandleCallChanged(EDeliveryPhoneCallState State, const FDeliveryPhoneCall& Call)
{
    if (!IsLocalPlayer() || (bHasState && State == LastState)) return;
    const bool bFirstState = !bHasState;
    const EDeliveryPhoneCallState Previous = LastState;
    bHasState = true;
    LastState = State;
    StopRinging();
    if (State == EDeliveryPhoneCallState::Ringing)
    {
        if (IncomingCallSound)
            RingAudio = UGameplayStatics::SpawnSound2D(this, IncomingCallSound, 1.f, 1.f, 0.f, nullptr, false, false);
    }
    // 晚加入时只恢复铃声，不补播已经发生的接听或挂断音效
    else if (!bFirstState && State == EDeliveryPhoneCallState::InCall && AnswerCallSound)
        UGameplayStatics::PlaySound2D(this, AnswerCallSound);
    else if (!bFirstState && State == EDeliveryPhoneCallState::Idle && Previous != State && HangUpCallSound)
        UGameplayStatics::PlaySound2D(this, HangUpCallSound);
}

void UDeliveryPhoneAudioComponent::StopRinging()
{
    if (!RingAudio) return;
    RingAudio->Stop();
    RingAudio->DestroyComponent();
    RingAudio = nullptr;
}

void UDeliveryPhoneAudioComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    if (Phone.IsValid()) Phone->OnPhoneStateChanged.RemoveDynamic(this, &UDeliveryPhoneAudioComponent::HandleCallChanged);
    if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(BindTimer);
    StopRinging();
    Super::EndPlay(Reason);
}
