#pragma once

#include "CoreMinimal.h"
#include "Ragdoll/Network/DeliveryRagdollSnapshot.h"

// 管理快照缓存、采样时机和恢复交接，不直接操作刚体或步态
class FDeliveryRagdollSync
{
public:
    void Reset() { *this = FDeliveryRagdollSync(); }

    bool ShouldCapture(float DeltaTime, float Rate)
    {
        CaptureElapsed += DeltaTime;
        if (CaptureElapsed < 1.f / FMath::Max(Rate, 1.f)) return false;
        CaptureElapsed = 0.f;
        return true;
    }

    // 返回是否收到了恢复交接所等待的新快照
    bool Receive(const FDeliveryRagdollSnapshot& Snapshot, float Now)
    {
        Previous = bHasSnapshot ? Target : Snapshot;
        Target = Snapshot;
        ReceivedAt = Now;
        bHasSnapshot = true;
        return bAwaitingResume && Target.Sequence != HandoffSequence;
    }

    const FDeliveryRagdollSnapshot& GetPrevious() const { return Previous; }
    const FDeliveryRagdollSnapshot& GetTarget() const { return Target; }
    bool HasSnapshot() const { return bHasSnapshot; }
    float GetReceivedAt() const { return ReceivedAt; }

    // 同一快照只允许修正一次，避免重复叠加校正速度
    bool ConsumeOwnerCorrection()
    {
        if (bHasCorrection && LastCorrectionSequence == Target.Sequence) return false;
        LastCorrectionSequence = Target.Sequence;
        bHasCorrection = true;
        return true;
    }
    void ResetOwnerCorrection() { bHasCorrection = false; }

    bool IsPlayingRecovery() const { return bRecoveryPlayback; }
    bool IsAwaitingResume() const { return bAwaitingResume; }
    void SetRecoveryPlayback(bool bEnabled) { bRecoveryPlayback = bEnabled; }
    void ResetRecovery() { bRecoveryPlayback = false; bAwaitingResume = false; }
    void WaitForActiveSnapshot()
    {
        bRecoveryPlayback = true;
        bAwaitingResume = true;
        HandoffSequence = Target.Sequence;
    }

private:
    FDeliveryRagdollSnapshot Previous;
    FDeliveryRagdollSnapshot Target;
    float CaptureElapsed = 0.f;
    float ReceivedAt = 0.f;
    bool bHasSnapshot = false;
    uint16 LastCorrectionSequence = 0;
    bool bHasCorrection = false;
    bool bRecoveryPlayback = false;
    bool bAwaitingResume = false;
    uint16 HandoffSequence = 0;
};
