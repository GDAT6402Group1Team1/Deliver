#pragma once

#include "CoreMinimal.h"

enum class EDeliverySpeechRecognitionEvent : uint8 { Ready, Partial, Final, Error };

struct FDeliverySpeechRecognitionEvent
{
    EDeliverySpeechRecognitionEvent Kind = EDeliverySpeechRecognitionEvent::Error;
    uint64 TurnId = 0;
    FString Text;
    double ProducedAt = 0;
};

/**
 * @brief 独立线程持有原生识别句柄，音频处理线程只提交有界音频，游戏线程读取结果
 * Start 和 Stop 由同一游戏线程调用，销毁前先停止所有提交音频的回调
 */
class FDeliverySpeechRecognizer
{
public:
    FDeliverySpeechRecognizer();
    ~FDeliverySpeechRecognizer();
    FDeliverySpeechRecognizer(const FDeliverySpeechRecognizer&) = delete;
    FDeliverySpeechRecognizer& operator=(const FDeliverySpeechRecognizer&) = delete;

    /// @brief 返回 true 表示准备任务已启动，模型加载成功通过 Ready 通知
    /// 上次工作线程尚未退出时返回 false，不在这里等待模型加载或释放
    bool Start(const FString& RuntimeDirectory, const FString& Hotwords, FString& Error);
    /// @brief 立即禁止新命令并丢弃结果，工作线程完成手头的原生调用后自行释放模型
    void RequestStop();
    bool IsStopped() const;
    /// @brief 正常关闭时先调用 RequestStop，等 IsStopped 返回 true 后再调用它；析构时也会调用，确保线程已经退出
    void Stop();
    /// @brief 同一工作线程会话的编号单调递增，旧 Begin 不能取消已经开始的新轮次
    bool BeginTurn(uint64 TurnId);
    bool PushAudio(uint64 TurnId, int32 SampleRate, TArray<float>&& Samples);
    /// @brief 最后一批音频已经提交后才调用，重复完成不会延长或再次提交本轮
    bool FinishTurn(uint64 TurnId);
    void CancelTurn(uint64 TurnId);
    bool Poll(FDeliverySpeechRecognitionEvent& OutEvent);

private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
};
