#include "Speech/DeliverySpeechAudioCapture.h"

#include "AudioCaptureCore.h"
#include "AudioDevice.h"
#include "AudioDeviceHandle.h"
#include "HAL/PlatformTime.h"
#include "ISubmixBufferListener.h"
#include "Sound/SoundSubmix.h"

namespace
{
    class FDeliverySpeechSubmixListener final : public ISubmixBufferListener
    {
    public:
        explicit FDeliverySpeechSubmixListener(const TSharedRef<FDeliverySpeechAudioProcessor, ESPMode::ThreadSafe>& InProcessor)
            : Processor(InProcessor) {}

        virtual void OnNewSubmixBuffer(const USoundSubmix*, float* AudioData, int32 NumSamples,
                                      int32 NumChannels, const int32 SampleRate, double) override
        {
            if (auto Pinned = Processor.Pin())
            {
                if (NumChannels <= 0 || NumSamples % NumChannels != 0)
                    Pinned->PushRender(nullptr, 0, 0, 0, FPlatformTime::Seconds());
                else
                    Pinned->PushRender(AudioData, NumSamples / NumChannels, NumChannels, SampleRate, FPlatformTime::Seconds());
            }
        }

        virtual bool IsRenderingAudio() const override
        {
            // 静音时仍需要连续的播放参考，不能让主混音自动停掉回声处理时钟
            const auto Pinned = Processor.Pin();
            return Pinned && Pinned->IsRunning();
        }

        virtual const FString& GetListenerName() const override { return Name; }

    private:
        TWeakPtr<FDeliverySpeechAudioProcessor, ESPMode::ThreadSafe> Processor;
        const FString Name = TEXT("DeliverySpeechEchoReference");
    };
}

class FDeliverySpeechAudioCapture::FImpl
{
public:
    bool Start(const FAudioDeviceHandle& InDevice, FDeliverySpeechAudioCallbacks&& Callbacks,
               FString& OutError, int32 CaptureDeviceIndex)
    {
        check(IsInGameThread());
        Stop();
        if (!InDevice.IsValid()) { OutError = TEXT("OutputAudioDeviceUnavailable"); return false; }

        Capture = MakeUnique<Audio::FAudioCapture>();
        Audio::FCaptureDeviceInfo Info;
        if (!Capture->GetCaptureDeviceInfo(Info, CaptureDeviceIndex))
        {
            OutError = TEXT("MicrophoneUnavailable");
            Stop();
            return false;
        }

        Device = InDevice;
        Submix = &Device->GetMainSubmixObject();
        Processor = MakeShared<FDeliverySpeechAudioProcessor, ESPMode::ThreadSafe>();
        const float OutputRate = Device->GetSampleRate();
        const int32 OutputLatencyMs = OutputRate > 0
            ? FMath::CeilToInt(1000.0 * Device->GetBufferLength() * Device->GetNumBuffers() / OutputRate) : 0;
        if (!Processor->Start(MoveTemp(Callbacks), OutputLatencyMs, OutError))
        {
            Stop();
            return false;
        }

        Listener = MakeShared<FDeliverySpeechSubmixListener, ESPMode::ThreadSafe>(Processor.ToSharedRef());
        Device->RegisterSubmixBufferListener(Listener.ToSharedRef(), *Submix.Get());

        Audio::FAudioCaptureDeviceParams Params;
        Params.DeviceIndex = CaptureDeviceIndex;
        Params.PCMAudioEncoding = Audio::EPCMAudioEncoding::FLOATING_POINT_32;
        Params.bUseHardwareAEC = false;
        // 接受设备真实采样率和声道数，转换交给 APM 与识别器，不重新标记 PCM 采样率
        Params.SampleRate = Audio::InvalidDeviceSampleRate;
        Params.NumInputChannels = Audio::InvalidDeviceChannelCount;
        const TWeakPtr<FDeliverySpeechAudioProcessor, ESPMode::ThreadSafe> WeakProcessor = Processor;
        const bool bOpened = Capture->OpenAudioCaptureStream(Params,
            [WeakProcessor](const void* Samples, int32 Frames, int32 Channels, int32 Rate, double, bool bOverflow)
            {
                if (auto Pinned = WeakProcessor.Pin())
                    Pinned->PushCapture(static_cast<const float*>(Samples), Frames, Channels, Rate,
                                        FPlatformTime::Seconds(), bOverflow);
            }, 512);
        if (!bOpened || !Capture->StartStream())
        {
            OutError = bOpened ? TEXT("MicrophoneStartFailed") : TEXT("MicrophoneOpenFailed");
            Stop();
            return false;
        }
        OutError.Reset();
        return true;
    }

    void Stop()
    {
        check(IsInGameThread());
        // 先拒绝新音频并结束工作线程，设备的晚到回调只能看到已停止的普通 C++ 对象
        if (Processor) Processor->Stop();
        if (Capture)
        {
            if (Capture->IsStreamOpen()) Capture->AbortStream();
            Capture.Reset();
        }
        if (Device.IsValid() && Listener && Submix.IsValid())
            Device->UnregisterSubmixBufferListener(Listener.ToSharedRef(), *Submix.Get());
        Listener.Reset();
        Submix.Reset();
        Device.Reset();
        Processor.Reset();
    }

    FAudioDeviceHandle Device;
    TWeakObjectPtr<USoundSubmix> Submix;
    TSharedPtr<FDeliverySpeechSubmixListener, ESPMode::ThreadSafe> Listener;
    TSharedPtr<FDeliverySpeechAudioProcessor, ESPMode::ThreadSafe> Processor;
    TUniquePtr<Audio::FAudioCapture> Capture;
};

FDeliverySpeechAudioCapture::FDeliverySpeechAudioCapture() : Impl(MakeUnique<FImpl>()) {}
FDeliverySpeechAudioCapture::~FDeliverySpeechAudioCapture() { Stop(); }

bool FDeliverySpeechAudioCapture::Start(const FAudioDeviceHandle& Device, FDeliverySpeechAudioCallbacks&& Callbacks,
                                      FString& OutError, int32 CaptureDeviceIndex)
{
    return Impl->Start(Device, MoveTemp(Callbacks), OutError, CaptureDeviceIndex);
}
void FDeliverySpeechAudioCapture::Stop() { Impl->Stop(); }
bool FDeliverySpeechAudioCapture::IsRunning() const { return Impl->Processor && Impl->Processor->IsRunning(); }
bool FDeliverySpeechAudioCapture::IsReady() const { return Impl->Processor && Impl->Processor->IsReady(); }
bool FDeliverySpeechAudioCapture::EnableContinuousListening() { return Impl->Processor && Impl->Processor->EnableContinuousListening(); }
bool FDeliverySpeechAudioCapture::BeginDetectedTurn(uint64 DetectionId, uint64 TurnId)
{ return Impl->Processor && Impl->Processor->BeginDetectedTurn(DetectionId, TurnId); }
bool FDeliverySpeechAudioCapture::BeginTurn(uint64 TurnId) { return Impl->Processor && Impl->Processor->BeginTurn(TurnId); }
bool FDeliverySpeechAudioCapture::FinishTurn(uint64 TurnId) { return Impl->Processor && Impl->Processor->FinishTurn(TurnId); }
void FDeliverySpeechAudioCapture::CancelTurn(uint64 TurnId) { if (Impl->Processor) Impl->Processor->CancelTurn(TurnId); }
FDeliverySpeechAudioStatistics FDeliverySpeechAudioCapture::GetStatistics() const
{
    return Impl->Processor ? Impl->Processor->GetStatistics() : FDeliverySpeechAudioStatistics{};
}
