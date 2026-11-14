#pragma once

#include "CoreMinimal.h"

// 音频线程只把净化后的本轮样本交给识别队列，不访问 UObject
struct FDeliverySpeechAudioCallbacks
{
    TFunction<void(uint64 TurnId, int32 SampleRate, TArray<float>&& Samples)> OnAudio;
    TFunction<void(uint64 TurnId)> OnFinished;
    // 连续模式确认人声后通知普通段编号，游戏线程随后绑定玩法轮次
    TFunction<void(uint64 DetectionId)> OnSpeechDetected;
    // TurnId 为 0 表示整段采音会话失效，调用方应在所属线程停止并重新准备设备
    TFunction<void(uint64 TurnId, FString Error)> OnError;
};

struct FDeliverySpeechAudioStatistics
{
    uint64 CaptureFrames = 0;
    uint64 RenderFrames = 0;
    uint64 ProcessedBlocks = 0;
    double LastProcessingMilliseconds = 0;
    double LastQueueMilliseconds = 0;
    int32 EstimatedStreamDelayMilliseconds = 0;
};

// 连续处理本机播放参考和麦克风，自动人声分段或显式录入期间的样本进入识别
// 实例归属于一个本机玩家，不保存世界、角色或跨 PIE 的静态状态
class FDeliverySpeechAudioProcessor
{
public:
    FDeliverySpeechAudioProcessor();
    ~FDeliverySpeechAudioProcessor();
    FDeliverySpeechAudioProcessor(const FDeliverySpeechAudioProcessor&) = delete;
    FDeliverySpeechAudioProcessor& operator=(const FDeliverySpeechAudioProcessor&) = delete;

    bool Start(FDeliverySpeechAudioCallbacks&& Callbacks, int32 EstimatedOutputLatencyMilliseconds, FString& OutError);
    // 先拒绝新回调，再等待工作线程结束；回调不得反向调用 Stop
    void Stop();
    bool IsRunning() const;
    bool IsReady() const;
    bool EnableContinuousListening();
    bool BeginDetectedTurn(uint64 DetectionId, uint64 TurnId);

    // true 表示命令已排入音频队列，不能据此认为已经产生识别结果
    bool BeginTurn(uint64 TurnId);
    bool FinishTurn(uint64 TurnId);
    void CancelTurn(uint64 TurnId);

    // 输入为交错 float PCM；只复制到有界队列，不在设备回调执行 AEC
    bool PushCapture(const float* Samples, int32 NumFrames, int32 NumChannels, int32 SampleRate,
                     double ArrivedAtSeconds, bool bOverflow = false);
    bool PushRender(const float* Samples, int32 NumFrames, int32 NumChannels, int32 SampleRate,
                    double ArrivedAtSeconds);
    FDeliverySpeechAudioStatistics GetStatistics() const;

private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
};
