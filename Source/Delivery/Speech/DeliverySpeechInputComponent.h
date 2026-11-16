#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Speech/Recognition/DeliverySpeechTurn.h"
#include "Speech/Capture/DeliverySpeechAudioCapture.h"
#include "Speech/DeliverySpeechPlayerContext.h"
#include "DeliverySpeechInputComponent.generated.h"

class ADeliveryNpcCharacter;
class APawn;
class UAudioDeviceNotificationSubsystem;
struct FDeliverySpeechInputBridge;

/// 本机开始录一句话，参数是这一句的编号和说给哪位 NPC
DECLARE_MULTICAST_DELEGATE_TwoParams(FDeliverySpeechTurnStarted,
    const FDeliverySpeechTurnToken&, ADeliveryNpcCharacter*);

/// 这一句被取消，不会再有识别结果
DECLARE_MULTICAST_DELEGATE_OneParam(FDeliverySpeechTurnCancelled, const FDeliverySpeechTurnToken&);

/// 这一句录完了（按住说话时松开，或连麦时检测到说完），之后等识别结果
DECLARE_MULTICAST_DELEGATE_OneParam(FDeliverySpeechHoldReleased, const FDeliverySpeechTurnToken&);

/// 这一句识别完成，参数是编号、说给哪位 NPC、识别出的文字；交谈组件收到后提交给服务器
DECLARE_MULTICAST_DELEGATE_ThreeParams(FDeliverySpeechFinalText,
    const FDeliverySpeechTurnToken&, ADeliveryNpcCharacter*, const FString&);

/// 语音状态变化（关闭、准备中、就绪、录音中、出错）
DECLARE_MULTICAST_DELEGATE_OneParam(FDeliverySpeechStateChanged, EDeliverySpeechInputState);

/// 界面要显示的内容变了（状态文字、识别中的半句），语音状态图标和交谈面板收到后刷新
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDeliverySpeechDisplayChanged);

/**
 * @brief 本机麦克风和语音识别，挂在玩家控制器上，默认关闭；目前只用来和二狗连麦
 *
 * EnableVoice 打开麦克风（FDeliverySpeechAudioCapture）和本机识别线程（FDeliverySpeechRecognizer）
 * 连麦模式下音频线程检测到人声后，TickComponent 调用 BeginDetectedSpeech 开始录一句；说完后识别线程给出整句文字，
 * TickComponent 取到后广播 OnFinalText，交谈组件把文字提交给服务器
 * 每帧还会检查：窗口失去焦点、换了控制的人物、音频设备变了、和二狗不能说话了，就关掉语音
 * 音频线程和识别线程只碰 FDeliverySpeechInputBridge 里的普通数据，不碰 UObject；关闭时先停音频，再等识别线程退出
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliverySpeechInputComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    // ── 开关 ──

    /// @brief 默认不每帧更新，EnableVoice 成功后才开始
    UDeliverySpeechInputComponent();

    /// @brief 析构时如果识别线程还在，等它退出
    virtual ~UDeliverySpeechInputComponent() override;

    /// @brief 打开和二狗的连麦：检查窗口焦点、已经结识二狗、能对它说话，再启动识别线程和麦克风
    /// @return true 表示开始准备，准备好后状态变成 Ready；之后检测到人声才开始录一句
    UFUNCTION(BlueprintCallable, Category="Delivery|Speech")
    bool EnableVoice();

    /// @brief 关闭语音，调用 CloseVoice
    UFUNCTION(BlueprintCallable, Category="Delivery|Speech")
    void DisableVoice();

    // ── 按住说话 ──
    // 旧的按住说话模式：按下调用 BeginHold，松开调用 FinishHold；字母键要先检查 CanUsePushToTalkKey，按钮可以直接调用

    /// @brief 开始对 Npc 录一句话；语音没准备好、已经在录或不能对它说话时返回 false
    UFUNCTION(BlueprintCallable, Category="Delivery|Speech")
    bool BeginHold(ADeliveryNpcCharacter* Npc);

    /// @brief 松开：这一句录完，等识别结果
    UFUNCTION(BlueprintCallable, Category="Delivery|Speech")
    void FinishHold();

    /// @brief 取消正在录的这一句
    UFUNCTION(BlueprintCallable, Category="Delivery|Speech")
    void CancelHold();

    /// @brief 键盘焦点在游戏视口上、不在输入框里时返回 true；在输入框里打字时按字母键不应开始录音
    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    bool CanUsePushToTalkKey() const;

    // ── 查询 ──

    /// @brief 语音是否开着
    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    bool IsVoiceEnabled() const { return bVoiceEnabled; }

    /// @brief 是否开着连麦（人声检测自动录音）
    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    bool IsContinuousVoiceEnabled() const { return bVoiceEnabled && bContinuousMode; }

    /// @brief 连麦的对象（二狗）
    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    ADeliveryNpcCharacter* GetConnectedNpc() const;

    /// @brief 麦克风和识别都准备好了，可以开始录
    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    bool IsVoiceReady() const { return TurnState.GetState() == EDeliverySpeechInputState::Ready; }

    /// @brief 正在录一句
    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    bool IsHolding() const { return TurnState.GetState() == EDeliverySpeechInputState::Capturing; }

    /// @brief 当前状态的名字，调试用
    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    FName GetVoiceStateName() const;

    /// @brief 给界面显示的状态文字
    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    FText GetStatusText() const { return FText::FromString(StatusText); }

    /// @brief 按住说话时识别出的半句
    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    FText GetPartialText() const { return FText::FromString(PartialText); }

    /// @brief 最近一次出错的文字
    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    FText GetLastError() const { return FText::FromString(LastError); }

    /// @brief 服务器拒收编号为 Token 的那一句时调用：只取消那一句，已经开始的下一句不受影响
    bool ReportTurnRejected(const FDeliverySpeechTurnToken& Token, FName Reason);

    /// @brief 当前状态
    EDeliverySpeechInputState GetState() const { return TurnState.GetState(); }

    /// @brief 正在录的这一句的编号
    const FDeliverySpeechTurnToken& GetActiveToken() const { return TurnState.GetActiveToken(); }

    // ── 通知 ──
    FDeliverySpeechTurnStarted OnTurnStarted;
    FDeliverySpeechTurnCancelled OnTurnCancelled;
    FDeliverySpeechHoldReleased OnHoldReleased;
    FDeliverySpeechFinalText OnFinalText;
    FDeliverySpeechStateChanged OnStateChanged;

    UPROPERTY(BlueprintAssignable, Category="Delivery|Speech")
    FDeliverySpeechDisplayChanged OnDisplayChanged;

    // ── 配置 ──

    /// 识别运行库的目录，里面放 DLL 和 models；相对路径以项目目录为基准
    UPROPERTY(EditDefaultsOnly, Category="Delivery|Speech")
    FString RuntimeDirectory = TEXT("Binaries/ThirdParty/DeliverySpeech");

    /// 热词，每行一个，让识别更容易认出这些词；不改识别结果，也不做意图匹配
    UPROPERTY(EditDefaultsOnly, Category="Delivery|Speech", meta=(MultiLine=true))
    FString Hotwords = TEXT("二狗\n小绿\n斧头\n快递");

protected:
    // ── 生命周期 ──

    /// @brief 关闭语音；世界销毁时等识别线程退出，运行库和模型要在组件之前释放
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /// @brief 语音开着时每帧：检查焦点、控制关系、音频设备和二狗，不对就关闭；处理录完的一句、取识别结果广播 OnFinalText；
    /// 连麦就绪且上一句已经提交完时，取出音频线程检测到的人声，调用 BeginDetectedSpeech 开始录下一句
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

private:
    friend class FDeliveryConversationVoiceKeyTest;
    friend class FDeliverySpeechVehicleContextTest;

    // ── 内部 ──

    /// @brief 游戏窗口在前台、键盘焦点在游戏视口上时返回 true
    bool HasLocalViewportFocus() const;

    /// @brief 检查开麦时记下的玩家和人物是否还对得上：对得上返回 Valid，上下车同步还没到齐时返回 Pending（最多等 2 秒），否则 Invalid
    EDeliverySpeechPlayerContextState CheckSessionContext(double Now);

    /// @brief 交谈组件允许用语音对 Npc 说话时返回 true
    bool CanTalkTo(ADeliveryNpcCharacter* Npc) const;

    /// @brief 音频线程检测到人声（DetectionId）后，开始对二狗录一句
    bool BeginDetectedSpeech(uint64 DetectionId);

    /// @brief 连麦时检查音频线程是否已经报告这一句说完，说完就进入等识别结果
    void PollContinuousEnd();

    /// @brief 连麦中的状态文字，例如与二狗连麦中
    FString ConnectedStatus() const;

    /// @brief 关闭语音：先停麦克风和识别线程，再取消正在录的一句并通知；Message 作为状态文字
    void CloseVoice(const FString& Message);

    /// @brief 出错关闭：记下错误，再调用 CloseVoice
    void FailSession(EDeliverySpeechTurnFailure Failure, const FString& Message);

    /// @brief 停止麦克风，让识别线程停止接收；识别线程退出后由 CollectStoppedRecognizer 释放
    void StopSessionResources();

    /// @brief 识别线程已经退出时释放它
    void CollectStoppedRecognizer();

    /// @brief 清掉编号为 Token 的那一句；bNotifyCancelled 为 true 时广播 OnTurnCancelled
    void ClearActiveTurn(const FDeliverySpeechTurnToken& Token, bool bNotifyCancelled);

    /// @brief 订阅音频设备变化（麦克风、扬声器换了），变化时关闭语音
    void BindDeviceNotifications();

    /// @brief 取消订阅音频设备变化
    void UnbindDeviceNotifications();

    /// @brief 状态变了时广播 OnStateChanged，再广播 OnDisplayChanged
    void PublishState();

    /// @brief 出错原因对应的提示文字
    FString FailureText(EDeliverySpeechTurnFailure Failure) const;

    // ── 状态 ──

    /// 一句话的状态机：准备、就绪、录音、等识别、出错
    FDeliverySpeechTurnState TurnState;

    /// 麦克风采集
    TUniquePtr<FDeliverySpeechAudioCapture> AudioCapture;

    /// 和音频线程、识别线程共用的数据；每次开麦新建一个
    TSharedPtr<FDeliverySpeechInputBridge, ESPMode::ThreadSafe> Bridge;

    /// 开麦时记下的玩家和人物
    FDeliverySpeechPlayerContext SessionContext;

    /// CheckSessionContext 开始返回 Pending 的时间
    double ContextPendingSince = -1;

    /// 正在录的这一句说给谁
    TWeakObjectPtr<ADeliveryNpcCharacter> ActiveNpc;

    /// 连麦的对象
    TWeakObjectPtr<ADeliveryNpcCharacter> ConnectedNpc;

    /// 音频设备变化通知
    TWeakObjectPtr<UAudioDeviceNotificationSubsystem> DeviceNotifications;
    FDelegateHandle CaptureDeviceChangedHandle;
    FDelegateHandle RenderDeviceChangedHandle;
    FDelegateHandle DeviceSwitchedHandle;

    /// 下一句交给识别线程的编号，只增不减，取消的旧句编号不会被新句重用
    uint64 NextNativeTurnId = 0;

    /// 正在录的这一句在识别线程里的编号
    uint64 ActiveNativeTurnId = 0;

    /// 开麦时的音频设备编号，变了就关闭语音
    uint32 AudioDeviceId = 0;

    /// 语音开着
    bool bVoiceEnabled = false;

    /// 是连麦模式（人声检测自动录音），不是按住说话
    bool bContinuousMode = false;

    /// 识别线程已经加载好模型
    bool bRecognizerReady = false;

    /// 这一句已经广播过 OnTurnStarted
    bool bTurnStarted = false;

    /// EndPlay 已经开始
    bool bEndingPlay = false;

    /// 上次广播的状态
    EDeliverySpeechInputState PublishedState = EDeliverySpeechInputState::Off;

    /// 给界面的状态文字
    FString StatusText = TEXT("语音已关闭");

    /// 识别中的半句
    FString PartialText;

    /// 最近一次出错的文字
    FString LastError;
};
