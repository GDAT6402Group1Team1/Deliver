#include "Camera/DeliveryDoorHitCameraShake.h"

UDeliveryDoorHitCameraShake::UDeliveryDoorHitCameraShake(const FObjectInitializer& Initializer) : Super(Initializer)
{
    bSingleInstance = true;
    SetRootShakePattern(CreateDefaultSubobject<UDeliveryDoorHitPattern>(TEXT("DoorHit")));
}

void UDeliveryDoorHitPattern::GetShakePatternInfoImpl(FCameraShakeInfo& Out) const
{
    Out.Duration = FCameraShakeDuration(.24f);
    Out.BlendIn = 0.f;
    Out.BlendOut = .05f;
}

void UDeliveryDoorHitPattern::UpdateShakePatternImpl(const FCameraShakePatternUpdateParams& Params, FCameraShakePatternUpdateResult& Out)
{
    Elapsed += Params.DeltaTime;
    const float Envelope = FMath::Square(FMath::Clamp(1.f - Elapsed / .24f, 0.f, 1.f));
    const float Pulse = FMath::Sin(Elapsed * 65.f) * Envelope;
    Out.Location = FVector(-3.f * Pulse, 0.f, .6f * Pulse);
    Out.Rotation = FRotator(1.4f * Pulse, 0.f, .35f * Pulse);
}
