// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Audio/DeliveryAudioComponent.h"
#include "DeliveryVehicleAudioComponent.generated.h"

class UAudioComponent;
class USoundBase;
class USoundAttenuation;

/** 车辆事件音效和发动机循环音，使用 DeliveryVehicleAudioConfig 配置 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryVehicleAudioComponent : public UDeliveryAudioComponent
{
    GENERATED_BODY()
public:
    /** 车辆每帧传入是否有人驾驶，以及当前速度除以最高速度得到的比例 */
    void UpdateEngine(bool bDriving, float SpeedRatio);

    /** 收到服务器确认后在车上播放一次鸣笛，复用本车音源，不与发动机互相停止 */
    void PlayHorn(USoundBase* Sound, float Volume, float Pitch, USoundAttenuation* Attenuation);
    void StopHorn();

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    // 复用车辆已有的 EngineAudio，保留挂接和衰减设置
    UPROPERTY(Transient)
    TObjectPtr<UAudioComponent> EngineAudio;

    UPROPERTY(Transient)
    TObjectPtr<UAudioComponent> HornAudio;

    friend class FDeliveryTrafficHornTest;
};
