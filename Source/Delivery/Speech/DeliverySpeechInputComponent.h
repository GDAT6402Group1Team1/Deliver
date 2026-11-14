#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Speech/DeliverySpeechTurn.h"
#include "Speech/DeliverySpeechAudioCapture.h"
#include "DeliverySpeechInputComponent.generated.h"

class ADeliveryNpcCharacter;
class APawn;
class UAudioDeviceNotificationSubsystem;
struct FDeliverySpeechInputBridge;

DECLARE_MULTICAST_DELEGATE_TwoParams(FDeliverySpeechTurnStarted,
    const FDeliverySpeechTurnToken&, ADeliveryNpcCharacter*);
DECLARE_MULTICAST_DELEGATE_OneParam(FDeliverySpeechTurnCancelled, const FDeliverySpeechTurnToken&);
DECLARE_MULTICAST_DELEGATE_OneParam(FDeliverySpeechHoldReleased, const FDeliverySpeechTurnToken&);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FDeliverySpeechFinalText,
    const FDeliverySpeechTurnToken&, ADeliveryNpcCharacter*, const FString&);
DECLARE_MULTICAST_DELEGATE_OneParam(FDeliverySpeechStateChanged, EDeliverySpeechInputState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDeliverySpeechDisplayChanged);

// 玩家控制器持有本机麦克风和识别会话，交谈组件监听最终文字并执行服务器请求
// 默认关闭，音频线程只访问普通共享数据，退出时先停止音频再等待识别线程收尾
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliverySpeechInputComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UDeliverySpeechInputComponent();
    virtual ~UDeliverySpeechInputComponent() override;

    // true 表示已开始准备持续连麦，人声确认后才建立一轮交谈
    UFUNCTION(BlueprintCallable, Category="Delivery|Speech")
    bool EnableVoice();

    UFUNCTION(BlueprintCallable, Category="Delivery|Speech")
    void DisableVoice();

    // UI 或瞄准逻辑传入目标，按钮可直接调用，字母键先检查 CanUsePushToTalkKey
    UFUNCTION(BlueprintCallable, Category="Delivery|Speech")
    bool BeginHold(ADeliveryNpcCharacter* Npc);

    UFUNCTION(BlueprintCallable, Category="Delivery|Speech")
    void FinishHold();

    UFUNCTION(BlueprintCallable, Category="Delivery|Speech")
    void CancelHold();

    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    bool CanUsePushToTalkKey() const;

    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    bool IsVoiceEnabled() const { return bVoiceEnabled; }

    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    bool IsContinuousVoiceEnabled() const { return bVoiceEnabled && bContinuousMode; }

    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    ADeliveryNpcCharacter* GetConnectedNpc() const;

    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    bool IsVoiceReady() const { return TurnState.GetState() == EDeliverySpeechInputState::Ready; }

    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    bool IsHolding() const { return TurnState.GetState() == EDeliverySpeechInputState::Capturing; }

    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    FName GetVoiceStateName() const;

    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    FText GetStatusText() const { return FText::FromString(StatusText); }

    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    FText GetPartialText() const { return FText::FromString(PartialText); }

    UFUNCTION(BlueprintPure, Category="Delivery|Speech")
    FText GetLastError() const { return FText::FromString(LastError); }

    // 服务器拒绝只取消它对应的本机轮次，旧答复不能中断下一轮
    bool ReportTurnRejected(const FDeliverySpeechTurnToken& Token, FName Reason);

    EDeliverySpeechInputState GetState() const { return TurnState.GetState(); }
    const FDeliverySpeechTurnToken& GetActiveToken() const { return TurnState.GetActiveToken(); }

    FDeliverySpeechTurnStarted OnTurnStarted;
    FDeliverySpeechTurnCancelled OnTurnCancelled;
    FDeliverySpeechHoldReleased OnHoldReleased;
    FDeliverySpeechFinalText OnFinalText;
    FDeliverySpeechStateChanged OnStateChanged;

    UPROPERTY(BlueprintAssignable, Category="Delivery|Speech")
    FDeliverySpeechDisplayChanged OnDisplayChanged;

    // 相对路径以项目目录为基准，目录同时包含 DLL 和 models
    UPROPERTY(EditDefaultsOnly, Category="Delivery|Speech")
    FString RuntimeDirectory = TEXT("Binaries/ThirdParty/DeliverySpeech");

    // 每行一个词，只影响识别词表，不执行意图匹配或修改最终文字
    UPROPERTY(EditDefaultsOnly, Category="Delivery|Speech", meta=(MultiLine=true))
    FString Hotwords = TEXT("二狗\n小绿\n斧头\n快递");

protected:
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

private:
    friend class FDeliveryConversationVoiceKeyTest;
    bool HasLocalViewportFocus() const;
    bool CanTalkTo(ADeliveryNpcCharacter* Npc) const;
    bool BeginDetectedSpeech(uint64 DetectionId);
    void PollContinuousEnd();
    FString ConnectedStatus() const;
    void CloseVoice(const FString& Message);
    void FailSession(EDeliverySpeechTurnFailure Failure, const FString& Message);
    void StopSessionResources();
    void CollectStoppedRecognizer();
    void ClearActiveTurn(const FDeliverySpeechTurnToken& Token, bool bNotifyCancelled);
    void BindDeviceNotifications();
    void UnbindDeviceNotifications();
    void PublishState();
    FString FailureText(EDeliverySpeechTurnFailure Failure) const;

    FDeliverySpeechTurnState TurnState;
    TUniquePtr<FDeliverySpeechAudioCapture> AudioCapture;
    TSharedPtr<FDeliverySpeechInputBridge, ESPMode::ThreadSafe> Bridge;
    TWeakObjectPtr<APawn> SessionPawn;
    TWeakObjectPtr<ADeliveryNpcCharacter> ActiveNpc;
    TWeakObjectPtr<ADeliveryNpcCharacter> ConnectedNpc;
    TWeakObjectPtr<UAudioDeviceNotificationSubsystem> DeviceNotifications;
    FDelegateHandle CaptureDeviceChangedHandle;
    FDelegateHandle RenderDeviceChangedHandle;
    FDelegateHandle DeviceSwitchedHandle;
    uint64 NextNativeTurnId = 0;
    uint64 ActiveNativeTurnId = 0;
    uint32 AudioDeviceId = 0;
    bool bVoiceEnabled = false;
    bool bContinuousMode = false;
    bool bRecognizerReady = false;
    bool bTurnStarted = false;
    bool bEndingPlay = false;
    EDeliverySpeechInputState PublishedState = EDeliverySpeechInputState::Off;
    FString StatusText = TEXT("语音已关闭");
    FString PartialText;
    FString LastError;
};
