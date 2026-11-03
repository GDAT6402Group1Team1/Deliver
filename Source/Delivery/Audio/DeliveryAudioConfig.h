// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Events/DeliveryEventComponent.h"
#include "DeliveryAudioConfig.generated.h"

class USoundBase;

/** 一次事件的播放设置 */
USTRUCT(BlueprintType)
struct FDeliverySoundSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="音效", meta=(DisplayName="声音"))
    TObjectPtr<USoundBase> Sound;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="音效", meta=(DisplayName="音量", ClampMin="0"))
    float Volume = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="音效", meta=(DisplayName="音调", ClampMin="0.1"))
    float Pitch = 1.0f;

    /** 开启时在对象位置播放，距离衰减由声音资源设置；关闭时只向操作玩家播放提示音 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="音效", meta=(DisplayName="在场景中播放"))
    bool bSpatial = false;
};

/** 事件到声音的配置，不保存正在播放的状态 */
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryAudioConfig : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="事件音效", meta=(DisplayName="事件与声音"))
    TMap<EDeliveryEvent, FDeliverySoundSettings> Sounds;
};

/** 车辆共用事件音效配置，并增加发动机参数 */
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryVehicleAudioConfig : public UDeliveryAudioConfig
{
    GENERATED_BODY()
public:
    /** 声音资源本身需要设置为循环 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="发动机", meta=(DisplayName="循环声音"))
    TObjectPtr<USoundBase> EngineSound;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="发动机", meta=(DisplayName="怠速音调", ClampMin="0.1"))
    float IdlePitch = 0.75f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="发动机", meta=(DisplayName="高速音调", ClampMin="0.1"))
    float MaxPitch = 1.5f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="发动机", meta=(DisplayName="怠速音量", ClampMin="0"))
    float IdleVolume = 0.35f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="发动机", meta=(DisplayName="高速音量", ClampMin="0"))
    float MaxVolume = 1.0f;
};
