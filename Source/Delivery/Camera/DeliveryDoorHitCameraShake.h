#pragma once
#include "Camera/CameraShakeBase.h"
#include "DeliveryDoorHitCameraShake.generated.h"

// 短促后坐与俯仰回弹，由 CameraManager 负责叠加、结束和世界清理
UCLASS()
class DELIVERY_API UDeliveryDoorHitPattern : public UCameraShakePattern
{
    GENERATED_BODY()
    float Elapsed = 0.f;
    virtual void GetShakePatternInfoImpl(FCameraShakeInfo& Out) const override;
    virtual void StartShakePatternImpl(const FCameraShakePatternStartParams&) override { Elapsed = 0.f; }
    virtual void UpdateShakePatternImpl(const FCameraShakePatternUpdateParams& Params, FCameraShakePatternUpdateResult& Out) override;
    virtual bool IsFinishedImpl() const override { return Elapsed >= .24f; }
    virtual void StopShakePatternImpl(const FCameraShakePatternStopParams&) override { Elapsed = .24f; }
};

UCLASS()
class DELIVERY_API UDeliveryDoorHitCameraShake : public UCameraShakeBase
{
    GENERATED_BODY()
public:
    UDeliveryDoorHitCameraShake(const FObjectInitializer& Initializer);
};
