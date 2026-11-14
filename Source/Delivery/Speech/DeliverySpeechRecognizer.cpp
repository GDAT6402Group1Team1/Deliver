#include "Speech/DeliverySpeechRecognizer.h"
#include "delivery_speech_runtime.h"
#include "Containers/Queue.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include <atomic>

class FDeliverySpeechRecognizer::FImpl final : public FRunnable
{
public:
    enum class ECommand : uint8 { Begin, Audio, Finish, Cancel };
    struct FCommand
    {
        ECommand Kind = ECommand::Cancel;
        uint64 TurnId = 0;
        int32 SampleRate = 0;
        int64 QueuedMicroseconds = 0;
        TArray<float> Samples;
    };

    FImpl() : Wake(FPlatformProcess::GetSynchEventFromPool(false)) {}
    ~FImpl() override
    {
        Shutdown();
        FPlatformProcess::ReturnSynchEventToPool(Wake);
    }

    bool StartWorker(const FString& Directory, const FString& Words, FString& Error)
    {
        Error.Reset();
        if (!bWorkerStopped.load())
        {
            Error = TEXT("语音识别仍在收尾，请稍后重试");
            return false;
        }
        Shutdown();
        RuntimeDirectory = FPaths::ConvertRelativePathToFull(Directory);
        Hotwords = Words;
        HighestTurnId = 0;
        bStopping.store(false);
        bWorkerStopped.store(false);
        Thread.Reset(FRunnableThread::Create(this, TEXT("DeliverySpeechRecognition"), 0, TPri_BelowNormal));
        if (!Thread)
        {
            bStopping.store(true);
            bWorkerStopped.store(true);
            Error = TEXT("无法启动语音识别线程");
            return false;
        }
        return true;
    }

    void RequestStop()
    {
        // 准入和入队使用同一把锁，Stop 之后不能留下迟到命令给下一次 Start
        {
            FScopeLock Lock(&CommandsMutex);
            bStopping.store(true);
            bRuntimeReady = false;
            AcceptedTurn = 0;
            bFinishQueued = false;
            ClearCommandsLocked();
            FScopeLock EventLock(&EventsMutex);
            Events.Reset();
        }
        Wake->Trigger();
    }

    bool IsStopped() const { return bWorkerStopped.load(); }

    void Shutdown()
    {
        RequestStop();
        if (Thread)
        {
            Thread->WaitForCompletion();
            Thread.Reset();
        }
    }

    bool BeginTurn(uint64 TurnId)
    {
        FScopeLock Lock(&CommandsMutex);
        if (bStopping.load() || !bRuntimeReady || !TurnId) return false;
        if (AcceptedTurn == TurnId) return !bFinishQueued;
        if (TurnId <= HighestTurnId) return false;

        // 新轮次取代旧轮次，旧 worker 即使正在解码也不能再发布结果
        ClearCommandsLocked();
        AcceptedTurn = TurnId;
        HighestTurnId = TurnId;
        bFinishQueued = false;
        {
            FScopeLock EventLock(&EventsMutex);
            Events.RemoveAll([](const auto& Event) { return Event.TurnId != 0; });
        }
        FCommand Command;
        Command.Kind = ECommand::Begin;
        Command.TurnId = TurnId;
        EnqueueLocked(MoveTemp(Command));
        return true;
    }

    bool EnqueueAudio(uint64 TurnId, int32 Rate, TArray<float>&& Samples)
    {
        FScopeLock Lock(&CommandsMutex);
        if (bStopping.load() || !bRuntimeReady || !TurnId || TurnId != AcceptedTurn || bFinishQueued) return false;
        if (Rate < 8000 || Rate > 96000 || Samples.IsEmpty() || Samples.Num() > Rate)
        {
            RejectTurnLocked(TurnId, TEXT("语音采样格式无效，本次录音已取消"));
            return false;
        }
        const int64 Duration = (static_cast<int64>(Samples.Num()) * 1000000 + Rate - 1) / Rate;
        if (PendingMicroseconds + Duration > MaxPendingMicroseconds || PendingCommands >= MaxPendingCommands)
        {
            RejectTurnLocked(TurnId, TEXT("语音处理跟不上录音，本次录音已取消，请重试"));
            return false;
        }
        FCommand Command;
        Command.Kind = ECommand::Audio;
        Command.TurnId = TurnId;
        Command.SampleRate = Rate;
        Command.QueuedMicroseconds = Duration;
        Command.Samples = MoveTemp(Samples);
        EnqueueLocked(MoveTemp(Command));
        return true;
    }

    bool FinishTurn(uint64 TurnId)
    {
        FScopeLock Lock(&CommandsMutex);
        if (bStopping.load() || !bRuntimeReady || !TurnId || TurnId != AcceptedTurn) return false;
        if (bFinishQueued) return true;
        if (PendingCommands >= MaxPendingCommands)
        {
            RejectTurnLocked(TurnId, TEXT("语音处理队列已满，本次录音已取消"));
            return false;
        }
        bFinishQueued = true;
        FCommand Command;
        Command.Kind = ECommand::Finish;
        Command.TurnId = TurnId;
        EnqueueLocked(MoveTemp(Command));
        return true;
    }

    void CancelTurn(uint64 TurnId)
    {
        FScopeLock Lock(&CommandsMutex);
        if (!TurnId) return;
        // final 可能已生成但游戏线程尚未领取，取消也要移除这份待领结果
        {
            FScopeLock EventLock(&EventsMutex);
            Events.RemoveAll([TurnId](const auto& Event) { return Event.TurnId == TurnId; });
        }
        if (bStopping.load() || TurnId != AcceptedTurn) return;
        CancelAcceptedTurnLocked(TurnId);
    }

    bool PopEvent(FDeliverySpeechRecognitionEvent& Out)
    {
        FScopeLock Lock(&EventsMutex);
        if (Events.IsEmpty()) return false;
        Out = MoveTemp(Events[0]);
        Events.RemoveAt(0);
        return true;
    }

    uint32 Run() override
    {
        // 标记前先释放句柄和 DLL，游戏线程看到完成后可安全回收此实例
        ON_SCOPE_EXIT { bWorkerStopped.store(true); };
        if (bStopping.load()) return 0;
        FString Error;
        if (!LoadRuntime(Error))
        {
            Publish(EDeliverySpeechRecognitionEvent::Error, 0, MoveTemp(Error));
            bStopping.store(true);
            ReleaseRuntime();
            return 0;
        }
        {
            FScopeLock Lock(&CommandsMutex);
            bRuntimeReady = !bStopping.load();
        }
        Publish(EDeliverySpeechRecognitionEvent::Ready, 0, FString());
        while (!bStopping.load())
        {
            FCommand Command;
            if (!Dequeue(Command))
            {
                Wake->Wait(100);
                continue;
            }
            Process(Command);
        }
        ReleaseRuntime();
        return 0;
    }

private:
    static constexpr int64 MaxPendingMicroseconds = 2000000;
    static constexpr int32 MaxPendingCommands = 256;
    static constexpr int32 MaxPendingEvents = 32;

    // 以下 Locked 方法只能在持有 CommandsMutex 时调用
    void ClearCommandsLocked()
    {
        FCommand Command;
        while (Commands.Dequeue(Command)) {}
        PendingMicroseconds = 0;
        PendingCommands = 0;
    }

    void EnqueueLocked(FCommand&& Command)
    {
        PendingMicroseconds += Command.QueuedMicroseconds;
        ++PendingCommands;
        Commands.Enqueue(MoveTemp(Command));
        Wake->Trigger();
    }

    bool Dequeue(FCommand& OutCommand)
    {
        FScopeLock Lock(&CommandsMutex);
        if (bStopping.load() || !Commands.Dequeue(OutCommand)) return false;
        PendingMicroseconds -= OutCommand.QueuedMicroseconds;
        --PendingCommands;
        return true;
    }

    void CancelAcceptedTurnLocked(uint64 TurnId)
    {
        AcceptedTurn = 0;
        bFinishQueued = false;
        ClearCommandsLocked();
        {
            FScopeLock EventLock(&EventsMutex);
            Events.RemoveAll([TurnId](const auto& Event) { return Event.TurnId == TurnId; });
        }
        FCommand Command;
        Command.Kind = ECommand::Cancel;
        Command.TurnId = TurnId;
        EnqueueLocked(MoveTemp(Command));
    }

    void RejectTurnLocked(uint64 TurnId, FString Message)
    {
        CancelAcceptedTurnLocked(TurnId);
        AppendEventLocked(EDeliverySpeechRecognitionEvent::Error, TurnId, MoveTemp(Message));
    }

    template<typename T>
    bool Bind(T& Function, const TCHAR* Name)
    {
        Function = reinterpret_cast<T>(FPlatformProcess::GetDllExport(Library, Name));
        return Function != nullptr;
    }

    bool LoadRuntime(FString& Error)
    {
#if PLATFORM_WINDOWS
        const FString Path = FPaths::Combine(RuntimeDirectory, TEXT("DeliverySpeechRuntime.dll"));
        if (!FPaths::FileExists(Path))
        {
            Error = TEXT("语音识别资源尚未安装，请运行项目的语音准备脚本");
            return false;
        }
        Library = FPlatformProcess::GetDllHandle(*Path);
        if (!Library || !Bind(AbiVersion, TEXT("dsr_abi_version")) ||
            !Bind(Create, TEXT("dsr_create")) || !Bind(Begin, TEXT("dsr_begin")) ||
            !Bind(Push, TEXT("dsr_push")) || !Bind(Finish, TEXT("dsr_finish")) ||
            !Bind(Cancel, TEXT("dsr_cancel")) || !Bind(GetResult, TEXT("dsr_get_result")) ||
            !Bind(GetError, TEXT("dsr_get_error")) || !Bind(Destroy, TEXT("dsr_destroy")) ||
            AbiVersion() != DSR_ABI_VERSION)
        {
            Error = TEXT("语音识别运行库版本不匹配");
            return false;
        }
        const FTCHARToUTF8 ModelPath(*FPaths::Combine(RuntimeDirectory, TEXT("models")));
        const FTCHARToUTF8 Words(*Hotwords);
        DsrConfig Config{};
        Config.struct_size = sizeof(Config);
        Config.abi_version = DSR_ABI_VERSION;
        Config.model_directory_utf8 = ModelPath.Get();
        Config.hotwords_utf8 = Words.Get();
        Config.num_threads = 1;
        Config.max_utterance_ms = 15000;
        Config.hotword_score = 2.f;
        char Message[2048]{};
        if (Create(&Config, &Recognizer, Message, UE_ARRAY_COUNT(Message)) != DSR_OK)
        {
            Error = FString::Printf(TEXT("语音识别准备失败：%s"), UTF8_TO_TCHAR(Message));
            return false;
        }
        return true;
#else
        Error = TEXT("本版语音识别仅支持 Windows");
        return false;
#endif
    }

    void ReleaseRuntime()
    {
        if (Recognizer && Destroy) Destroy(Recognizer);
        Recognizer = nullptr;
        if (Library) FPlatformProcess::FreeDllHandle(Library);
        Library = nullptr;
        ActiveTurn = 0;
        LastRevision = 0;
    }

    void Process(const FCommand& Command)
    {
        {
            FScopeLock Lock(&CommandsMutex);
            if (bStopping.load()) return;
            if (Command.Kind != ECommand::Cancel && Command.TurnId != AcceptedTurn) return;
        }
        // 旧轮次命令不影响正在识别的新一轮
        if (Command.Kind != ECommand::Begin && Command.TurnId != ActiveTurn) return;
        int32 Status = DSR_OK;
        switch (Command.Kind)
        {
        case ECommand::Begin:
            if (ActiveTurn && ActiveTurn != Command.TurnId) Cancel(Recognizer, ActiveTurn);
            Status = Begin(Recognizer, Command.TurnId);
            if (Status == DSR_OK)
            {
                ActiveTurn = Command.TurnId;
                LastRevision = 0;
            }
            break;
        case ECommand::Audio:
            Status = Push(Recognizer, Command.TurnId, Command.SampleRate,
                          Command.Samples.GetData(), Command.Samples.Num());
            break;
        case ECommand::Finish:
            Status = Finish(Recognizer, Command.TurnId);
            break;
        case ECommand::Cancel:
            Cancel(Recognizer, Command.TurnId);
            ActiveTurn = 0;
            return;
        }
        if (Status != DSR_OK)
        {
            char Message[2048]{};
            uint32 Required = 0;
            GetError(Recognizer, Message, UE_ARRAY_COUNT(Message), &Required);
            Cancel(Recognizer, Command.TurnId);
            ActiveTurn = 0;
            Publish(EDeliverySpeechRecognitionEvent::Error, Command.TurnId,
                    FString::Printf(TEXT("语音识别失败（%d）：%s"), Status, UTF8_TO_TCHAR(Message)));
            return;
        }
        ReadResult();
    }

    void ReadResult()
    {
        DsrResultInfo Info{};
        Info.struct_size = sizeof(Info);
        char SmallText[2048]{};
        uint32 Required = 0;
        int32 Status = GetResult(Recognizer, &Info, SmallText, UE_ARRAY_COUNT(SmallText), &Required);
        TArray<char> LargeText;
        const char* Text = SmallText;
        if (Status == DSR_BUFFER_TOO_SMALL && Required <= 65536)
        {
            LargeText.SetNumZeroed(Required);
            Status = GetResult(Recognizer, &Info, LargeText.GetData(), LargeText.Num(), &Required);
            Text = LargeText.GetData();
        }
        if (Status != DSR_OK)
        {
            const uint64 FailedTurn = ActiveTurn;
            Cancel(Recognizer, FailedTurn);
            ActiveTurn = 0;
            Publish(EDeliverySpeechRecognitionEvent::Error, FailedTurn, TEXT("无法读取语音识别结果"));
            return;
        }
        if (!Info.turn_id || Info.turn_id != ActiveTurn || Info.revision == LastRevision) return;
        LastRevision = Info.revision;
        if (Info.state == DSR_FINAL)
        {
            Publish(EDeliverySpeechRecognitionEvent::Final, ActiveTurn, UTF8_TO_TCHAR(Text));
            ActiveTurn = 0;
        }
        else if (Info.state == DSR_STREAMING)
        {
            Publish(EDeliverySpeechRecognitionEvent::Partial, ActiveTurn, UTF8_TO_TCHAR(Text));
        }
    }

    void Publish(EDeliverySpeechRecognitionEvent Kind, uint64 TurnId, FString Text)
    {
        FScopeLock Lock(&CommandsMutex);
        if (bStopping.load() || (TurnId && TurnId != AcceptedTurn)) return;
        if (Kind == EDeliverySpeechRecognitionEvent::Ready && !bRuntimeReady) return;
        AppendEventLocked(Kind, TurnId, MoveTemp(Text));
        if (TurnId && (Kind == EDeliverySpeechRecognitionEvent::Final || Kind == EDeliverySpeechRecognitionEvent::Error))
        {
            AcceptedTurn = 0;
            bFinishQueued = false;
            ClearCommandsLocked();
        }
    }

    void AppendEventLocked(EDeliverySpeechRecognitionEvent Kind, uint64 TurnId, FString Text)
    {
        FScopeLock EventLock(&EventsMutex);
        // 游戏卡顿时只保留每轮最新草稿，终态不会排在大量旧草稿之后
        Events.RemoveAll([TurnId](const auto& Event)
        { return Event.TurnId == TurnId && Event.Kind == EDeliverySpeechRecognitionEvent::Partial; });
        if (Events.Num() >= MaxPendingEvents)
        {
            if (Kind == EDeliverySpeechRecognitionEvent::Partial) return;
            // 终态不能静默丢失，意外积压时明确结束识别会话
            Events.Reset();
            Events.Add({ EDeliverySpeechRecognitionEvent::Error, 0,
                TEXT("语音结果队列已满，请重新开启语音对话"), FPlatformTime::Seconds() });
            bRuntimeReady = false;
            bStopping.store(true);
            AcceptedTurn = 0;
            bFinishQueued = false;
            ClearCommandsLocked();
            Wake->Trigger();
            return;
        }
        Events.Add({Kind, TurnId, MoveTemp(Text), FPlatformTime::Seconds()});
    }

    FString RuntimeDirectory;
    FString Hotwords;
    TUniquePtr<FRunnableThread> Thread;
    FEvent* Wake = nullptr;
    std::atomic<bool> bStopping{true};
    std::atomic<bool> bWorkerStopped{true};
    FCriticalSection CommandsMutex;
    bool bRuntimeReady = false;
    bool bFinishQueued = false;
    uint64 AcceptedTurn = 0;
    uint64 HighestTurnId = 0;
    int64 PendingMicroseconds = 0;
    int32 PendingCommands = 0;
    TQueue<FCommand, EQueueMode::Mpsc> Commands;
    FCriticalSection EventsMutex;
    TArray<FDeliverySpeechRecognitionEvent> Events;
    void* Library = nullptr;
    DsrHandle* Recognizer = nullptr;
    uint64 ActiveTurn = 0;
    uint64 LastRevision = 0;
    decltype(&dsr_abi_version) AbiVersion = nullptr;
    decltype(&dsr_create) Create = nullptr;
    decltype(&dsr_begin) Begin = nullptr;
    decltype(&dsr_push) Push = nullptr;
    decltype(&dsr_finish) Finish = nullptr;
    decltype(&dsr_cancel) Cancel = nullptr;
    decltype(&dsr_get_result) GetResult = nullptr;
    decltype(&dsr_get_error) GetError = nullptr;
    decltype(&dsr_destroy) Destroy = nullptr;
};

FDeliverySpeechRecognizer::FDeliverySpeechRecognizer() : Impl(MakeUnique<FImpl>()) {}
FDeliverySpeechRecognizer::~FDeliverySpeechRecognizer() = default;
bool FDeliverySpeechRecognizer::Start(const FString& Directory, const FString& Hotwords, FString& Error)
{ return Impl->StartWorker(Directory, Hotwords, Error); }
void FDeliverySpeechRecognizer::RequestStop() { Impl->RequestStop(); }
bool FDeliverySpeechRecognizer::IsStopped() const { return Impl->IsStopped(); }
void FDeliverySpeechRecognizer::Stop() { Impl->Shutdown(); }
bool FDeliverySpeechRecognizer::BeginTurn(uint64 TurnId)
{ return Impl->BeginTurn(TurnId); }
bool FDeliverySpeechRecognizer::PushAudio(uint64 TurnId, int32 Rate, TArray<float>&& Samples)
{ return Impl->EnqueueAudio(TurnId, Rate, MoveTemp(Samples)); }
bool FDeliverySpeechRecognizer::FinishTurn(uint64 TurnId)
{ return Impl->FinishTurn(TurnId); }
void FDeliverySpeechRecognizer::CancelTurn(uint64 TurnId)
{ Impl->CancelTurn(TurnId); }
bool FDeliverySpeechRecognizer::Poll(FDeliverySpeechRecognitionEvent& Out) { return Impl->PopEvent(Out); }
