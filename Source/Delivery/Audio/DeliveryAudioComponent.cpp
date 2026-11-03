// Copyright Epic Games, Inc. All Rights Reserved.
#include "Audio/DeliveryAudioComponent.h"
#include "Audio/DeliveryAudioConfig.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

UDeliveryAudioComponent::UDeliveryAudioComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

// 只监听当前对象，多个角色之间不共用订阅列表
void UDeliveryAudioComponent::BeginPlay()
{
    Super::BeginPlay();
    Events = GetOwner()->FindComponentByClass<UDeliveryEventComponent>();
    if (Events)
    {
        Events->OnEvent.AddUniqueDynamic(this, &UDeliveryAudioComponent::HandleEvent);
    }
}

void UDeliveryAudioComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (Events)
    {
        Events->OnEvent.RemoveDynamic(this, &UDeliveryAudioComponent::HandleEvent);
    }
    Events = nullptr;
    Super::EndPlay(EndPlayReason);
}

// 没有配置的事件不发声，其他监听者仍会收到通知
void UDeliveryAudioComponent::HandleEvent(EDeliveryEvent Event)
{
    if (!Config || GetNetMode() == NM_DedicatedServer)
    {
        return;
    }
    const FDeliverySoundSettings* Settings = Config->Sounds.Find(Event);
    if (Settings && Settings->Sound)
    {
        PlaySound(*Settings);
    }
}

void UDeliveryAudioComponent::PlaySound(const FDeliverySoundSettings& Settings)
{
    if (Settings.bSpatial)
    {
        UGameplayStatics::PlaySoundAtLocation(this, Settings.Sound, GetOwner()->GetActorLocation(),
                                            Settings.Volume, Settings.Pitch);
        return;
    }

    // 提示音只播放给操作玩家，服务器上的远端角色不能触发本机提示
    const APawn* Pawn = Cast<APawn>(GetOwner());
    const APlayerController* Controller = Cast<APlayerController>(GetOwner());
    if ((Pawn && Pawn->IsLocallyControlled()) || (Controller && Controller->IsLocalController()))
    {
        UGameplayStatics::PlaySound2D(this, Settings.Sound, Settings.Volume, Settings.Pitch);
    }
}
