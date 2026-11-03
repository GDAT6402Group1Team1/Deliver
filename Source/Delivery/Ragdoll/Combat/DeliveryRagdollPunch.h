#pragma once

#include "CoreMinimal.h"

// 保存当前出拳指令，手臂收回的姿态仍由 BoxingPose 处理
class FDeliveryRagdollPunch
{
public:
    void Begin(const FVector& AimDirection, float Side)
    {
        Direction = AimDirection.GetSafeNormal2D();
        HandSide = FMath::Sign(Side);
        bActive = !Direction.IsNearlyZero();
        bReleased = false;
    }

    void Release() { bReleased = bActive; }

    // 结束攻击后保留最后拳路，躯干和手臂仍需沿原方向完成收拳
    void End() { bActive = false; bReleased = false; }

    bool IsActive() const { return bActive; }
    bool IsReleased() const { return bReleased; }
    float GetHandSide() const { return HandSide; }
    const FVector& GetDirection() const { return Direction; }

private:
    bool bActive = false;
    bool bReleased = false;
    float HandSide = 1.f;
    FVector Direction = FVector::ForwardVector;
};
