#pragma once

#include "Audio/DeliveryAudioComponent.h"
#include "Audio/DeliveryAudioConfig.h"
#include "DeliveryAudioTestComponent.generated.h"

/** 测试中记录播放请求，避免依赖扬声器和音频设备 */
UCLASS(Transient, NotBlueprintable)
class UDeliveryAudioTestComponent : public UDeliveryAudioComponent
{
    GENERATED_BODY()
public:
    int32 PlayCount = 0;
    float LastVolume = 0.0f;

protected:
    virtual void PlaySound(const FDeliverySoundSettings& Settings) override
    {
        ++PlayCount;
        LastVolume = Settings.Volume;
    }
};
