#pragma once

#include "CoreMinimal.h"

// 需要由恢复流程处理的身体异常
enum class EDeliveryRecoveryIssue : uint8
{
    FootBlocked,
    FeetCrossed,
    ArmBlocked
};

// 管理脚步、手臂异常恢复和起身进度，物理目标由布娃娃组件执行
class FDeliveryRagdollRecovery
{
public:
    void Reset()
    {
        ResetFeet();
        ResetArms();
        StableTime = StandingTime = 0.f;
        FinishRise();
    }
    // 异常只改变恢复状态，重复通知不会延长当前释放时间
    bool ReportIssue(EDeliveryRecoveryIssue Issue, float ReleaseDuration = .45f, int32 ArmIndex = INDEX_NONE)
    {
        if (Issue == EDeliveryRecoveryIssue::ArmBlocked)
        {
            if (ArmIndex < 0 || ArmIndex > 1 || ArmWatch[ArmIndex] <= 0.f || ArmRemaining[ArmIndex] > 0.f) return false;
            ArmDuration[ArmIndex] = FMath::Max(.1f, ReleaseDuration);
            ArmRemaining[ArmIndex] = ArmDuration[ArmIndex];
            ArmWatch[ArmIndex] = ArmErrorTime[ArmIndex] = 0.f;
            return true;
        }
        if (ReleaseRemaining > 0.f || (Issue == EDeliveryRecoveryIssue::FeetCrossed &&
            (bCorrectingFeet || RetryRemaining > 0.f))) return false;
        ReleaseRemaining = FMath::Max(0.f, ReleaseDuration);
        if (Issue == EDeliveryRecoveryIssue::FeetCrossed)
        {
            bCorrectingFeet = true;
            CorrectionElapsed = FeetStableTime = 0.f;
        }
        return true;
    }

    // 计时使用与步态相同的物理步长，长帧不会跳过释放阶段
    void UpdateFeet(float DeltaTime)
    {
        ReleaseRemaining = FMath::Max(0.f, ReleaseRemaining - DeltaTime);
        RetryRemaining = FMath::Max(0.f, RetryRemaining - DeltaTime);
        if (bCorrectingFeet) CorrectionElapsed += DeltaTime;
    }

    // 两脚顺序持续颠倒才报告，正常转身的一帧交叉不触发恢复
    bool ObserveFeet(float Separation, float DeltaTime, float CrossDistance, float ConfirmTime,
                     float ClearDistance, float StableDuration, float MaxDuration)
    {
        if (bCorrectingFeet)
        {
            FeetStableTime = Separation >= ClearDistance ? FeetStableTime + DeltaTime : 0.f;
            if (FeetStableTime >= StableDuration || CorrectionElapsed >= MaxDuration)
            {
                bCorrectingFeet = false;
                CrossedTime = 0.f;
                RetryRemaining = .5f;
            }
            return false;
        }
        CrossedTime = Separation < -CrossDistance && RetryRemaining <= 0.f
            ? CrossedTime + DeltaTime : 0.f;
        return CrossedTime >= ConfirmTime && ReportIssue(EDeliveryRecoveryIssue::FeetCrossed, .12f);
    }

    // 中断交叉观察时保留原有卡脚释放，避免改变战斗期间的步态保护
    void CancelFootCorrection()
    {
        if (bCorrectingFeet) ReleaseRemaining = 0.f;
        CrossedTime = FeetStableTime = CorrectionElapsed = RetryRemaining = 0.f;
        bCorrectingFeet = false;
    }

    void ResetFeet()
    {
        ReleaseRemaining = CrossedTime = FeetStableTime = CorrectionElapsed = RetryRemaining = 0.f;
        bCorrectingFeet = false;
    }
    bool IsReleasingFeet() const { return ReleaseRemaining > 0.f; }
    bool IsCorrectingFeet() const { return bCorrectingFeet; }
    bool ShouldPauseWalking() const { return IsReleasingFeet() || bCorrectingFeet; }
    float GetReleaseRemaining() const { return ReleaseRemaining; }

    // 每次出拳结束只观察对应手臂，异常恢复最多执行一次
    void WatchArm(int32 Side)
    {
        if (Side < 0 || Side > 1 || ArmRemaining[Side] > 0.f) return;
        ArmWatch[Side] = 3.f;
        ArmErrorTime[Side] = 0.f;
    }
    void ResetArms()
    {
        for (int32 Side = 0; Side < 2; ++Side)
            ArmWatch[Side] = ArmErrorTime[Side] = ArmRemaining[Side] = 0.f;
    }
    void AdvanceArms(float DeltaTime)
    {
        for (int32 Side = 0; Side < 2; ++Side)
        {
            ArmWatch[Side] = FMath::Max(0.f, ArmWatch[Side] - DeltaTime);
            ArmRemaining[Side] = FMath::Max(0.f, ArmRemaining[Side] - DeltaTime);
        }
    }
    bool ObserveArm(int32 Side, bool bBlocked, float DeltaTime, float ConfirmTime, float Duration)
    {
        if (Side < 0 || Side > 1 || ArmWatch[Side] <= 0.f) return false;
        ArmErrorTime[Side] = bBlocked ? ArmErrorTime[Side] + DeltaTime : 0.f;
        return ArmErrorTime[Side] >= ConfirmTime && ReportIssue(EDeliveryRecoveryIssue::ArmBlocked, Duration, Side);
    }
    bool IsWatchingArm(int32 Side) const { return Side >= 0 && Side < 2 && ArmWatch[Side] > 0.f; }
    bool IsRecoveringArms() const { return ArmRemaining[0] > 0.f || ArmRemaining[1] > 0.f; }
    FVector2D GetArmProgress() const
    {
        return FVector2D(1.f - ArmRemaining[0] / ArmDuration[0], 1.f - ArmRemaining[1] / ArmDuration[1]);
    }

    // 倒地后的起身进度
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
    // 手臂恢复不暂停走路，计时与脚步状态相互独立
    float ArmWatch[2] = {};
    float ArmErrorTime[2] = {};
    float ArmRemaining[2] = {};
    float ArmDuration[2] = {1.f, 1.f};
    // 脚步释放与交叉补步
    float ReleaseRemaining = 0.f;
    float CrossedTime = 0.f;
    float FeetStableTime = 0.f;
    float CorrectionElapsed = 0.f;
    // 刚结束补步时不立即接受新的交叉请求
    float RetryRemaining = 0.f;
    bool bCorrectingFeet = false;
    // 倒地稳定与起身
    float StableTime = 0.f;
    float StandingTime = 0.f;
    float RiseAlpha = 1.f;
    float StartHeight = 0.f;
    bool bRising = false;
};
