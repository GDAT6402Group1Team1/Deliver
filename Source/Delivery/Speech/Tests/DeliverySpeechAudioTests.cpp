#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Speech/DeliverySpeechAudioProcessor.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include <atomic>
#include "Audio.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
    struct FSpeechAudioProbe
    {
        FCriticalSection Mutex;
        TMap<uint64, int32> SamplesByTurn;
        TArray<uint64> FinishedTurns;
        TArray<FString> Errors;
        bool bNonFiniteSample = false;
        bool bAudioAfterFinish = false;

        FDeliverySpeechAudioCallbacks MakeCallbacks()
        {
            FDeliverySpeechAudioCallbacks Result;
            Result.OnAudio = [this](uint64 TurnId, int32 SampleRate, TArray<float>&& Samples)
            {
                FScopeLock Lock(&Mutex);
                SamplesByTurn.FindOrAdd(TurnId) += Samples.Num();
                bAudioAfterFinish |= FinishedTurns.Contains(TurnId);
                bNonFiniteSample |= SampleRate != 48000;
                for (float Sample : Samples) bNonFiniteSample |= !FMath::IsFinite(Sample);
            };
            Result.OnFinished = [this](uint64 TurnId)
            {
                FScopeLock Lock(&Mutex);
                FinishedTurns.Add(TurnId);
            };
            Result.OnError = [this](uint64, FString Error)
            {
                FScopeLock Lock(&Mutex);
                Errors.Add(MoveTemp(Error));
            };
            return Result;
        }

        bool HasFinished(uint64 TurnId)
        {
            FScopeLock Lock(&Mutex);
            return FinishedTurns.Contains(TurnId);
        }
        bool HasError()
        {
            FScopeLock Lock(&Mutex);
            return !Errors.IsEmpty();
        }
    };

    bool WaitForAudio(TFunctionRef<bool()> Predicate)
    {
        const double Deadline = FPlatformTime::Seconds() + 2.0;
        while (!Predicate() && FPlatformTime::Seconds() < Deadline) FPlatformProcess::SleepNoStats(.001f);
        return Predicate();
    }

    bool PushSilence(FDeliverySpeechAudioProcessor& Processor, int32 Frames, bool bRender, int32 SampleRate = 48000)
    {
        TArray<float> Audio;
        const int32 Channels = bRender ? 2 : 1;
        Audio.SetNumZeroed(Frames * Channels);
        return bRender
            ? Processor.PushRender(Audio.GetData(), Frames, Channels, SampleRate, FPlatformTime::Seconds())
            : Processor.PushCapture(Audio.GetData(), Frames, Channels, SampleRate, FPlatformTime::Seconds());
    }

    bool WarmAudio(FDeliverySpeechAudioProcessor& Processor)
    {
        return PushSilence(Processor, 480, true) && PushSilence(Processor, 480, false) &&
               WaitForAudio([&Processor] { return Processor.IsReady(); });
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechAudioTurnBoundaryTest, "Delivery.Speech.Audio.TurnBoundaries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliverySpeechAudioTurnBoundaryTest::RunTest(const FString&)
{
    FSpeechAudioProbe Probe;
    FDeliverySpeechAudioProcessor Processor;
    FString Error;
    if (!TestTrue(TEXT("启动独立回声处理线程"), Processor.Start(Probe.MakeCallbacks(), 43, Error))) return false;
    if (!TestTrue(TEXT("麦克风和播放参考均处理后准备完成"), WarmAudio(Processor)))
    {
        Processor.Stop();
        return false;
    }
    TestFalse(TEXT("空轮次不能开始录入"), Processor.BeginTurn(0));
    TestTrue(TEXT("开始第一轮"), Processor.BeginTurn(17));
    TestFalse(TEXT("同一时间只接受一个录入轮次"), Processor.BeginTurn(18));
    TestTrue(TEXT("录入不足10ms的声音"), PushSilence(Processor, 120, false));
    TestTrue(TEXT("松键排入结束屏障"), Processor.FinishTurn(17));
    TestFalse(TEXT("重复松键不能生成第二个结束通知"), Processor.FinishTurn(17));
    // 连续采音补齐 AEC 的10ms帧，只有松键前的120个样本属于第一轮
    TestTrue(TEXT("松键后的麦克风仅用于持续回声处理"), PushSilence(Processor, 360, false));
    TestTrue(TEXT("第一轮在尾部样本交出后完成"), WaitForAudio([&Probe] { return Probe.HasFinished(17); }));

    TestTrue(TEXT("完成后可以开启下一轮"), Processor.BeginTurn(18));
    TestTrue(TEXT("新轮次跨越完整帧和剩余部分"), PushSilence(Processor, 600, false));
    TestTrue(TEXT("结束第二轮"), Processor.FinishTurn(18));
    TestTrue(TEXT("持续采音补齐第二轮尾部"), PushSilence(Processor, 360, false));
    TestTrue(TEXT("第二轮完成"), WaitForAudio([&Probe] { return Probe.HasFinished(18); }));
    Processor.Stop();

    FScopeLock Lock(&Probe.Mutex);
    TestEqual(TEXT("预热和松键后的音频没有泄漏到识别"), Probe.SamplesByTurn.Num(), 2);
    TestEqual(TEXT("第一轮尾部120个样本完整保留"), Probe.SamplesByTurn.FindRef(17), 120);
    TestEqual(TEXT("第二轮600个样本完整保留"), Probe.SamplesByTurn.FindRef(18), 600);
    TestEqual(TEXT("每轮只完成一次"), Probe.FinishedTurns.Num(), 2);
    TestFalse(TEXT("完成之后不会再交出本轮声音"), Probe.bAudioAfterFinish);
    TestFalse(TEXT("输出采样率和数值有效"), Probe.bNonFiniteSample);
    TestTrue(TEXT("回声处理没有错误"), Probe.Errors.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechAudioCancellationTest, "Delivery.Speech.Audio.Cancellation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliverySpeechAudioCancellationTest::RunTest(const FString&)
{
    FSpeechAudioProbe Probe;
    FDeliverySpeechAudioProcessor Processor;
    FString Error;
    if (!TestTrue(TEXT("启动处理线程"), Processor.Start(Probe.MakeCallbacks(), 43, Error))) return false;
    if (!TestTrue(TEXT("预热完成"), WarmAudio(Processor))) { Processor.Stop(); return false; }
    TestTrue(TEXT("开始将被取消的轮次"), Processor.BeginTurn(31));
    PushSilence(Processor, 240, false);
    Processor.CancelTurn(31);
    Processor.CancelTurn(31);
    TestFalse(TEXT("取消后的松键不再完成旧轮次"), Processor.FinishTurn(31));
    TestTrue(TEXT("取消后可立即开始新轮次"), Processor.BeginTurn(32));
    PushSilence(Processor, 240, false);
    TestTrue(TEXT("完成新轮次"), Processor.FinishTurn(32));
    TestTrue(TEXT("新轮次完成通知到达"), WaitForAudio([&Probe] { return Probe.HasFinished(32); }));
    Processor.Stop();
    Processor.Stop();

    FScopeLock Lock(&Probe.Mutex);
    TestEqual(TEXT("不足一帧的旧轮次样本已作废"), Probe.SamplesByTurn.FindRef(31), 0);
    TestEqual(TEXT("新轮次只接收自己的240个样本"), Probe.SamplesByTurn.FindRef(32), 240);
    TestEqual(TEXT("取消不是正常完成"), Probe.FinishedTurns.Num(), 1);
    TestFalse(TEXT("旧轮次没有完成通知"), Probe.FinishedTurns.Contains(31));
    TestTrue(TEXT("重复取消与停止没有错误"), Probe.Errors.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechAudioFormatChangeTest, "Delivery.Speech.Audio.DeviceFormatChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliverySpeechAudioFormatChangeTest::RunTest(const FString&)
{
    FSpeechAudioProbe Probe;
    FDeliverySpeechAudioProcessor Processor;
    FString Error;
    if (!TestTrue(TEXT("启动处理线程"), Processor.Start(Probe.MakeCallbacks(), 43, Error))) return false;
    if (!TestTrue(TEXT("初始格式预热完成"), WarmAudio(Processor))) { Processor.Stop(); return false; }
    TestTrue(TEXT("开始设备切换前的轮次"), Processor.BeginTurn(41));
    AddExpectedMessagePlain(TEXT("SpeechAudioFormatChanged capture=1"), ELogVerbosity::Warning);
    AddExpectedMessagePlain(TEXT("SpeechAudioStopped reason=AudioDeviceFormatChanged"), ELogVerbosity::Warning);
    PushSilence(Processor, 441, false, 44100);
    TestTrue(TEXT("采样率变化明确报告会话失效"), WaitForAudio([&Probe] { return Probe.HasError(); }));
    TestFalse(TEXT("设备改变后不再接受旧轮次样本"), PushSilence(Processor, 480, false));
    Processor.Stop();
    {
        FScopeLock Lock(&Probe.Mutex);
        TestEqual(TEXT("设备失效只报告一次"), Probe.Errors.Num(), 1);
        if (!Probe.Errors.IsEmpty()) TestEqual(TEXT("报告真实格式变化原因"), Probe.Errors[0], FString(TEXT("AudioDeviceFormatChanged")));
        TestTrue(TEXT("失效轮次不能提交"), Probe.FinishedTurns.IsEmpty());
    }
    TestTrue(TEXT("停止后可以重新准备新会话"), Processor.Start(Probe.MakeCallbacks(), 43, Error));
    TestTrue(TEXT("重启后重新等待真实音频准备"), WarmAudio(Processor));
    Processor.Stop();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechAudioStartupDiscontinuityTest, "Delivery.Speech.Audio.StartupDiscontinuity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliverySpeechAudioStartupDiscontinuityTest::RunTest(const FString&)
{
    FSpeechAudioProbe Probe;
    FDeliverySpeechAudioProcessor Processor;
    FString Error;
    float Packet[480] = {};
    if (!TestTrue(TEXT("启动处理线程"), Processor.Start(Probe.MakeCallbacks(), 43, Error))) return false;
    TestTrue(TEXT("启动时先收到不足一帧的播放参考"), PushSilence(Processor, 240, true));
    TestTrue(TEXT("首个麦克风包不连续时排入重置并丢弃此包"),
        Processor.PushCapture(Packet, 480, 1, 48000, FPlatformTime::Seconds(), true));
    TestTrue(TEXT("接收重置之后的半帧播放参考"), PushSilence(Processor, 240, true));
    TestTrue(TEXT("接收下一包连续麦克风"), PushSilence(Processor, 480, false));
    TestTrue(TEXT("新麦克风包已处理"), WaitForAudio([&Processor] { return Processor.GetStatistics().CaptureFrames == 480; }));
    TestFalse(TEXT("重置前的半帧不能与新播放参考拼接成准备完成"), Processor.IsReady());
    TestTrue(TEXT("补齐新的播放参考"), PushSilence(Processor, 240, true));
    TestTrue(TEXT("两路重新预热后才准备完成"), WaitForAudio([&Processor] { return Processor.IsReady(); }));
    {
        FScopeLock Lock(&Probe.Mutex);
        TestTrue(TEXT("未按住说话时没有样本交给识别"), Probe.SamplesByTurn.IsEmpty());
        TestTrue(TEXT("首包重置没有产生完成或错误"), Probe.FinishedTurns.IsEmpty() && Probe.Errors.IsEmpty());
    }
    AddExpectedMessagePlain(TEXT("SpeechAudioStopped reason=AudioCaptureDiscontinuity"), ELogVerbosity::Warning);
    TestFalse(TEXT("之后即使空闲也不能吞掉设备不连续"),
        Processor.PushCapture(Packet, 480, 1, 48000, FPlatformTime::Seconds(), true));
    TestTrue(TEXT("后续不连续报告真实故障"), WaitForAudio([&Probe] { return Probe.HasError(); }));
    Processor.Stop();
    FScopeLock Lock(&Probe.Mutex);
    TestEqual(TEXT("设备不连续只报告一次"), Probe.Errors.Num(), 1);
    if (!Probe.Errors.IsEmpty()) TestEqual(TEXT("设备不连续和内部队列溢出分开诊断"),
        Probe.Errors[0], FString(TEXT("AudioCaptureDiscontinuity")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechAudioFinishDiscontinuityTest, "Delivery.Speech.Audio.FinishDiscontinuity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliverySpeechAudioFinishDiscontinuityTest::RunTest(const FString&)
{
    FSpeechAudioProbe Probe;
    FDeliverySpeechAudioProcessor Processor;
    FString Error;
    float Packet[480] = {};
    if (!TestTrue(TEXT("启动处理线程"), Processor.Start(Probe.MakeCallbacks(), 43, Error))) return false;
    if (!TestTrue(TEXT("预热完成"), WarmAudio(Processor))) { Processor.Stop(); return false; }
    TestTrue(TEXT("开始录音轮次"), Processor.BeginTurn(51));
    TestTrue(TEXT("保留等待补齐的尾部"), PushSilence(Processor, 120, false));
    TestTrue(TEXT("松键等待音频屏障"), Processor.FinishTurn(51));
    AddExpectedMessagePlain(TEXT("SpeechAudioStopped reason=AudioCaptureDiscontinuity"), ELogVerbosity::Warning);
    TestFalse(TEXT("等待尾部时不连续不能作为预热重置"),
        Processor.PushCapture(Packet, 480, 1, 48000, FPlatformTime::Seconds(), true));
    TestTrue(TEXT("尾部丢失会使会话失败"), WaitForAudio([&Probe] { return Probe.HasError(); }));
    Processor.Stop();
    FScopeLock Lock(&Probe.Mutex);
    TestTrue(TEXT("失败尾部不能产生正常完成"), Probe.FinishedTurns.IsEmpty());
    TestEqual(TEXT("本轮不连续只报告一次"), Probe.Errors.Num(), 1);
    if (!Probe.Errors.IsEmpty()) TestEqual(TEXT("保留实际设备故障原因"),
        Probe.Errors[0], FString(TEXT("AudioCaptureDiscontinuity")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechContinuousSilenceTest, "Delivery.Speech.Audio.ContinuousSilence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliverySpeechContinuousSilenceTest::RunTest(const FString&)
{
    FSpeechAudioProbe Probe;
    std::atomic<int32> Detections{0};
    auto Callbacks = Probe.MakeCallbacks();
    Callbacks.OnSpeechDetected = [&Detections](uint64) { Detections.fetch_add(1); };
    FDeliverySpeechAudioProcessor Processor;
    FString Error;
    if (!TestTrue(TEXT("启动真实APM与RNN接线"), Processor.Start(MoveTemp(Callbacks), 43, Error))) return false;
    if (!TestTrue(TEXT("两路预热完成"), WarmAudio(Processor))) { Processor.Stop(); return false; }
    TestTrue(TEXT("启用自动分句"), Processor.EnableContinuousListening());
    TestTrue(TEXT("连续模式拒绝混入旧PTT命令"), !Processor.BeginTurn(501));
    TestTrue(TEXT("静音播放参考正常输入"), PushSilence(Processor, 4800, true));
    TestTrue(TEXT("静音麦克风正常输入"), PushSilence(Processor, 4800, false));
    TestTrue(TEXT("AEC和RNN完成这批真实10ms帧"), WaitForAudio([&Processor] { return Processor.GetStatistics().ProcessedBlocks >= 22; }));
    Processor.Stop();
    TestEqual(TEXT("真实RNN没有把数字静音判为起声"), Detections.load(), 0);
    FScopeLock Lock(&Probe.Mutex);
    TestTrue(TEXT("空闲不交出ASR音频或完成事件"), Probe.SamplesByTurn.IsEmpty() && Probe.FinishedTurns.IsEmpty());
    TestTrue(TEXT("VAD初始化和处理没有错误"), Probe.Errors.IsEmpty());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechContinuousFixtureTest, "Delivery.Speech.Audio.ContinuousFixture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliverySpeechContinuousFixtureTest::RunTest(const FString&)
{
    const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DeliverySpeech/fixtures/3.wav"));
    if (!FPaths::FileExists(Path))
    {
        AddWarning(TEXT("Skipped ContinuousFixture: existing Saved/DeliverySpeech/fixtures/3.wav is unavailable"));
        return true;
    }
    TArray<uint8> Wave;
    FWaveModInfo Info;
    if (!TestTrue(TEXT("读取已有中文录音"), FFileHelper::LoadFileToArray(Wave, *Path) && Info.ReadWaveInfo(Wave.GetData(), Wave.Num()))) return false;
    if (!TestTrue(TEXT("样本保持16k单声道PCM16"), *Info.pSamplesPerSec == 16000 && *Info.pChannels == 1 && *Info.pBitsPerSample == 16)) return false;
    TArray<float> Speech;
    Speech.SetNumUninitialized(Info.SampleDataSize / 2);
    for (int32 Index = 0; Index < Speech.Num(); ++Index)
    {
        int16 Value = 0;
        FMemory::Memcpy(&Value, Info.SampleDataStart + Index * 2, 2);
        Speech[Index] = Value / 32768.f;
    }

    struct FSpan { uint64 DetectionFrame = 0; uint64 EndFrame = 0; int32 Samples = 0; };
    FCriticalSection Mutex;
    TArray<uint64> Detections;
    TMap<uint64, FSpan> Spans;
    TArray<FString> Errors;
    bool bInvalidOutput = false;
    FDeliverySpeechAudioProcessor Processor;
    FDeliverySpeechAudioCallbacks Callbacks;
    Callbacks.OnSpeechDetected = [&](uint64 Id)
    {
        FScopeLock Lock(&Mutex);
        Detections.Add(Id);
        Spans.FindOrAdd(Id).DetectionFrame = Processor.GetStatistics().CaptureFrames;
    };
    Callbacks.OnAudio = [&](uint64 Id, int32 Rate, TArray<float>&& Audio)
    {
        FScopeLock Lock(&Mutex);
        auto& Span = Spans.FindOrAdd(Id);
        bInvalidOutput |= Rate != 16000 || Span.EndFrame != 0;
        for (float Value : Audio) bInvalidOutput |= !FMath::IsFinite(Value);
        Span.Samples += Audio.Num();
    };
    Callbacks.OnFinished = [&](uint64 Id)
    {
        FScopeLock Lock(&Mutex);
        auto& Span = Spans.FindOrAdd(Id);
        bInvalidOutput |= Span.EndFrame != 0;
        Span.EndFrame = Processor.GetStatistics().CaptureFrames;
    };
    Callbacks.OnError = [&](uint64, FString Reason)
    { FScopeLock Lock(&Mutex); Errors.Add(MoveTemp(Reason)); };
    FString Error;
    if (!TestTrue(TEXT("启动真实AEC与RNN工作线程"), Processor.Start(MoveTemp(Callbacks), 0, Error))) return false;
    PushSilence(Processor, 160, true, 16000);
    PushSilence(Processor, 160, false, 16000);
    if (!TestTrue(TEXT("16k两路预热"), WaitForAudio([&] { return Processor.IsReady(); }))) { Processor.Stop(); return false; }
    if (!TestTrue(TEXT("启用真实人声自动分句"), Processor.EnableContinuousListening())) { Processor.Stop(); return false; }
    uint64 Active = 0;
    auto AcceptDetected = [&]()
    {
        uint64 Id = 0;
        {
            FScopeLock Lock(&Mutex);
            if (Active && Spans.FindChecked(Active).EndFrame) Active = 0;
            if (!Active && !Detections.IsEmpty())
            {
                Id = Detections[0];
                Detections.RemoveAt(0);
            }
        }
        if (Id)
        {
            if (!Processor.BeginDetectedTurn(Id, Id)) return false;
            Active = Id;
        }
        return true;
    };
    // 快速逐帧喂入已有样本，不访问硬件；每帧等worker完成，避免测试人为制造队列过载
    const int32 SpeechFrames = FMath::DivideAndRoundUp(Speech.Num(), 160);
    bool bProcessed = true;
    for (int32 Frame = -30; Frame < SpeechFrames + 150 && bProcessed; ++Frame)
    {
        float Samples[160] = {};
        if (Frame >= 0 && Frame < SpeechFrames)
            FMemory::Memcpy(Samples, Speech.GetData() + Frame * 160, sizeof(float) * FMath::Min(160, Speech.Num() - Frame * 160));
        const uint64 Blocks = Processor.GetStatistics().ProcessedBlocks;
        bProcessed = PushSilence(Processor, 160, true, 16000) &&
            Processor.PushCapture(Samples, 160, 1, 16000, FPlatformTime::Seconds()) &&
            WaitForAudio([&] { return Processor.GetStatistics().ProcessedBlocks >= Blocks + 2 || !Processor.IsRunning(); }) &&
            Processor.IsRunning() && AcceptDetected();
    }
    Processor.Stop();
    TestTrue(TEXT("整个真实样本经过AEC与RNN"), bProcessed);
    FScopeLock Lock(&Mutex);
    TestTrue(TEXT("中文录音确实触发人声"), !Spans.IsEmpty());
    TestTrue(TEXT("没有遗留未绑定的后续段"), Detections.IsEmpty());
    int32 TotalSamples = 0;
    for (const auto& Entry : Spans)
    {
        const auto& Span = Entry.Value;
        TestTrue(TEXT("真实人声段自动结束"), Span.EndFrame > Span.DetectionFrame);
        TestTrue(TEXT("保留至少800ms尾部后结束"), Span.EndFrame >= Span.DetectionFrame + 12800);
        if (Entry.Key == 1)
            TestEqual(TEXT("首段预卷和最后完整帧均保留"), uint64(Span.Samples), Span.EndFrame - Span.DetectionFrame + 4000);
        TotalSamples += Span.Samples;
    }
    TestTrue(TEXT("没有把真实录音只截成极短噪声片段"), TotalSamples >= Speech.Num() / 2);
    TestFalse(TEXT("没有重复结束或结束后音频，输出采样有效"), bInvalidOutput);
    TestTrue(TEXT("真实RNN集成没有处理错误"), Errors.IsEmpty());
    return true;
}
#endif
