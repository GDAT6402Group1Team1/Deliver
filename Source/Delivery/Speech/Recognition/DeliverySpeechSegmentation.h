#pragma once

#include "CoreMinimal.h"

struct FDeliverySpeechSegmentCallbacks
{
    TFunction<void(uint64 DetectionId)> OnDetected;
    TFunction<void(uint64 TurnId, int32 Rate, TArray<float>&&)> OnAudio;
    TFunction<void(uint64 TurnId)> OnFinished;
};

/**
 * @brief AEC 工作线程按 10ms 推进；VAD 提供概率，这里只负责分句、预卷和有界接续
 */
class FDeliverySpeechSegmentation
{
public:
    static constexpr int32 PreRollMilliseconds = 250;
    static constexpr int32 StartVoiceMilliseconds = 100;
    static constexpr int32 EndSilenceMilliseconds = 800;
    /// 包括预卷，给游戏线程和服务器的 15s 录入期限留出余量
    static constexpr int32 SegmentSoftLimitMilliseconds = 14000;
    static constexpr int32 PendingAudioLimitMilliseconds = 2000;

    void Initialize(int32 InRate, FDeliverySpeechSegmentCallbacks&& InCallbacks)
    {
        Reset();
        Rate = InRate;
        BlockSamples = Rate / 100;
        History.SetNumZeroed(Rate * PreRollMilliseconds / 1000);
        Callbacks = MoveTemp(InCallbacks);
    }

    void Reset()
    {
        Segments.Reset(); History.Reset(); Callbacks = {};
        Rate = BlockSamples = HistoryWrite = HistoryCount = PendingSamples = 0;
        StartFrames = SilenceFrames = 0;
        NextDetection = 0;
        bContinueNextFrame = bSuppressUntilSilence = false;
    }

    /// @brief false 表示输入或缓存失败，所属会话必须显式停止，不能丢弃一段后假装正常
    bool PushFrame(const float* Samples, int32 Count, float SpeechProbability)
    {
        if (!Samples || Count != BlockSamples || !Rate || !FMath::IsFinite(SpeechProbability)) return false;
        const bool bVoice = SpeechProbability >= .65f;
        const bool bMayContinue = SpeechProbability >= .35f;
        if (bSuppressUntilSilence)
        {
            SilenceFrames = bMayContinue ? 0 : SilenceFrames + 1;
            if (SilenceFrames >= EndSilenceMilliseconds / 10)
            {
                bSuppressUntilSilence = false;
                SilenceFrames = 0;
            }
            return true;
        }

        if (!Segments.IsEmpty() && !Segments.Last().bEnded)
        {
            auto& Segment = Segments.Last();
            if (!DeliverOrBuffer(Segment, Samples, Count)) return false;
            Segment.SampleCount += Count;
            SilenceFrames = bMayContinue ? 0 : SilenceFrames + 1;
            const bool bAtLimit = Segment.SampleCount >= Rate * SegmentSoftLimitMilliseconds / 1000;
            if (bAtLimit || SilenceFrames >= EndSilenceMilliseconds / 10)
            {
                Segment.bEnded = true;
                bContinueNextFrame = bAtLimit && bMayContinue;
                HistoryCount = HistoryWrite = StartFrames = SilenceFrames = 0;
                if (Segment.TurnId)
                {
                    const uint64 Turn = Segment.TurnId;
                    Segments.Pop(EAllowShrinking::No);
                    Callbacks.OnFinished(Turn);
                }
            }
            return true;
        }

        AppendHistory(Samples, Count);
        StartFrames = bVoice ? StartFrames + 1 : 0;
        if (!bContinueNextFrame && StartFrames < StartVoiceMilliseconds / 10) return true;
        /// @brief 最多保留两段尚未交给识别的内容，另有总音频时长限制
        if (Segments.Num() >= 2) return false;
        auto& Segment = Segments.AddDefaulted_GetRef();
        Segment.DetectionId = ++NextDetection;
        const int32 First = (HistoryWrite + History.Num() - HistoryCount) % History.Num();
        const int32 FirstCount = FMath::Min(HistoryCount, History.Num() - First);
        Segment.Pending.Append(History.GetData() + First, FirstCount);
        Segment.Pending.Append(History.GetData(), HistoryCount - FirstCount);
        Segment.SampleCount = HistoryCount;
        PendingSamples += HistoryCount;
        if (PendingSamples > Rate * PendingAudioLimitMilliseconds / 1000) return false;
        HistoryCount = HistoryWrite = StartFrames = SilenceFrames = 0;
        bContinueNextFrame = false;
        Callbacks.OnDetected(Segment.DetectionId);
        return true;
    }

    /// @brief 游戏线程已领取上一句 final 后，经音频队列为最早的待识别段绑定轮次
    bool Accept(uint64 DetectionId, uint64 TurnId)
    {
        if (!TurnId || Segments.IsEmpty() || Segments[0].DetectionId != DetectionId || Segments[0].TurnId) return false;
        auto& Segment = Segments[0];
        Segment.TurnId = TurnId;
        for (int32 Offset = 0; Offset < Segment.Pending.Num();)
        {
            const int32 Count = FMath::Min(Rate / 10, Segment.Pending.Num() - Offset);
            TArray<float> Audio;
            Audio.Append(Segment.Pending.GetData() + Offset, Count);
            Callbacks.OnAudio(TurnId, Rate, MoveTemp(Audio));
            Offset += Count;
        }
        PendingSamples -= Segment.Pending.Num();
        Segment.Pending.Reset();
        if (Segment.bEnded)
        {
            Segments.RemoveAt(0, 1, EAllowShrinking::No);
            Callbacks.OnFinished(TurnId);
        }
        return true;
    }

    void Cancel(uint64 TurnId)
    {
        const int32 Index = Segments.IndexOfByPredicate([TurnId](const FSegment& Segment) { return Segment.TurnId == TurnId; });
        if (!TurnId || Index == INDEX_NONE) return;
        const bool bWasSpeaking = !Segments[Index].bEnded;
        PendingSamples -= Segments[Index].Pending.Num();
        Segments.RemoveAt(Index, 1, EAllowShrinking::No);
        if (bWasSpeaking)
        {
            /// 拒绝或取消后等待这次说话结束，避免同一句不断重新申请交谈
            bSuppressUntilSilence = true;
            HistoryCount = HistoryWrite = StartFrames = SilenceFrames = 0;
            bContinueNextFrame = false;
        }
    }

    int32 GetPendingSamples() const { return PendingSamples; }

private:
    struct FSegment
    {
        uint64 DetectionId = 0;
        uint64 TurnId = 0;
        int32 SampleCount = 0;
        bool bEnded = false;
        TArray<float> Pending;
    };

    void AppendHistory(const float* Samples, int32 Count)
    {
        for (int32 Index = 0; Index < Count; ++Index)
        {
            History[HistoryWrite] = Samples[Index];
            HistoryWrite = (HistoryWrite + 1) % History.Num();
        }
        HistoryCount = FMath::Min(History.Num(), HistoryCount + Count);
    }

    bool DeliverOrBuffer(FSegment& Segment, const float* Samples, int32 Count)
    {
        if (Segment.TurnId)
        {
            TArray<float> Audio;
            Audio.Append(Samples, Count);
            Callbacks.OnAudio(Segment.TurnId, Rate, MoveTemp(Audio));
            return true;
        }
        if (PendingSamples + Count > Rate * PendingAudioLimitMilliseconds / 1000) return false;
        Segment.Pending.Append(Samples, Count);
        PendingSamples += Count;
        return true;
    }

    FDeliverySpeechSegmentCallbacks Callbacks;
    TArray<FSegment> Segments;
    TArray<float> History;
    int32 Rate = 0;
    int32 BlockSamples = 0;
    int32 HistoryWrite = 0;
    int32 HistoryCount = 0;
    int32 PendingSamples = 0;
    int32 StartFrames = 0;
    int32 SilenceFrames = 0;
    uint64 NextDetection = 0;
    bool bContinueNextFrame = false;
    bool bSuppressUntilSilence = false;
};
