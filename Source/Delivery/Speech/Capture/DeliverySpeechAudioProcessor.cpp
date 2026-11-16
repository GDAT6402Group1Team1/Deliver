#include "Speech/Capture/DeliverySpeechAudioProcessor.h"
#include "Speech/Recognition/DeliverySpeechSegmentation.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"
#include <atomic>
#include <memory>

DEFINE_LOG_CATEGORY_STATIC(LogDeliverySpeechAudio, Log, All);

THIRD_PARTY_INCLUDES_START
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif
#include "PreWebRTCApi.h"
#include "api/audio/echo_canceller3_factory.h"
#include "modules/audio_processing/agc2/vad_wrapper.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "PostWebRTCApi.h"
#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif
THIRD_PARTY_INCLUDES_END

namespace DeliverySpeechAudio
{
    constexpr int32 QueueCapacity = 128;
    constexpr int32 MaxPacketSamples = 8192;
    constexpr int32 MaxChannels = 8;
    constexpr int32 MaxSampleRate = 96000;
    constexpr int32 MaxBlockFrames = MaxSampleRate / 100;
    constexpr double MaxQueueSeconds = .25;
    constexpr double MissingStreamSeconds = 1.0;

    enum class EInput : uint8 { Capture, Render, Begin, Finish, Cancel, CaptureStartDiscontinuity, EnableContinuous, AcceptDetected };
    enum class EFault : uint8 { None, CaptureDiscontinuity, Overflow, QueueStalled, InvalidFormat, ChangedFormat, InvalidSamples, Processing, MissingStream, ContinuousOverflow, VadProcessing };

    struct FInput
    {
        EInput Kind = EInput::Capture;
        uint64 TurnId = 0;
        uint64 DetectionId = 0;
        int32 Frames = 0;
        int32 Channels = 0;
        int32 SampleRate = 0;
        double ArrivedAt = 0;
        float Samples[MaxPacketSamples];

        void CopyFrom(const FInput& Other)
        {
            Kind = Other.Kind; TurnId = Other.TurnId; DetectionId = Other.DetectionId;
            Frames = Other.Frames; Channels = Other.Channels; SampleRate = Other.SampleRate;
            ArrivedAt = Other.ArrivedAt;
            if (Frames > 0) FMemory::Memcpy(Samples, Other.Samples, sizeof(float) * Frames * Channels);
        }
    };

    struct FFrame
    {
        int32 SampleRate = 0;
        int32 Channels = 0;
        int32 Count = 0;
        double ArrivedAt = 0;
        float Planes[MaxChannels][MaxBlockFrames];
        uint64 Turns[MaxBlockFrames];

        void Reset()
        {
            SampleRate = Channels = Count = 0;
            ArrivedAt = 0;
        }
    };

    const TCHAR* FaultText(EFault Fault)
    {
        switch (Fault)
        {
        case EFault::CaptureDiscontinuity: return TEXT("AudioCaptureDiscontinuity");
        case EFault::Overflow: return TEXT("AudioQueueOverflow");
        case EFault::QueueStalled: return TEXT("AudioQueueStalled");
        case EFault::InvalidFormat: return TEXT("AudioFormatUnsupported");
        case EFault::ChangedFormat: return TEXT("AudioDeviceFormatChanged");
        case EFault::InvalidSamples: return TEXT("AudioSamplesInvalid");
        case EFault::Processing: return TEXT("EchoProcessingFailed");
        case EFault::MissingStream: return TEXT("AudioStreamStopped");
        case EFault::ContinuousOverflow: return TEXT("ContinuousAudioBacklog");
        case EFault::VadProcessing: return TEXT("SpeechActivityDetectionFailed");
        default: return TEXT("AudioProcessingFailed");
        }
    }
}

class FDeliverySpeechAudioProcessor::FImpl final : public FRunnable
{
public:
    FImpl()
    {
        Queue.SetNum(DeliverySpeechAudio::QueueCapacity);
        Wake = FPlatformProcess::GetSynchEventFromPool(false);
    }

    ~FImpl()
    {
        Shutdown();
        FPlatformProcess::ReturnSynchEventToPool(Wake);
    }

    bool Start(FDeliverySpeechAudioCallbacks&& InCallbacks, int32 InOutputLatencyMs, FString& OutError)
    {
        Shutdown();
        if (!InCallbacks.OnAudio || !InCallbacks.OnFinished || !InCallbacks.OnError)
        {
            OutError = TEXT("AudioCallbacksMissing");
            return false;
        }
        Callbacks = MoveTemp(InCallbacks);
        OutputLatencyMs = FMath::Clamp(InOutputLatencyMs, 0, 500);
        ReadIndex = WriteIndex = Queued = 0;
        bReceivedCapturePacket = false;
        bContinuousRequested = bContinuous = false;
        Capture.Reset(); Render.Reset();
        ActiveTurn = PendingFinishTurn = FinishBoundary = TotalCaptureFrames = ProcessedCaptureFrames = 0;
        LastCaptureAt = LastRenderAt = 0;
        bProcessedCapture = bProcessedRender = false;
        RequestedTurn.store(0); bFinishQueued.store(false);
        Fault.store(DeliverySpeechAudio::EFault::None);
        bStopping.store(false); bAccepting.store(true); bReady.store(false);
        CaptureFrames.store(0); RenderFrames.store(0); ProcessedBlocks.store(0);
        LastProcessingMs.store(0); LastQueueMs.store(0); LastDelayMs.store(0);
        Thread.Reset(FRunnableThread::Create(this, TEXT("DeliverySpeechAEC"), 0, TPri_Normal));
        if (!Thread)
        {
            bAccepting.store(false);
            Callbacks = {};
            OutError = TEXT("AudioWorkerUnavailable");
            return false;
        }
        OutError.Reset();
        return true;
    }

    void Shutdown()
    {
        bAccepting.store(false);
        bReady.store(false);
        RequestedTurn.store(0);
        bStopping.store(true);
        Wake->Trigger();
        if (Thread)
        {
            checkf(FPlatformTLS::GetCurrentThreadId() != Thread->GetThreadID(),
                   TEXT("Stop speech audio from its owning thread, not from an audio callback"));
            Thread->WaitForCompletion();
            Thread.Reset();
        }
        FScopeLock Lock(&QueueMutex);
        Queued = ReadIndex = WriteIndex = 0;
        Callbacks = {};
    }

    virtual void Stop() override
    {
        bStopping.store(true);
        Wake->Trigger();
    }

    virtual uint32 Run() override
    {
        webrtc::AudioProcessing::Config Config;
        Config.pipeline.multi_channel_render = true;
        Config.pipeline.multi_channel_capture = false;
        Config.echo_canceller.enabled = true;
        Config.echo_canceller.mobile_mode = false;
        Config.high_pass_filter.enabled = true;
        Config.noise_suppression.enabled = false;
        Config.gain_controller1.enabled = false;
        Config.gain_controller2.enabled = false;
        webrtc::AudioProcessingBuilder Builder;
        Builder.SetConfig(Config);
        Builder.SetEchoControlFactory(std::make_unique<webrtc::EchoCanceller3Factory>());
        Apm = Builder.Create();
        if (!Apm) SetFault(DeliverySpeechAudio::EFault::Processing);

        const double StartedAt = FPlatformTime::Seconds();
        auto Input = MakeUnique<DeliverySpeechAudio::FInput>();
        while (!bStopping.load())
        {
            if (Fault.load() != DeliverySpeechAudio::EFault::None) break;
            if (Pop(*Input))
            {
                Process(*Input);
                continue;
            }
            const double Now = FPlatformTime::Seconds();
            const bool bMissing = bReady.load()
                ? Now - LastCaptureAt > DeliverySpeechAudio::MissingStreamSeconds ||
                  Now - LastRenderAt > DeliverySpeechAudio::MissingStreamSeconds
                : Now - StartedAt > 3.0;
            if (bMissing) SetFault(DeliverySpeechAudio::EFault::MissingStream);
            else Wake->Wait(10);
        }
        bAccepting.store(false);
        bReady.store(false);
        RequestedTurn.store(0);
        Segmenter.Reset();
        Vad.Reset();
        Apm = nullptr;
        const auto Error = Fault.load();
        if (!bStopping.load() && Error != DeliverySpeechAudio::EFault::None && Callbacks.OnError)
        {
            UE_LOG(LogDeliverySpeechAudio, Warning,
                TEXT("SpeechAudioStopped reason=%s capture_frames=%llu render_frames=%llu apm_blocks=%llu"),
                DeliverySpeechAudio::FaultText(Error), CaptureFrames.load(), RenderFrames.load(), ProcessedBlocks.load());
            Callbacks.OnError(0, DeliverySpeechAudio::FaultText(Error));
        }
        return 0;
    }

    bool PushAudio(DeliverySpeechAudio::EInput Kind, const float* Samples, int32 NumFrames,
                   int32 NumChannels, int32 SampleRate, double ArrivedAt, bool bOverflow)
    {
        if (!bAccepting.load()) return false;
        if (!Samples || NumFrames <= 0 || NumChannels < 1 || NumChannels > DeliverySpeechAudio::MaxChannels ||
            SampleRate < 8000 || SampleRate > DeliverySpeechAudio::MaxSampleRate || SampleRate % 100 != 0 ||
            !FMath::IsFinite(ArrivedAt) || ArrivedAt <= 0)
        {
            SetFault(DeliverySpeechAudio::EFault::InvalidFormat);
            return false;
        }
        const int32 FramesPerPacket = DeliverySpeechAudio::MaxPacketSamples / NumChannels;
        const int64 PacketCount = (int64(NumFrames) + FramesPerPacket - 1) / FramesPerPacket;
        {
            // 这把锁只保护固定容量复制与索引，不在锁内调用 AEC、回调或设备接口
            FScopeLock Lock(&QueueMutex);
            if (!bAccepting.load()) return false;
            const bool bFirstCapture = Kind == DeliverySpeechAudio::EInput::Capture && !bReceivedCapturePacket;
            if (Kind == DeliverySpeechAudio::EInput::Capture) bReceivedCapturePacket = true;
            if (bOverflow)
            {
                // WASAPI 将 DATA_DISCONTINUITY 传为 bOverflow；仅首包且尚无录入轮次可重新预热
                // 丢弃这个包，在串行工作线程重置 APM，之后任何不连续仍使会话失效
                if (bFirstCapture && !RequestedTurn.load() && !bReady.load())
                    return PushCommandLocked(DeliverySpeechAudio::EInput::CaptureStartDiscontinuity, 0);
                SetFault(DeliverySpeechAudio::EFault::CaptureDiscontinuity);
                return false;
            }
            if (PacketCount > DeliverySpeechAudio::QueueCapacity - Queued)
            {
                SetFault(DeliverySpeechAudio::EFault::Overflow);
                return false;
            }
            for (int32 Offset = 0; Offset < NumFrames;)
            {
                auto& Item = Queue[WriteIndex];
                Item.Kind = Kind; Item.TurnId = 0;
                Item.Frames = FMath::Min(FramesPerPacket, NumFrames - Offset);
                Item.Channels = NumChannels; Item.SampleRate = SampleRate; Item.ArrivedAt = ArrivedAt;
                FMemory::Memcpy(Item.Samples, Samples + Offset * NumChannels,
                                sizeof(float) * Item.Frames * NumChannels);
                AdvanceWrite();
                Offset += Item.Frames;
            }
        }
        Wake->Trigger();
        return true;
    }

    bool BeginTurn(uint64 TurnId)
    {
        if (!TurnId || !bReady.load()) return false;
        FScopeLock Lock(&QueueMutex);
        if (!bAccepting.load() || bContinuousRequested || RequestedTurn.load() != 0) return false;
        if (!PushCommandLocked(DeliverySpeechAudio::EInput::Begin, TurnId)) return false;
        bFinishQueued.store(false);
        RequestedTurn.store(TurnId);
        return true;
    }

    bool EnableContinuousListening()
    {
        FScopeLock Lock(&QueueMutex);
        if (!bAccepting.load() || !bReady.load() || !Callbacks.OnSpeechDetected || RequestedTurn.load()) return false;
        if (bContinuousRequested) return true;
        if (!PushCommandLocked(DeliverySpeechAudio::EInput::EnableContinuous, 0)) return false;
        bContinuousRequested = true;
        return true;
    }

    bool BeginDetectedTurn(uint64 DetectionId, uint64 TurnId)
    {
        FScopeLock Lock(&QueueMutex);
        if (!DetectionId || !TurnId || !bAccepting.load() || !bContinuousRequested || RequestedTurn.load()) return false;
        if (!PushCommandLocked(DeliverySpeechAudio::EInput::AcceptDetected, TurnId, DetectionId)) return false;
        RequestedTurn.store(TurnId);
        bFinishQueued.store(false);
        return true;
    }

    bool FinishTurn(uint64 TurnId)
    {
        FScopeLock Lock(&QueueMutex);
        if (!TurnId || !bAccepting.load() || bContinuousRequested || RequestedTurn.load() != TurnId || bFinishQueued.load()) return false;
        if (!PushCommandLocked(DeliverySpeechAudio::EInput::Finish, TurnId)) return false;
        bFinishQueued.store(true);
        return true;
    }

    void CancelTurn(uint64 TurnId)
    {
        FScopeLock Lock(&QueueMutex);
        if (!TurnId || RequestedTurn.load() != TurnId) return;
        // 先关闭旧轮输出，已经送往识别队列的样本仍由调用方按轮次作废
        RequestedTurn.store(0);
        bFinishQueued.store(false);
        if (bAccepting.load()) PushCommandLocked(DeliverySpeechAudio::EInput::Cancel, TurnId);
    }

    bool IsRunning() const { return bAccepting.load(); }
    bool IsReady() const { return bReady.load() && bAccepting.load(); }

    FDeliverySpeechAudioStatistics GetStatistics() const
    {
        FDeliverySpeechAudioStatistics Result;
        Result.CaptureFrames = CaptureFrames.load();
        Result.RenderFrames = RenderFrames.load();
        Result.ProcessedBlocks = ProcessedBlocks.load();
        Result.LastProcessingMilliseconds = LastProcessingMs.load();
        Result.LastQueueMilliseconds = LastQueueMs.load();
        Result.EstimatedStreamDelayMilliseconds = LastDelayMs.load();
        return Result;
    }

private:
    void AdvanceWrite()
    {
        WriteIndex = (WriteIndex + 1) % DeliverySpeechAudio::QueueCapacity;
        ++Queued;
    }

    bool PushCommandLocked(DeliverySpeechAudio::EInput Kind, uint64 TurnId, uint64 DetectionId = 0)
    {
        if (Queued == DeliverySpeechAudio::QueueCapacity)
        {
            SetFault(DeliverySpeechAudio::EFault::Overflow);
            return false;
        }
        auto& Item = Queue[WriteIndex];
        Item.Kind = Kind; Item.TurnId = TurnId; Item.DetectionId = DetectionId;
        Item.Frames = Item.Channels = Item.SampleRate = 0;
        Item.ArrivedAt = FPlatformTime::Seconds();
        AdvanceWrite();
        Wake->Trigger();
        return true;
    }

    bool Pop(DeliverySpeechAudio::FInput& Out)
    {
        FScopeLock Lock(&QueueMutex);
        if (!Queued) return false;
        Out.CopyFrom(Queue[ReadIndex]);
        ReadIndex = (ReadIndex + 1) % DeliverySpeechAudio::QueueCapacity;
        --Queued;
        return true;
    }

    void SetFault(DeliverySpeechAudio::EFault Value)
    {
        auto Expected = DeliverySpeechAudio::EFault::None;
        Fault.compare_exchange_strong(Expected, Value);
        bAccepting.store(false);
        bReady.store(false);
        Wake->Trigger();
    }

    void Process(const DeliverySpeechAudio::FInput& Input)
    {
        using namespace DeliverySpeechAudio;
        if (Input.Kind == EInput::EnableContinuous)
        {
            FDeliverySpeechSegmentCallbacks Events;
            Events.OnDetected = [this](uint64 Id)
            {
                UE_LOG(LogDeliverySpeechAudio, Display, TEXT("SpeechVoiceActivity detected=%llu"), Id);
                Callbacks.OnSpeechDetected(Id);
            };
            Events.OnAudio = [this](uint64 Turn, int32 Rate, TArray<float>&& Samples)
            {
                if (RequestedTurn.load() == Turn && !bStopping.load())
                    Callbacks.OnAudio(Turn, Rate, MoveTemp(Samples));
            };
            Events.OnFinished = [this](uint64 Turn)
            {
                uint64 Expected = Turn;
                if (RequestedTurn.compare_exchange_strong(Expected, 0) && !bStopping.load())
                {
                    UE_LOG(LogDeliverySpeechAudio, Display, TEXT("SpeechSegmentFinished turn=%llu"), Turn);
                    Callbacks.OnFinished(Turn);
                }
            };
            Segmenter.Initialize(Capture.SampleRate, MoveTemp(Events));
            Vad = MakeUnique<webrtc::VoiceActivityDetectorWrapper>(10000, webrtc::GetAvailableCpuFeatures(), Capture.SampleRate);
            bContinuous = true;
            UE_LOG(LogDeliverySpeechAudio, Display,
                TEXT("SpeechContinuousReady preroll_ms=%d start_ms=%d silence_ms=%d segment_limit_ms=%d pending_limit_ms=%d"),
                FDeliverySpeechSegmentation::PreRollMilliseconds, FDeliverySpeechSegmentation::StartVoiceMilliseconds,
                FDeliverySpeechSegmentation::EndSilenceMilliseconds, FDeliverySpeechSegmentation::SegmentSoftLimitMilliseconds,
                FDeliverySpeechSegmentation::PendingAudioLimitMilliseconds);
            return;
        }
        if (Input.Kind == EInput::AcceptDetected)
        {
            if (!Segmenter.Accept(Input.DetectionId, Input.TurnId)) SetFault(EFault::ContinuousOverflow);
            return;
        }
        if (Input.Kind == EInput::CaptureStartDiscontinuity)
        {
            Capture.Reset(); Render.Reset();
            bProcessedCapture = bProcessedRender = false;
            bReady.store(false);
            LastCaptureAt = LastRenderAt = 0;
            TotalCaptureFrames = ProcessedCaptureFrames = 0;
            const int32 Status = Apm->Initialize();
            UE_LOG(LogDeliverySpeechAudio, Display,
                TEXT("SpeechAudioWarmup reason=InitialCaptureDiscontinuity action=DiscardPacketAndReset apm_status=%d"), Status);
            if (Status != webrtc::AudioProcessing::kNoError) SetFault(EFault::Processing);
            return;
        }
        if (Input.Kind == EInput::Begin)
        {
            ActiveTurn = Input.TurnId;
            return;
        }
        if (Input.Kind == EInput::Cancel)
        {
            if (bContinuous) Segmenter.Cancel(Input.TurnId);
            if (ActiveTurn == Input.TurnId) ActiveTurn = 0;
            if (PendingFinishTurn == Input.TurnId) PendingFinishTurn = 0;
            return;
        }
        if (Input.Kind == EInput::Finish)
        {
            if (ActiveTurn != Input.TurnId) return;
            ActiveTurn = 0;
            PendingFinishTurn = Input.TurnId;
            FinishBoundary = TotalCaptureFrames;
            MaybeFinishTurn();
            return;
        }
        const double QueueSeconds = FPlatformTime::Seconds() - Input.ArrivedAt;
        LastQueueMs.store(FMath::Max(0.0, QueueSeconds) * 1000);
        if (QueueSeconds > MaxQueueSeconds) { SetFault(EFault::QueueStalled); return; }
        auto& Frame = Input.Kind == EInput::Capture ? Capture : Render;
        if (!Frame.SampleRate) { Frame.SampleRate = Input.SampleRate; Frame.Channels = Input.Channels; }
        else if (Frame.SampleRate != Input.SampleRate || Frame.Channels != Input.Channels)
        {
            UE_LOG(LogDeliverySpeechAudio, Warning,
                TEXT("SpeechAudioFormatChanged capture=%d previous_rate=%d previous_channels=%d rate=%d channels=%d"),
                Input.Kind == EInput::Capture, Frame.SampleRate, Frame.Channels, Input.SampleRate, Input.Channels);
            SetFault(EFault::ChangedFormat);
            return;
        }
        const bool bCapture = Input.Kind == EInput::Capture;
        if (bCapture) { LastCaptureAt = Input.ArrivedAt; CaptureFrames.fetch_add(Input.Frames); }
        else { LastRenderAt = Input.ArrivedAt; RenderFrames.fetch_add(Input.Frames); }
        const int32 BlockFrames = Frame.SampleRate / 100;
        for (int32 Index = 0; Index < Input.Frames; ++Index)
        {
            if (Frame.Count == 0) Frame.ArrivedAt = Input.ArrivedAt;
            for (int32 Channel = 0; Channel < Frame.Channels; ++Channel)
            {
                const float Value = Input.Samples[Index * Frame.Channels + Channel];
                if (!FMath::IsFinite(Value)) { SetFault(EFault::InvalidSamples); return; }
                Frame.Planes[Channel][Frame.Count] = FMath::Clamp(Value, -1.f, 1.f);
            }
            if (bCapture)
            {
                Frame.Turns[Frame.Count] = ActiveTurn;
                ++TotalCaptureFrames;
            }
            if (++Frame.Count == BlockFrames)
            {
                if (!ProcessFrame(Frame, bCapture)) return;
                Frame.Count = 0;
            }
        }
    }

    bool ProcessFrame(DeliverySpeechAudio::FFrame& Frame, bool bCapture)
    {
        const double StartedAt = FPlatformTime::Seconds();
        const int32 Frames = Frame.SampleRate / 100;
        const float* Source[DeliverySpeechAudio::MaxChannels];
        float* Destination[DeliverySpeechAudio::MaxChannels];
        for (int32 Channel = 0; Channel < Frame.Channels; ++Channel)
        {
            Source[Channel] = Frame.Planes[Channel];
            Destination[Channel] = Frame.Planes[Channel];
        }
        const webrtc::StreamConfig InputConfig(Frame.SampleRate, Frame.Channels);
        int32 Status = 0;
        if (!bCapture)
        {
            Status = Apm->ProcessReverseStream(Source, InputConfig, InputConfig, Destination);
            bProcessedRender = Status == webrtc::AudioProcessing::kNoError;
        }
        else
        {
            // 回调到达时间和已知缓冲只能估计流延迟，AEC3 继续使用内部延迟估计
            const double CaptureQueueMs = FMath::Max(0.0, StartedAt - Frame.ArrivedAt) * 1000;
            const int32 DelayMs = FMath::Clamp(FMath::RoundToInt(OutputLatencyMs + CaptureQueueMs + 10), 0, 500);
            LastDelayMs.store(DelayMs);
            Status = Apm->set_stream_delay_ms(DelayMs);
            float* OutputChannels[] = { CleanMono };
            if (Status == webrtc::AudioProcessing::kNoError)
                Status = Apm->ProcessStream(Source, InputConfig, webrtc::StreamConfig(Frame.SampleRate, 1), OutputChannels);
            bProcessedCapture = Status == webrtc::AudioProcessing::kNoError;
            if (bProcessedCapture)
            {
                if (bContinuous)
                {
                    // AGC2 的独立 RNN VAD 接收 FloatS16，识别器仍保留归一化 PCM
                    for (int32 Index = 0; Index < Frames; ++Index)
                        VadSamples[Index] = FMath::Clamp(CleanMono[Index] * 32768.f, -32768.f, 32767.f);
                    const float* Channels[] = { VadSamples };
                    const float Probability = Vad->Analyze(webrtc::AudioFrameView<const float>(Channels, 1, Frames));
                    if (!FMath::IsFinite(Probability)) { SetFault(DeliverySpeechAudio::EFault::VadProcessing); return false; }
                    if (!Segmenter.PushFrame(CleanMono, Frames, Probability))
                    { SetFault(DeliverySpeechAudio::EFault::ContinuousOverflow); return false; }
                }
                else
                {
                    // 一帧可能跨过按下或松键，只输出属于该轮的连续样本，保留首尾不足 10 ms 的部分
                    for (int32 Start = 0; Start < Frames;)
                    {
                        const uint64 TurnId = Frame.Turns[Start];
                        int32 End = Start + 1;
                        while (End < Frames && Frame.Turns[End] == TurnId) ++End;
                        if (TurnId && TurnId == RequestedTurn.load() && !bStopping.load())
                        {
                            TArray<float> Samples;
                            Samples.Append(CleanMono + Start, End - Start);
                            Callbacks.OnAudio(TurnId, Frame.SampleRate, MoveTemp(Samples));
                        }
                        Start = End;
                    }
                }
                ProcessedCaptureFrames += Frames;
                MaybeFinishTurn();
            }
        }
        LastProcessingMs.store((FPlatformTime::Seconds() - StartedAt) * 1000);
        ProcessedBlocks.fetch_add(1);
        if (Status != webrtc::AudioProcessing::kNoError)
        {
            UE_LOG(LogDeliverySpeechAudio, Warning,
                TEXT("SpeechApmFailed capture=%d rate=%d channels=%d status=%d"),
                bCapture, Frame.SampleRate, Frame.Channels, Status);
            SetFault(DeliverySpeechAudio::EFault::Processing);
            return false;
        }
        bReady.store(bProcessedCapture && bProcessedRender);
        return true;
    }

    void MaybeFinishTurn()
    {
        if (!PendingFinishTurn || ProcessedCaptureFrames < FinishBoundary) return;
        const uint64 FinishedTurn = PendingFinishTurn;
        PendingFinishTurn = 0;
        bool bNotify = false;
        {
            // 与下一轮的按下和松键一起更新，避免旧轮完成覆盖新轮的结束标记
            FScopeLock Lock(&QueueMutex);
            uint64 Expected = FinishedTurn;
            bNotify = RequestedTurn.compare_exchange_strong(Expected, 0);
            if (bNotify) bFinishQueued.store(false);
        }
        if (bNotify && !bStopping.load()) Callbacks.OnFinished(FinishedTurn);
    }

    FCriticalSection QueueMutex;
    TArray<DeliverySpeechAudio::FInput> Queue;
    int32 ReadIndex = 0;
    int32 WriteIndex = 0;
    int32 Queued = 0;
    // 仅在 QueueMutex 内访问，Start 在设备回调注册前清零
    bool bReceivedCapturePacket = false;
    bool bContinuousRequested = false;
    bool bContinuous = false;
    FEvent* Wake = nullptr;
    TUniquePtr<FRunnableThread> Thread;
    FDeliverySpeechAudioCallbacks Callbacks;
    rtc::scoped_refptr<webrtc::AudioProcessing> Apm;
    TUniquePtr<webrtc::VoiceActivityDetectorWrapper> Vad;
    FDeliverySpeechSegmentation Segmenter;
    DeliverySpeechAudio::FFrame Capture;
    DeliverySpeechAudio::FFrame Render;
    float CleanMono[DeliverySpeechAudio::MaxBlockFrames];
    float VadSamples[DeliverySpeechAudio::MaxBlockFrames];
    int32 OutputLatencyMs = 0;
    uint64 ActiveTurn = 0;
    uint64 PendingFinishTurn = 0;
    uint64 FinishBoundary = 0;
    uint64 TotalCaptureFrames = 0;
    uint64 ProcessedCaptureFrames = 0;
    double LastCaptureAt = 0;
    double LastRenderAt = 0;
    bool bProcessedCapture = false;
    bool bProcessedRender = false;
    std::atomic<bool> bAccepting{false};
    std::atomic<bool> bStopping{false};
    std::atomic<bool> bReady{false};
    std::atomic<bool> bFinishQueued{false};
    std::atomic<uint64> RequestedTurn{0};
    std::atomic<DeliverySpeechAudio::EFault> Fault{DeliverySpeechAudio::EFault::None};
    std::atomic<uint64> CaptureFrames{0};
    std::atomic<uint64> RenderFrames{0};
    std::atomic<uint64> ProcessedBlocks{0};
    std::atomic<double> LastProcessingMs{0};
    std::atomic<double> LastQueueMs{0};
    std::atomic<int32> LastDelayMs{0};
};

FDeliverySpeechAudioProcessor::FDeliverySpeechAudioProcessor() : Impl(MakeUnique<FImpl>()) {}
FDeliverySpeechAudioProcessor::~FDeliverySpeechAudioProcessor() = default;

bool FDeliverySpeechAudioProcessor::Start(FDeliverySpeechAudioCallbacks&& Callbacks, int32 OutputLatencyMs, FString& OutError)
{
    return Impl->Start(MoveTemp(Callbacks), OutputLatencyMs, OutError);
}
void FDeliverySpeechAudioProcessor::Stop() { Impl->Shutdown(); }
bool FDeliverySpeechAudioProcessor::IsRunning() const { return Impl->IsRunning(); }
bool FDeliverySpeechAudioProcessor::IsReady() const { return Impl->IsReady(); }
bool FDeliverySpeechAudioProcessor::EnableContinuousListening() { return Impl->EnableContinuousListening(); }
bool FDeliverySpeechAudioProcessor::BeginDetectedTurn(uint64 DetectionId, uint64 TurnId)
{ return Impl->BeginDetectedTurn(DetectionId, TurnId); }
bool FDeliverySpeechAudioProcessor::BeginTurn(uint64 TurnId) { return Impl->BeginTurn(TurnId); }
bool FDeliverySpeechAudioProcessor::FinishTurn(uint64 TurnId) { return Impl->FinishTurn(TurnId); }
void FDeliverySpeechAudioProcessor::CancelTurn(uint64 TurnId) { Impl->CancelTurn(TurnId); }
bool FDeliverySpeechAudioProcessor::PushCapture(const float* Samples, int32 NumFrames, int32 NumChannels, int32 SampleRate,
                                               double ArrivedAtSeconds, bool bOverflow)
{
    return Impl->PushAudio(DeliverySpeechAudio::EInput::Capture, Samples, NumFrames, NumChannels, SampleRate, ArrivedAtSeconds, bOverflow);
}
bool FDeliverySpeechAudioProcessor::PushRender(const float* Samples, int32 NumFrames, int32 NumChannels, int32 SampleRate, double ArrivedAtSeconds)
{
    return Impl->PushAudio(DeliverySpeechAudio::EInput::Render, Samples, NumFrames, NumChannels, SampleRate, ArrivedAtSeconds, false);
}
FDeliverySpeechAudioStatistics FDeliverySpeechAudioProcessor::GetStatistics() const { return Impl->GetStatistics(); }
