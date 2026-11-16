#pragma once

#include "CoreMinimal.h"

enum class EDeliverySpeechInputState : uint8
{
    Off,
    Preparing,
    Ready,
    Capturing,
    Finalizing,
    Error
};

enum class EDeliverySpeechTurnFailure : uint8
{
    None,
    Cancelled,
    CaptureTimeout,
    FinalTimeout,
    EmptyText,
    TextTooLong,
    InvalidClock,
    PreparationFailed,
    AudioFailure,
    RecognitionFailure
};

/**
 * @brief 代次区分麦克风会话，编号区分同一会话中的每次按住说话
 */
struct FDeliverySpeechTurnToken
{
    FGuid Id;
    uint64 Generation = 0;

    bool IsValid() const { return Generation != 0 && Id.IsValid(); }

    bool operator==(const FDeliverySpeechTurnToken& Other) const
    {
        return Generation == Other.Generation && Id == Other.Id;
    }
};

/**
 * @brief 本机语音组件在游戏线程维护轮次，工作线程只回传令牌和结果
 * 状态不持有设备或 UObject，也不发出玩法请求
 */
class FDeliverySpeechTurnState
{
public:
    static constexpr double MaxCaptureSeconds = 15.0;
    static constexpr double FinalizeTimeoutSeconds = 5.0;
    static constexpr int32 MaxTextLength = 256;

    uint64 BeginPreparation()
    {
        AdvanceGeneration();
        ClearActiveTurn();
        State = EDeliverySpeechInputState::Preparing;
        LastFailure = EDeliverySpeechTurnFailure::None;
        return Generation;
    }

    bool MarkReady(uint64 PreparedGeneration)
    {
        if (PreparedGeneration != Generation || State != EDeliverySpeechInputState::Preparing) return false;
        State = EDeliverySpeechInputState::Ready;
        LastFailure = EDeliverySpeechTurnFailure::None;
        return true;
    }

    void Disable()
    {
        AdvanceGeneration();
        ClearActiveTurn();
        State = EDeliverySpeechInputState::Off;
        LastFailure = EDeliverySpeechTurnFailure::None;
    }

    /// @brief 返回 true 才开始本轮，重复按下不会替换仍在录入或收尾的内容
    bool BeginCapture(double Now, FDeliverySpeechTurnToken& OutToken)
    {
        OutToken = {};
        if (State != EDeliverySpeechInputState::Ready) return false;
        if (!FMath::IsFinite(Now))
        {
            LastFailure = EDeliverySpeechTurnFailure::InvalidClock;
            return false;
        }

        ActiveToken = { FGuid::NewGuid(), Generation };
        CaptureStartedAt = LastObservedAt = Now;
        Deadline = Now + MaxCaptureSeconds;
        State = EDeliverySpeechInputState::Capturing;
        LastFailure = EDeliverySpeechTurnFailure::None;
        OutToken = ActiveToken;
        return true;
    }

    /// @brief 只有正常松键进入收尾，重复松键不能延长识别期限
    bool FinishCapture(const FDeliverySpeechTurnToken& Token, double Now)
    {
        if (!IsCurrent(Token) || State != EDeliverySpeechInputState::Capturing) return false;
        if (Tick(Now) != EDeliverySpeechTurnFailure::None) return false;
        State = EDeliverySpeechInputState::Finalizing;
        Deadline = Now + FinalizeTimeoutSeconds;
        return true;
    }

    /// @brief 成功时先作废令牌再交出文字，调用者最多发布一次服务器请求
    /// 只裁去首尾空白，单字和玩家改口都按原文保留
    bool TryAcceptFinal(const FDeliverySpeechTurnToken& Token, const FString& Text, double Now, FString& OutText)
    {
        if (!IsCurrent(Token) || State != EDeliverySpeechInputState::Finalizing)
        {
            OutText.Reset();
            return false;
        }
        if (Tick(Now) != EDeliverySpeechTurnFailure::None)
        {
            OutText.Reset();
            return false;
        }

        FString FinalText = Text.TrimStartAndEnd();
        OutText.Reset();
        if (FinalText.IsEmpty())
        {
            EndTurn(EDeliverySpeechTurnFailure::EmptyText);
            return false;
        }
        if (FinalText.Len() > MaxTextLength)
        {
            EndTurn(EDeliverySpeechTurnFailure::TextTooLong);
            return false;
        }

        EndTurn(EDeliverySpeechTurnFailure::None);
        OutText = MoveTemp(FinalText);
        return true;
    }

    bool CancelCapture(const FDeliverySpeechTurnToken& Token)
    {
        if (!IsCurrent(Token)) return false;
        EndTurn(EDeliverySpeechTurnFailure::Cancelled);
        return true;
    }

    /// @brief 无参入口只供本机取消当前操作，异步回调必须使用带令牌的入口
    bool CancelCapture() { return CancelCapture(ActiveToken); }

    /// @brief 超时只终止本轮，不提交已经识别出的半句话
    EDeliverySpeechTurnFailure Tick(double Now)
    {
        if (!HasActiveTurn()) return EDeliverySpeechTurnFailure::None;
        if (!FMath::IsFinite(Now) || Now < LastObservedAt)
        {
            EndTurn(EDeliverySpeechTurnFailure::InvalidClock);
            return EDeliverySpeechTurnFailure::InvalidClock;
        }
        LastObservedAt = Now;
        if (Now < Deadline) return EDeliverySpeechTurnFailure::None;

        const auto Failure = State == EDeliverySpeechInputState::Capturing
            ? EDeliverySpeechTurnFailure::CaptureTimeout : EDeliverySpeechTurnFailure::FinalTimeout;
        EndTurn(Failure);
        return Failure;
    }

    /// @brief 设备和准备错误按会话代次作废，旧设备的回调不能关闭新会话
    bool Fail(uint64 FailedGeneration, EDeliverySpeechTurnFailure Failure)
    {
        if (FailedGeneration != Generation || State == EDeliverySpeechInputState::Off ||
            Failure == EDeliverySpeechTurnFailure::None) return false;
        ClearActiveTurn();
        State = EDeliverySpeechInputState::Error;
        LastFailure = Failure;
        return true;
    }

    bool FailTurn(const FDeliverySpeechTurnToken& Token, EDeliverySpeechTurnFailure Failure)
    {
        if (!IsCurrent(Token) || Failure == EDeliverySpeechTurnFailure::None) return false;
        return Fail(Token.Generation, Failure);
    }

    bool IsCurrent(const FDeliverySpeechTurnToken& Token) const
    {
        return HasActiveTurn() && Token.IsValid() && Token == ActiveToken;
    }

    bool HasActiveTurn() const
    {
        return ActiveToken.IsValid() &&
            (State == EDeliverySpeechInputState::Capturing || State == EDeliverySpeechInputState::Finalizing);
    }

    EDeliverySpeechInputState GetState() const { return State; }
    EDeliverySpeechTurnFailure GetLastFailure() const { return LastFailure; }
    uint64 GetGeneration() const { return Generation; }
    const FDeliverySpeechTurnToken& GetActiveToken() const { return ActiveToken; }
    double GetCaptureStartedAt() const { return CaptureStartedAt; }
    double GetDeadline() const { return Deadline; }

private:
    void AdvanceGeneration()
    {
        ++Generation;
        if (Generation == 0) ++Generation;
    }

    void ClearActiveTurn()
    {
        ActiveToken = {};
        CaptureStartedAt = LastObservedAt = Deadline = 0;
    }

    void EndTurn(EDeliverySpeechTurnFailure Failure)
    {
        ClearActiveTurn();
        State = EDeliverySpeechInputState::Ready;
        LastFailure = Failure;
    }

    EDeliverySpeechInputState State = EDeliverySpeechInputState::Off;
    EDeliverySpeechTurnFailure LastFailure = EDeliverySpeechTurnFailure::None;
    uint64 Generation = 0;
    FDeliverySpeechTurnToken ActiveToken;
    double CaptureStartedAt = 0;
    double LastObservedAt = 0;
    double Deadline = 0;
};
