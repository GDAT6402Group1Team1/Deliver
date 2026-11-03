#pragma once

#include "CoreMinimal.h"

// 记录倒地稳定与起身进度，是否切换身体模式仍由主组件决定
class FDeliveryRagdollRecovery
{
public:
    void Reset()
    {
        StableTime = StandingTime = 0.f;
        FinishRise();
    }
    void BeginRise(float Height)
    {
        StartHeight = Height;
        RiseAlpha = StandingTime = 0.f;
        bRising = true;
    }
    void AdvanceRise(float DeltaTime, float Duration)
    {
        RiseAlpha = FMath::Min(1.f, RiseAlpha + DeltaTime / FMath::Max(Duration, .05f));
    }
    void FinishRise() { bRising = false; RiseAlpha = 1.f; }
    bool IsRising() const { return bRising; }
    float GetRiseAlpha() const { return RiseAlpha; }
    float GetStartHeight() const { return StartHeight; }

    // 条件中断就重新计时，不把多次短暂稳定累加为一次起身
    void UpdateStable(bool bStable, float DeltaTime) { StableTime = bStable ? StableTime + DeltaTime : 0.f; }
    void UpdateStanding(bool bStanding, float DeltaTime) { StandingTime = bStanding ? StandingTime + DeltaTime : 0.f; }
    void ResetStable() { StableTime = 0.f; }
    void ResetStanding() { StandingTime = 0.f; }
    float GetStableTime() const { return StableTime; }
    float GetStandingTime() const { return StandingTime; }

private:
    float StableTime = 0.f;
    float StandingTime = 0.f;
    float RiseAlpha = 1.f;
    float StartHeight = 0.f;
    bool bRising = false;
};
