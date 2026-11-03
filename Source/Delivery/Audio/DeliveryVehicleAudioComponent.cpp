// Copyright Epic Games, Inc. All Rights Reserved.
#include "Audio/DeliveryVehicleAudioComponent.h"
#include "Audio/DeliveryAudioConfig.h"
#include "Components/AudioComponent.h"
#include "GameFramework/Actor.h"
#include "Delivery.h"

void UDeliveryVehicleAudioComponent::BeginPlay()
{
    Super::BeginPlay();
    // 按名称取得原来的发动机组件，避免误用车辆上其他声音组件
    TArray<UAudioComponent*> Components;
    GetOwner()->GetComponents(Components);
    for (UAudioComponent* Component : Components)
    {
        if (Component->GetFName() == TEXT("EngineAudio"))
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

// 停车时停止循环音，行驶时按速度调整音调和音量
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

void UDeliveryVehicleAudioComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (EngineAudio)
    {
        EngineAudio->Stop();
    }
    Super::EndPlay(EndPlayReason);
}
