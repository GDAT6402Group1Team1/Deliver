#pragma once

#include "CoreMinimal.h"
#include "Speech/Capture/DeliverySpeechAudioProcessor.h"

class FAudioDeviceHandle;

/**
 * @brief 持有当前客户端的输入设备和主混音监听，停止时解绑原设备
 * 开关和轮次入口由所属玩家在游戏线程调用，工作线程回调只投递普通数据
 */
class FDeliverySpeechAudioCapture
{
public:
    FDeliverySpeechAudioCapture();
    ~FDeliverySpeechAudioCapture();
    FDeliverySpeechAudioCapture(const FDeliverySpeechAudioCapture&) = delete;
    FDeliverySpeechAudioCapture& operator=(const FDeliverySpeechAudioCapture&) = delete;

    bool Start(const FAudioDeviceHandle& AudioDevice, FDeliverySpeechAudioCallbacks&& Callbacks,
               FString& OutError, int32 CaptureDeviceIndex = INDEX_NONE);
    void Stop();
    bool IsRunning() const;
    bool IsReady() const;
    bool EnableContinuousListening();
    bool BeginDetectedTurn(uint64 DetectionId, uint64 TurnId);
    bool BeginTurn(uint64 TurnId);
    bool FinishTurn(uint64 TurnId);
    void CancelTurn(uint64 TurnId);
    FDeliverySpeechAudioStatistics GetStatistics() const;

private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
};
