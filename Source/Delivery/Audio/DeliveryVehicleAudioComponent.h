// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Audio/DeliveryAudioComponent.h"
#include "DeliveryVehicleAudioComponent.generated.h"

class UAudioComponent;

/** 车辆事件音效和发动机循环音，使用 DeliveryVehicleAudioConfig 配置 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryVehicleAudioComponent : public UDeliveryAudioComponent
{
    GENERATED_BODY()
public:
    /** 车辆完成本帧移动后传入驾驶状态和速度比例 */
    void UpdateEngine(bool bDriving, float SpeedRatio);

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    // 复用车辆已有的 EngineAudio，保留挂接和衰减设置
    UPROPERTY(Transient)
    TObjectPtr<UAudioComponent> EngineAudio;
};
