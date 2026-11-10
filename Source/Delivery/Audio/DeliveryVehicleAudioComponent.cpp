// Copyright Epic Games, Inc. All Rights Reserved.
#include "Audio/DeliveryVehicleAudioComponent.h"
#include "Audio/DeliveryAudioConfig.h"
#include "Components/AudioComponent.h"
#include "GameFramework/Actor.h"
#include "Delivery.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"

void UDeliveryVehicleAudioComponent::BeginPlay()
{
    Super::BeginPlay();
    // 按名称取得原来的发动机组件，避免误用车辆上其他声音组件
    TArray<UAudioComponent*> Components;
    GetOwner()->GetComponents(Components);
    for (UAudioComponent* Component : Components)
    {
        // 只有接管了发动机配置的车辆才停止旧音源，交通车仅借用鸣笛播放
        if (Config && Component->GetFName() == TEXT("EngineAudio"))
        {
            EngineAudio = Component;
            EngineAudio->Stop();
            break;
        }
    }
    if (Config && !Cast<UDeliveryVehicleAudioConfig>(Config))
    {
        UE_LOG(LogDelivery, Warning, TEXT("%s 的车辆音效需要 DeliveryVehicleAudioConfig 配置"), *GetOwner()->GetName());
    }
}

// 有驾驶员时按车速调整发动机声音，没有驾驶员时停止播放
void UDeliveryVehicleAudioComponent::UpdateEngine(bool bDriving, float SpeedRatio)
{
    if (!EngineAudio || GetNetMode() == NM_DedicatedServer)
    {
        return;
    }
    const auto* Settings = Cast<UDeliveryVehicleAudioConfig>(Config);
    if (!bDriving || !Settings || !Settings->EngineSound)
    {
        if (EngineAudio->IsPlaying())
        {
            EngineAudio->Stop();
        }
        return;
    }
    if (EngineAudio->GetSound() != Settings->EngineSound)
    {
        EngineAudio->SetSound(Settings->EngineSound);
    }
    if (!EngineAudio->IsPlaying())
    {
        EngineAudio->Play();
    }
    const float Alpha = FMath::Clamp(FMath::Abs(SpeedRatio), 0.0f, 1.0f);
    EngineAudio->SetPitchMultiplier(FMath::Lerp(Settings->IdlePitch, Settings->MaxPitch, Alpha));
    EngineAudio->SetVolumeMultiplier(FMath::Lerp(Settings->IdleVolume, Settings->MaxVolume, Alpha));
}

void UDeliveryVehicleAudioComponent::PlayHorn(USoundBase* Sound, float Volume, float Pitch, USoundAttenuation* Attenuation, bool bAllowLoop)
{
    if (!IsValid(Sound) || (Sound->IsLooping() && !bAllowLoop) || GetNetMode() == NM_DedicatedServer
        || !GetOwner() || !GetOwner()->GetRootComponent())
    {
        return;
    }
    if (!IsValid(HornAudio))
    {
        HornAudio = NewObject<UAudioComponent>(GetOwner());
        HornAudio->bAutoActivate = false;
        HornAudio->bAutoDestroy = false;
        HornAudio->bStopWhenOwnerDestroyed = true;
        HornAudio->SetupAttachment(GetOwner()->GetRootComponent());
        HornAudio->RegisterComponent();
    }
    HornAudio->Stop();
    HornAudio->AttenuationSettings = Attenuation;
    HornAudio->bOverrideAttenuation = Attenuation == nullptr;
    if (!Attenuation)
    {
        // 原始 SoundWave 未配置衰减时仍按三维位置播放，避免全地图同音量
        FSoundAttenuationSettings Settings;
        Settings.bAttenuate = true;
        Settings.bSpatialize = true;
        Settings.AttenuationShape = EAttenuationShape::Sphere;
        Settings.AttenuationShapeExtents = FVector(800.0f, 0.0f, 0.0f);
        Settings.FalloffDistance = 2400.0f;
        HornAudio->AdjustAttenuation(Settings);
    }
    HornAudio->SetSound(Sound);
    HornAudio->SetVolumeMultiplier(FMath::Max(0.0f, Volume));
    HornAudio->SetPitchMultiplier(FMath::Clamp(Pitch, 0.5f, 2.0f));
    HornAudio->Play();
}

void UDeliveryVehicleAudioComponent::StopHorn()
{
    if (IsValid(HornAudio))
    {
        HornAudio->Stop();
    }
}

void UDeliveryVehicleAudioComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopHorn();
    if (IsValid(HornAudio))
    {
        HornAudio->DestroyComponent();
    }
    HornAudio = nullptr;
    if (EngineAudio)
    {
        EngineAudio->Stop();
    }
    Super::EndPlay(EndPlayReason);
}
