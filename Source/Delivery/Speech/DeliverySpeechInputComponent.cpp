#include "Speech/DeliverySpeechInputComponent.h"

#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AudioDeviceHandle.h"
#include "AudioDeviceNotificationSubsystem.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Speech/Capture/DeliverySpeechAudioCapture.h"
#include "Speech/Recognition/DeliverySpeechRecognizer.h"
#include "Widgets/SViewport.h"
#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogDeliverySpeechInput, Log, All);

namespace
{
    // 只接受音频层约定的固定错误码，设备文本、音频和识别内容不进入日志
    const TCHAR* SafeAudioFault(const FString& Reason)
    {
        static const TCHAR* const Codes[] = {
            TEXT("AudioCaptureDiscontinuity"), TEXT("AudioQueueOverflow"), TEXT("AudioQueueStalled"),
            TEXT("AudioFormatUnsupported"), TEXT("AudioDeviceFormatChanged"), TEXT("AudioSamplesInvalid"),
            TEXT("EchoProcessingFailed"), TEXT("AudioStreamStopped"), TEXT("AudioProcessingFailed"),
            TEXT("OutputAudioDeviceUnavailable"), TEXT("MicrophoneUnavailable"), TEXT("AudioCallbacksMissing"),
            TEXT("AudioWorkerUnavailable"), TEXT("MicrophoneStartFailed"), TEXT("MicrophoneOpenFailed"),
            TEXT("ContinuousAudioBacklog"), TEXT("SpeechActivityDetectionFailed")
        };
        for (const TCHAR* Code : Codes) if (Reason == Code) return Code;
        return TEXT("UnknownAudioFault");
    }
}

// 回调仅持有这一份普通数据，组件或世界退出后不能反向访问 UObject
struct FDeliverySpeechInputBridge
{
    explicit FDeliverySpeechInputBridge(uint64 InGeneration) : Generation(InGeneration) {}

    void ReportError(uint64 TurnId, EDeliverySpeechTurnFailure Failure)
    {
        FScopeLock Lock(&ErrorMutex);
        if (!bAccepting.load() || (TurnId && TurnId != CurrentTurn.load())) return;
        // 设备级错误覆盖轮次错误，错误槽始终有界
        if (bHasError && FailedTurnId == 0) return;
        bHasError = true;
        FailedTurnId = TurnId;
        PendingFailure = Failure;
    }

    bool TakeError(uint64& TurnId, EDeliverySpeechTurnFailure& Failure)
    {
        FScopeLock Lock(&ErrorMutex);
        if (!bHasError) return false;
        TurnId = FailedTurnId;
        Failure = PendingFailure;
        bHasError = false;
        return true;
    }

    void ReportDetection(uint64 DetectionId)
    {
        bool bOverflow = false;
        {
            FScopeLock Lock(&DetectionMutex);
            if (!bAccepting.load()) return;
            bOverflow = Detections.Num() >= 2;
            if (!bOverflow) Detections.Add(DetectionId);
        }
        if (bOverflow) ReportError(0, EDeliverySpeechTurnFailure::AudioFailure);
    }

    bool TakeDetection(uint64& DetectionId)
    {
        FScopeLock Lock(&DetectionMutex);
        if (Detections.IsEmpty()) return false;
        DetectionId = Detections[0];
        Detections.RemoveAt(0, 1, EAllowShrinking::No);
        return true;
    }

    // 仅游戏线程访问；结束通知重入时若控制关系正在复制，保留已经取出的唯一 final
    bool PollRecognition(FDeliverySpeechRecognitionEvent& Event)
    {
        if (DeferredFinal.IsSet())
        {
            Event = MoveTemp(DeferredFinal.GetValue());
            DeferredFinal.Reset();
            return true;
        }
        return Recognizer->Poll(Event);
    }

    const uint64 Generation;
    TSharedPtr<FDeliverySpeechRecognizer, ESPMode::ThreadSafe> Recognizer =
        MakeShared<FDeliverySpeechRecognizer, ESPMode::ThreadSafe>();
    std::atomic<bool> bAccepting{true};
    std::atomic<bool> bDeviceChanged{false};
    std::atomic<uint64> CurrentTurn{0};
    std::atomic<uint64> EndedTurn{0};
    FCriticalSection DetectionMutex;
    TArray<uint64> Detections;
    TOptional<FDeliverySpeechRecognitionEvent> DeferredFinal;
    FCriticalSection ErrorMutex;
    bool bHasError = false;
    uint64 FailedTurnId = 0;
    EDeliverySpeechTurnFailure PendingFailure = EDeliverySpeechTurnFailure::None;
};

// ── 生命周期 ──

/// @brief 默认不每帧更新，EnableVoice 成功后才开始
UDeliverySpeechInputComponent::UDeliverySpeechInputComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    PrimaryComponentTick.bTickEvenWhenPaused = true;
    SetIsReplicatedByDefault(false);
}

UDeliverySpeechInputComponent::~UDeliverySpeechInputComponent() = default;

// ── 开关 ──

/// @brief 打开和二狗的连麦：检查窗口焦点、已经结识二狗、能对它说话，再启动识别线程和麦克风
/// @return true 表示开始准备，准备好后状态变成 Ready；之后检测到人声才开始录一句
bool UDeliverySpeechInputComponent::EnableVoice()
{
    check(IsInGameThread());
    if (bEndingPlay) return false;
    if (bVoiceEnabled) return true;
    CollectStoppedRecognizer();
    if (Bridge)
    {
        StatusText = TEXT("语音正在关闭，请稍后重试");
        PublishState();
        return false;
    }
    auto* PC = Cast<APlayerController>(GetOwner());
    FDeliverySpeechPlayerContext NewContext;
    if (!PC || !PC->IsLocalController() || !HasLocalViewportFocus() || !NewContext.Capture(PC))
    {
        StatusText = TEXT("请在当前游戏窗口中开启语音");
        PublishState();
        return false;
    }
    const auto* Conversation = PC->FindComponentByClass<UDeliveryConversationComponent>();
    auto* Companion = Conversation ? Conversation->ResolveVoiceCompanion() : nullptr;
    if (!Companion || !Conversation->CanVoiceTalkTo(Companion))
    {
        const uint64 FailedGeneration = TurnState.BeginPreparation();
        TurnState.Fail(FailedGeneration, EDeliverySpeechTurnFailure::PreparationFailed);
        StatusText = LastError = Companion ? TEXT("现在无法与二狗连麦，请稍后再试")
            : TEXT("先与二狗结识，再开启连麦");
        PublishState();
        return false;
    }

    const uint64 Generation = TurnState.BeginPreparation();
    bVoiceEnabled = true;
    bContinuousMode = true;
    ConnectedNpc = Companion;
    bRecognizerReady = false;
    SessionContext = MoveTemp(NewContext);
    ContextPendingSince = -1;
    PartialText.Reset();
    LastError.Reset();
    StatusText = TEXT("正在准备语音");
    SetComponentTickEnabled(true);
    Bridge = MakeShared<FDeliverySpeechInputBridge, ESPMode::ThreadSafe>(Generation);

    FString Error;
    const FString RuntimePath = FPaths::IsRelative(RuntimeDirectory)
        ? FPaths::Combine(FPaths::ProjectDir(), RuntimeDirectory) : RuntimeDirectory;
    if (!Bridge->Recognizer->Start(RuntimePath, Hotwords, Error))
    {
        FailSession(EDeliverySpeechTurnFailure::PreparationFailed, TEXT("语音识别启动失败，请重试"));
        return false;
    }

    const auto Device = GetWorld()->GetAudioDevice();
    AudioDeviceId = Device.GetDeviceID();
    const auto CallbackBridge = Bridge;
    FDeliverySpeechAudioCallbacks Callbacks;
    Callbacks.OnAudio = [CallbackBridge](uint64 TurnId, int32 Rate, TArray<float>&& Samples)
    {
        if (!CallbackBridge->bAccepting.load() || CallbackBridge->CurrentTurn.load() != TurnId) return;
        if (!CallbackBridge->Recognizer->PushAudio(TurnId, Rate, MoveTemp(Samples)))
            CallbackBridge->ReportError(TurnId, EDeliverySpeechTurnFailure::RecognitionFailure);
    };
    Callbacks.OnFinished = [CallbackBridge](uint64 TurnId)
    {
        if (!CallbackBridge->bAccepting.load() || CallbackBridge->CurrentTurn.load() != TurnId) return;
        CallbackBridge->EndedTurn.store(TurnId);
        // 音频线程确认最后一批样本已入队，识别线程才开始生成 final
        if (!CallbackBridge->Recognizer->FinishTurn(TurnId))
            CallbackBridge->ReportError(TurnId, EDeliverySpeechTurnFailure::RecognitionFailure);
    };
    Callbacks.OnSpeechDetected = [CallbackBridge](uint64 DetectionId) { CallbackBridge->ReportDetection(DetectionId); };
    Callbacks.OnError = [CallbackBridge](uint64 TurnId, FString Reason)
    {
        if (!CallbackBridge->bAccepting.load() || (TurnId && CallbackBridge->CurrentTurn.load() != TurnId)) return;
        // 此回调在 AEC 工作线程结束时执行，不访问组件或世界
        UE_LOG(LogDeliverySpeechInput, Warning, TEXT("SpeechAudioFault generation=%llu turn=%llu reason=%s"),
            CallbackBridge->Generation, TurnId, SafeAudioFault(Reason));
        CallbackBridge->ReportError(TurnId, EDeliverySpeechTurnFailure::AudioFailure);
    };
    AudioCapture = MakeUnique<FDeliverySpeechAudioCapture>();
    if (!AudioCapture->Start(Device, MoveTemp(Callbacks), Error))
    {
        UE_LOG(LogDeliverySpeechInput, Warning, TEXT("SpeechAudioStartFailed generation=%llu reason=%s"),
            Generation, SafeAudioFault(Error));
        FailSession(EDeliverySpeechTurnFailure::AudioFailure, TEXT("麦克风无法开启，请检查设备后重试"));
        return false;
    }
    BindDeviceNotifications();
    PublishState();
    return bVoiceEnabled;
}

/// @brief 关闭语音，调用 CloseVoice
void UDeliverySpeechInputComponent::DisableVoice()
{
    CloseVoice(TEXT("语音已关闭"));
}

// ── 按住说话 ──

/// @brief 开始对 Npc 录一句话；语音没准备好、已经在录或不能对它说话时返回 false
bool UDeliverySpeechInputComponent::BeginHold(ADeliveryNpcCharacter* Npc)
{
    check(IsInGameThread());
    if (bContinuousMode) return false;
    if (!bVoiceEnabled || !IsVoiceReady() || !Bridge || !AudioCapture) return false;
    const auto ContextState = CheckSessionContext(FPlatformTime::Seconds());
    if (ContextState == EDeliverySpeechPlayerContextState::Invalid || !HasLocalViewportFocus())
    {
        CloseVoice(TEXT("操作已切换，语音已关闭"));
        return false;
    }
    if (ContextState == EDeliverySpeechPlayerContextState::Pending) return false;
    if (!CanTalkTo(Npc))
    {
        StatusText = TEXT("请靠近可以交谈的人物");
        PublishState();
        return false;
    }
    FDeliverySpeechTurnToken Token;
    if (!TurnState.BeginCapture(FPlatformTime::Seconds(), Token)) return false;
    // 不重用编号，取消后的旧音频不能开始同名的新轮次
    if (++NextNativeTurnId == 0) ++NextNativeTurnId;
    ActiveNativeTurnId = NextNativeTurnId;
    ActiveNpc = Npc;
    Bridge->CurrentTurn.store(ActiveNativeTurnId);
    PartialText.Reset();
    LastError.Reset();
    if (!Bridge->Recognizer->BeginTurn(ActiveNativeTurnId) || !AudioCapture->BeginTurn(ActiveNativeTurnId))
    {
        FailSession(EDeliverySpeechTurnFailure::AudioFailure, TEXT("语音尚未准备好，请重新开启"));
        return false;
    }
    bTurnStarted = true;
    StatusText = TEXT("正在聆听，松开发送");
    OnTurnStarted.Broadcast(Token, Npc);
    PublishState();
    return TurnState.IsCurrent(Token);
}

/// @brief 松开：这一句录完，等识别结果
void UDeliverySpeechInputComponent::FinishHold()
{
    check(IsInGameThread());
    if (bContinuousMode) return;
    if (!IsHolding()) return;
    const auto ContextState = CheckSessionContext(FPlatformTime::Seconds());
    if (ContextState == EDeliverySpeechPlayerContextState::Invalid)
    {
        CloseVoice(TEXT("控制人物已切换，语音已关闭"));
        return;
    }
    if (!HasLocalViewportFocus())
    {
        CloseVoice(TEXT("已离开游戏窗口，语音已关闭"));
        return;
    }
    if (ContextState == EDeliverySpeechPlayerContextState::Valid && !CanTalkTo(ActiveNpc.Get()))
    {
        CancelHold();
        return;
    }
    const auto Token = TurnState.GetActiveToken();
    if (!TurnState.FinishCapture(Token, FPlatformTime::Seconds()))
    {
        StatusText = FailureText(TurnState.GetLastFailure());
        LastError = StatusText;
        ClearActiveTurn(Token, true);
        PublishState();
        return;
    }
    if (!AudioCapture || !AudioCapture->FinishTurn(ActiveNativeTurnId))
    {
        FailSession(EDeliverySpeechTurnFailure::AudioFailure, TEXT("录音结束失败，请重新开启语音"));
        return;
    }
    StatusText = TEXT("正在识别");
    OnHoldReleased.Broadcast(Token);
    PublishState();
}

/// @brief 服务器拒收编号为 Token 的那一句时调用：只取消那一句，已经开始的下一句不受影响
bool UDeliverySpeechInputComponent::ReportTurnRejected(const FDeliverySpeechTurnToken& Token, FName Reason)
{
    check(IsInGameThread());
    if (!TurnState.CancelCapture(Token)) return false;
    if (Reason == TEXT("Busy")) StatusText = TEXT("对方正在忙，请稍后再说");
    else if (Reason == TEXT("Unavailable")) StatusText = TEXT("同伴暂时无法交谈");
    else if (Reason == TEXT("Expired")) StatusText = TEXT("这次交谈已超时，请重新说话");
    else StatusText = TEXT("这次交谈未被接收，请重试");
    LastError = StatusText;
    ClearActiveTurn(Token, true);
    PublishState();
    return true;
}

/// @brief 取消正在录的这一句
void UDeliverySpeechInputComponent::CancelHold()
{
    check(IsInGameThread());
    const auto Token = TurnState.GetActiveToken();
    if (!TurnState.CancelCapture(Token)) return;
    StatusText = bContinuousMode ? ConnectedStatus() : TEXT("已取消，按住可重新说话");
    ClearActiveTurn(Token, true);
    PublishState();
}

// ── 内部 ──

/// @brief 关闭语音：先停麦克风和识别线程，再取消正在录的一句并通知；Message 作为状态文字
void UDeliverySpeechInputComponent::CloseVoice(const FString& Message)
{
    check(IsInGameThread());
    const auto Token = TurnState.GetActiveToken();
    bVoiceEnabled = false;
    TurnState.Disable();
    StatusText = Message;
    // 先关设备，再通知外部取消，外部回调不能让旧音频继续进入识别
    StopSessionResources();
    ClearActiveTurn(Token, true);
    CollectStoppedRecognizer();
    PublishState();
}

/// @brief 出错关闭：记下错误，再调用 CloseVoice
void UDeliverySpeechInputComponent::FailSession(EDeliverySpeechTurnFailure Failure, const FString& Message)
{
    const auto Token = TurnState.GetActiveToken();
    if (!TurnState.Fail(TurnState.GetGeneration(), Failure)) return;
    bVoiceEnabled = false;
    StatusText = Message;
    LastError = Message;
    StopSessionResources();
    ClearActiveTurn(Token, true);
    CollectStoppedRecognizer();
    PublishState();
}

/// @brief 清掉编号为 Token 的那一句；bNotifyCancelled 为 true 时广播 OnTurnCancelled
void UDeliverySpeechInputComponent::ClearActiveTurn(const FDeliverySpeechTurnToken& Token, bool bNotifyCancelled)
{
    if (Bridge)
    {
        Bridge->CurrentTurn.store(0);
        Bridge->DeferredFinal.Reset();
        Bridge->Recognizer->CancelTurn(ActiveNativeTurnId);
    }
    if (AudioCapture) AudioCapture->CancelTurn(ActiveNativeTurnId);
    const bool bWasStarted = bTurnStarted;
    bTurnStarted = false;
    ActiveNativeTurnId = 0;
    ActiveNpc.Reset();
    PartialText.Reset();
    if (bNotifyCancelled && bWasStarted && Token.IsValid()) OnTurnCancelled.Broadcast(Token);
}

/// @brief 停止麦克风，让识别线程停止接收；识别线程退出后由 CollectStoppedRecognizer 释放
void UDeliverySpeechInputComponent::StopSessionResources()
{
    if (Bridge)
    {
        Bridge->bAccepting.store(false);
        Bridge->CurrentTurn.store(0);
    }
    if (AudioCapture)
    {
        AudioCapture->Stop();
        AudioCapture.Reset();
    }
    UnbindDeviceNotifications();
    if (Bridge) Bridge->Recognizer->RequestStop();
    bRecognizerReady = false;
    SessionContext.Reset();
    ContextPendingSince = -1;
    ConnectedNpc.Reset();
    bContinuousMode = false;
}

/// @brief 识别线程已经退出时释放它
void UDeliverySpeechInputComponent::CollectStoppedRecognizer()
{
    if (!bVoiceEnabled && Bridge && Bridge->Recognizer->IsStopped())
    {
        Bridge->Recognizer->Stop();
        Bridge.Reset();
    }
    if (!bVoiceEnabled && !Bridge) SetComponentTickEnabled(false);
}

// ── 生命周期 ──

/// @brief 语音开着时每帧：检查焦点、控制关系、音频设备和二狗，不对就关闭；处理录完的一句、取识别结果广播 OnFinalText；
/// 连麦就绪且上一句已经提交完时，取出音频线程检测到的人声，调用 BeginDetectedSpeech 开始录下一句
void UDeliverySpeechInputComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!bVoiceEnabled)
    {
        CollectStoppedRecognizer();
        return;
    }
    const auto* PC = Cast<APlayerController>(GetOwner());
    const auto ContextState = CheckSessionContext(FPlatformTime::Seconds());
    if (!PC || !PC->IsLocalController() || ContextState == EDeliverySpeechPlayerContextState::Invalid)
    {
        CloseVoice(TEXT("控制人物已切换，语音已关闭"));
        return;
    }
    if (!HasLocalViewportFocus())
    {
        CloseVoice(TEXT("已离开游戏窗口，语音已关闭"));
        return;
    }
    if (!Bridge || Bridge->Generation != TurnState.GetGeneration()) return;
    const auto Device = GetWorld()->GetAudioDevice();
    if (!Device.IsValid() || Device.GetDeviceID() != AudioDeviceId || Bridge->bDeviceChanged.load())
    {
        FailSession(EDeliverySpeechTurnFailure::AudioFailure, TEXT("音频设备已变化，请重新开启语音"));
        return;
    }
    uint64 FailedTurnId = 0;
    EDeliverySpeechTurnFailure Failure;
    if (Bridge->TakeError(FailedTurnId, Failure) && (!FailedTurnId || FailedTurnId == ActiveNativeTurnId))
    {
        FailSession(Failure, FailureText(Failure));
        return;
    }
    if (bContinuousMode && (!ConnectedNpc.IsValid() ||
        (ContextState == EDeliverySpeechPlayerContextState::Valid && !CanTalkTo(ConnectedNpc.Get()))))
    {
        CloseVoice(TEXT("与同伴的语音连接已结束"));
        return;
    }
    if (TurnState.HasActiveTurn() && (!ActiveNpc.IsValid() ||
        (ContextState == EDeliverySpeechPlayerContextState::Valid && !CanTalkTo(ActiveNpc.Get()))))
    {
        CancelHold();
        return;
    }
    // 音频结束先于 final 入识别队列；先转收尾，避免同帧到达的 final 被误当录音中
    PollContinuousEnd();
    if (!bVoiceEnabled) return;
    const auto ExpiringToken = TurnState.GetActiveToken();
    Failure = TurnState.Tick(FPlatformTime::Seconds());
    if (Failure != EDeliverySpeechTurnFailure::None)
    {
        StatusText = FailureText(Failure);
        LastError = StatusText;
        ClearActiveTurn(ExpiringToken, true);
        PublishState();
        return;
    }

    // 上下车复制未齐时保持音频线程和既有期限，最终文字与下一句都等控制关系完整后再领取
    if (ContextState == EDeliverySpeechPlayerContextState::Pending) return;

    // 委托可能关闭或重新开启组件，始终用这次会话快照检查后续事件
    const auto PollBridge = Bridge;
    FDeliverySpeechRecognitionEvent Event;
    while (bVoiceEnabled && Bridge == PollBridge && PollBridge->PollRecognition(Event))
    {
        if (Event.Kind == EDeliverySpeechRecognitionEvent::Ready)
        {
            bRecognizerReady = true;
            continue;
        }
        if (Event.Kind == EDeliverySpeechRecognitionEvent::Error && Event.TurnId == 0)
        {
            FailSession(EDeliverySpeechTurnFailure::RecognitionFailure, TEXT("语音识别不可用，请检查本机模型后重试"));
            return;
        }
        if (!Event.TurnId || Event.TurnId != ActiveNativeTurnId || !TurnState.HasActiveTurn()) continue;
        if (Event.Kind == EDeliverySpeechRecognitionEvent::Error)
        {
            FailSession(EDeliverySpeechTurnFailure::RecognitionFailure, FailureText(EDeliverySpeechTurnFailure::RecognitionFailure));
            return;
        }
        if (Event.Kind == EDeliverySpeechRecognitionEvent::Partial)
        {
            if (bContinuousMode) continue;
            const FString LocalText = Event.Text.Left(FDeliverySpeechTurnState::MaxTextLength);
            if (PartialText != LocalText)
            {
                PartialText = LocalText;
                OnDisplayChanged.Broadcast();
            }
            continue;
        }
        if (Event.Kind == EDeliverySpeechRecognitionEvent::Final)
        {
            PollContinuousEnd();
            if (!bVoiceEnabled || Bridge != PollBridge) return;
            const auto FinalContext = CheckSessionContext(FPlatformTime::Seconds());
            if (!HasLocalViewportFocus() || FinalContext == EDeliverySpeechPlayerContextState::Invalid)
            {
                CloseVoice(TEXT("操作已切换，语音已关闭"));
                return;
            }
            if (FinalContext == EDeliverySpeechPlayerContextState::Pending)
            {
                PollBridge->DeferredFinal = MoveTemp(Event);
                return;
            }
            if (!CanTalkTo(ActiveNpc.Get()))
            {
                CancelHold();
                return;
            }
            const auto Token = TurnState.GetActiveToken();
            const auto Target = ActiveNpc;
            FString FinalText;
            const bool bAccepted = TurnState.TryAcceptFinal(Token, Event.Text, FPlatformTime::Seconds(), FinalText);
            if (!bAccepted && TurnState.IsCurrent(Token)) continue;
            const bool bQuietEmpty = bContinuousMode && TurnState.GetLastFailure() == EDeliverySpeechTurnFailure::EmptyText;
            StatusText = bContinuousMode && (bAccepted || bQuietEmpty) ? ConnectedStatus()
                : (bAccepted ? TEXT("按住说话，松开发送") : FailureText(TurnState.GetLastFailure()));
            if (!bAccepted && !bQuietEmpty) LastError = StatusText;
            ClearActiveTurn(Token, !bAccepted);
            // 接收成功前已作废本轮令牌，重复 final 无法重复触发提交委托
            if (bAccepted && Target.IsValid()) OnFinalText.Broadcast(Token, Target.Get(), FinalText);
            PublishState();
        }
    }
    if (bVoiceEnabled && Bridge == PollBridge && bRecognizerReady && AudioCapture && AudioCapture->IsReady() &&
        TurnState.MarkReady(PollBridge->Generation))
    {
        if (bContinuousMode && !AudioCapture->EnableContinuousListening())
        {
            FailSession(EDeliverySpeechTurnFailure::AudioFailure, TEXT("持续语音启动失败，请重新连接"));
            return;
        }
        StatusText = bContinuousMode ? ConnectedStatus() : TEXT("按住说话，松开发送");
        PublishState();
    }
    const auto* Conversation = PC->FindComponentByClass<UDeliveryConversationComponent>();
    // 上一句等许可或服务器回执时保留检测编号与音频，避免新起声取消尚未送达的完整文字
    if (bVoiceEnabled && Bridge == PollBridge && bContinuousMode && IsVoiceReady() &&
        Conversation && !Conversation->HasPendingVoiceSubmission() &&
        CheckSessionContext(FPlatformTime::Seconds()) == EDeliverySpeechPlayerContextState::Valid)
    {
        uint64 DetectionId = 0;
        if (PollBridge->TakeDetection(DetectionId)) BeginDetectedSpeech(DetectionId);
    }
}

// ── 查询 ──

/// @brief 连麦的对象（二狗）
ADeliveryNpcCharacter* UDeliverySpeechInputComponent::GetConnectedNpc() const { return ConnectedNpc.Get(); }

// ── 内部 ──

/// @brief 连麦中的状态文字，例如与二狗连麦中
FString UDeliverySpeechInputComponent::ConnectedStatus() const
{
    const auto* Npc = ConnectedNpc.Get();
    return Npc ? FString::Printf(TEXT("与%s连麦中"), *Npc->DisplayName.ToString()) : TEXT("语音连麦中");
}

/// @brief 音频线程检测到人声（DetectionId）后，开始对二狗录一句
bool UDeliverySpeechInputComponent::BeginDetectedSpeech(uint64 DetectionId)
{
    if (!bVoiceEnabled || !bContinuousMode || !Bridge || !AudioCapture || !CanTalkTo(ConnectedNpc.Get())) return false;
    if (CheckSessionContext(FPlatformTime::Seconds()) != EDeliverySpeechPlayerContextState::Valid) return false;
    const auto* PC = Cast<APlayerController>(GetOwner());
    const auto* Conversation = PC ? PC->FindComponentByClass<UDeliveryConversationComponent>() : nullptr;
    if (!Conversation || Conversation->HasPendingVoiceSubmission()) return false;
    FDeliverySpeechTurnToken Token;
    if (!TurnState.BeginCapture(FPlatformTime::Seconds(), Token)) return false;
    if (++NextNativeTurnId == 0) ++NextNativeTurnId;
    ActiveNativeTurnId = NextNativeTurnId;
    ActiveNpc = ConnectedNpc;
    Bridge->CurrentTurn.store(ActiveNativeTurnId);
    PartialText.Reset(); LastError.Reset();
    if (!Bridge->Recognizer->BeginTurn(ActiveNativeTurnId) || !AudioCapture->BeginDetectedTurn(DetectionId, ActiveNativeTurnId))
    {
        FailSession(EDeliverySpeechTurnFailure::AudioFailure, TEXT("语音处理跟不上，请重新连接"));
        return false;
    }
    bTurnStarted = true;
    StatusText = ConnectedStatus();
    OnTurnStarted.Broadcast(Token, ActiveNpc.Get());
    PublishState();
    return TurnState.IsCurrent(Token);
}

/// @brief 连麦时检查音频线程是否已经报告这一句说完，说完就进入等识别结果
void UDeliverySpeechInputComponent::PollContinuousEnd()
{
    if (!bContinuousMode || !Bridge) return;
    const uint64 Ended = Bridge->EndedTurn.exchange(0);
    if (!Ended || Ended != ActiveNativeTurnId || !IsHolding()) return;
    const auto Token = TurnState.GetActiveToken();
    if (!TurnState.FinishCapture(Token, FPlatformTime::Seconds()))
    {
        StatusText = FailureText(TurnState.GetLastFailure());
        LastError = StatusText;
        ClearActiveTurn(Token, true);
        PublishState();
        return;
    }
    OnHoldReleased.Broadcast(Token);
    PublishState();
}

/// @brief 检查开麦时记下的玩家和人物是否还对得上：对得上返回 Valid，上下车同步还没到齐时返回 Pending（最多等 2 秒），否则 Invalid
EDeliverySpeechPlayerContextState UDeliverySpeechInputComponent::CheckSessionContext(double Now)
{
    const auto State = SessionContext.Check(Cast<APlayerController>(GetOwner()));
    if (State == EDeliverySpeechPlayerContextState::Invalid) return State;
    // 先检查已开始的等待，长帧后字段刚好补齐也不能复活已超时的会话
    if (ContextPendingSince >= 0 && (!FMath::IsFinite(Now) || Now < ContextPendingSince ||
        Now - ContextPendingSince >= FDeliverySpeechPlayerContext::TransitionGraceSeconds))
        return EDeliverySpeechPlayerContextState::Invalid;
    if (State == EDeliverySpeechPlayerContextState::Valid)
    {
        ContextPendingSince = -1;
        return State;
    }
    if (!FMath::IsFinite(Now)) return EDeliverySpeechPlayerContextState::Invalid;
    if (ContextPendingSince < 0) ContextPendingSince = Now;
    return EDeliverySpeechPlayerContextState::Pending;
}

/// @brief 游戏窗口在前台、键盘焦点在游戏视口上时返回 true
bool UDeliverySpeechInputComponent::HasLocalViewportFocus() const
{
    if (!GetWorld() || !FSlateApplication::IsInitialized()) return false;
    const auto* ViewportClient = GetWorld()->GetGameViewport();
    const auto Viewport = ViewportClient ? ViewportClient->GetGameViewportWidget() : nullptr;
    if (!Viewport) return false;
    const auto& Slate = FSlateApplication::Get();
    if (!Slate.IsActive() || !Viewport->HasAnyUserFocusOrFocusedDescendants()) return false;
    const auto Window = Slate.FindWidgetWindow(Viewport.ToSharedRef());
    return Window.IsValid() && Window == Slate.GetActiveTopLevelWindow();
}

/// @brief 键盘焦点在游戏视口上、不在输入框里时返回 true；在输入框里打字时按字母键不应开始录音
bool UDeliverySpeechInputComponent::CanUsePushToTalkKey() const
{
    if (!HasLocalViewportFocus()) return false;
    // 文本控件内部的子控件也算正在输入文字，UMG 按住按钮不走此限制
    auto Widget = FSlateApplication::Get().GetKeyboardFocusedWidget();
    while (Widget)
    {
        if (Widget->GetTypeAsString().Contains(TEXT("EditableText"))) return false;
        Widget = Widget->GetParentWidget();
    }
    return true;
}

/// @brief 交谈组件允许用语音对 Npc 说话时返回 true
bool UDeliverySpeechInputComponent::CanTalkTo(ADeliveryNpcCharacter* Npc) const
{
    const auto* PC = Cast<APlayerController>(GetOwner());
    if (!PC || !PC->IsLocalController() || !IsValid(Npc) || Npc->GetWorld() != GetWorld()) return false;
    const auto* Conversation = PC->FindComponentByClass<UDeliveryConversationComponent>();
    return Conversation && (bContinuousMode ? Conversation->CanVoiceTalkTo(Npc) : Conversation->CanTalkTo(Npc));
}

/// @brief 订阅音频设备变化（麦克风、扬声器换了），变化时关闭语音
void UDeliverySpeechInputComponent::BindDeviceNotifications()
{
    auto* Notifications = GEngine ? GEngine->GetEngineSubsystem<UAudioDeviceNotificationSubsystem>() : nullptr;
    if (!Notifications || !Bridge) return;
    DeviceNotifications = Notifications;
    const TWeakPtr<FDeliverySpeechInputBridge, ESPMode::ThreadSafe> WeakBridge = Bridge;
    const auto DeviceChanged = [WeakBridge](EAudioDeviceChangedRole, FString)
    {
        if (auto Pinned = WeakBridge.Pin()) Pinned->bDeviceChanged.store(true);
    };
    CaptureDeviceChangedHandle = Notifications->DefaultCaptureDeviceChangedNative.AddLambda(DeviceChanged);
    RenderDeviceChangedHandle = Notifications->DefaultRenderDeviceChangedNative.AddLambda(DeviceChanged);
    DeviceSwitchedHandle = Notifications->DeviceSwitchedNative.AddLambda([WeakBridge](FString)
    {
        if (auto Pinned = WeakBridge.Pin()) Pinned->bDeviceChanged.store(true);
    });
}

/// @brief 取消订阅音频设备变化
void UDeliverySpeechInputComponent::UnbindDeviceNotifications()
{
    if (auto* Notifications = DeviceNotifications.Get())
    {
        Notifications->DefaultCaptureDeviceChangedNative.Remove(CaptureDeviceChangedHandle);
        Notifications->DefaultRenderDeviceChangedNative.Remove(RenderDeviceChangedHandle);
        Notifications->DeviceSwitchedNative.Remove(DeviceSwitchedHandle);
    }
    DeviceNotifications.Reset();
    CaptureDeviceChangedHandle.Reset();
    RenderDeviceChangedHandle.Reset();
    DeviceSwitchedHandle.Reset();
}

/// @brief 关闭语音；世界销毁时等识别线程退出，运行库和模型要在组件之前释放
void UDeliverySpeechInputComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    bEndingPlay = true;
    CloseVoice(TEXT("语音已关闭"));
    // 正常关闭由 Tick 收尾，世界销毁时最后等待线程，确保 DLL 和模型先于组件释放
    if (Bridge)
    {
        Bridge->Recognizer->Stop();
        Bridge.Reset();
    }
    Super::EndPlay(EndPlayReason);
}

/// @brief 状态变了时广播 OnStateChanged，再广播 OnDisplayChanged
void UDeliverySpeechInputComponent::PublishState()
{
    const auto State = TurnState.GetState();
    if (PublishedState != State)
    {
        PublishedState = State;
        OnStateChanged.Broadcast(State);
    }
    OnDisplayChanged.Broadcast();
}

/// @brief 当前状态的名字，调试用
FName UDeliverySpeechInputComponent::GetVoiceStateName() const
{
    switch (TurnState.GetState())
    {
    case EDeliverySpeechInputState::Preparing: return TEXT("Preparing");
    case EDeliverySpeechInputState::Ready: return TEXT("Ready");
    case EDeliverySpeechInputState::Capturing: return TEXT("Capturing");
    case EDeliverySpeechInputState::Finalizing: return TEXT("Finalizing");
    case EDeliverySpeechInputState::Error: return TEXT("Error");
    default: return TEXT("Off");
    }
}

/// @brief 出错原因对应的提示文字
FString UDeliverySpeechInputComponent::FailureText(EDeliverySpeechTurnFailure Failure) const
{
    switch (Failure)
    {
    case EDeliverySpeechTurnFailure::CaptureTimeout: return TEXT("超过15秒，本次录音已取消");
    case EDeliverySpeechTurnFailure::FinalTimeout: return TEXT("识别超时，请重试");
    case EDeliverySpeechTurnFailure::EmptyText: return TEXT("没有听清，请重试");
    case EDeliverySpeechTurnFailure::TextTooLong: return TEXT("这句话过长，请分开说");
    case EDeliverySpeechTurnFailure::AudioFailure: return TEXT("音频中断，请检查设备后重新开启语音");
    case EDeliverySpeechTurnFailure::RecognitionFailure: return TEXT("识别失败，请重新开启语音");
    default: return TEXT("本次录音已取消，请重试");
    }
}
