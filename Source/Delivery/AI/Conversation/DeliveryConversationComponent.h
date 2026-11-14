#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AI/Conversation/DeliveryConversationTurnSubsystem.h"
#include "Speech/DeliverySpeechTurn.h"
#include "AI/Decision/DeliveryNpcDecisionTrace.h"
#include "TimerManager.h"
#include "DeliveryConversationComponent.generated.h"
class ADeliveryNpcCharacter;
class UDeliverySpeechInputComponent;
class UDeliveryNpcAgentComponent;

DECLARE_MULTICAST_DELEGATE_TwoParams(FDeliveryConversationSubmissionBound, FGuid, FGuid);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FDeliveryConversationSubmitted,
    ADeliveryNpcCharacter*, FGuid, const FString&);
DECLARE_MULTICAST_DELEGATE_OneParam(FDeliveryConversationVoiceError, FName);

// 玩家控制器持有这个组件，负责将玩家的话发送到服务器
// 服务器检查玩家与 NPC 的距离、遮挡和发送间隔，通过后才交给 NPC
UCLASS(ClassGroup=(Delivery))
class DELIVERY_API UDeliveryConversationComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryConversationComponent();
    void BindInput();
    void SetConversationTarget(ADeliveryNpcCharacter* Resident);
    void ClearConversationTarget(ADeliveryNpcCharacter* Resident);
    UDeliverySpeechInputComponent* GetSpeechInput() const;
    // 连麦只连接当前玩家自己的同伴，文字面板与镜头不会改变连接对象
    ADeliveryNpcCharacter* ResolveVoiceCompanion() const;
    bool CanVoiceTalkTo(const ADeliveryNpcCharacter* Resident) const;
    // 上一句 final 收到服务器回执后，连续麦克风再领取下一句
    bool HasPendingVoiceSubmission() const { return LocalVoiceToken.IsValid() && bVoiceTextReady; }
    // 服务器签发的语音来源保留至回复结束，供远程队友声音识别归属
    bool IsVoiceTurn(const FDeliveryConversationTurnHandle& Handle) const;
    bool IsVoiceReplyMessage(FGuid MessageId,const ADeliveryNpcCharacter* ExpectedNpc=nullptr) const;
    UPROPERTY(EditDefaultsOnly,Category="Delivery|Voice",meta=(ClampMin="0",ClampMax="8"))
    int32 VoiceOverhearLimit=2;
    FDeliveryConversationSubmissionBound OnSubmissionBound;
    FDeliveryConversationSubmitted OnSubmitted;
    FDeliveryConversationVoiceError OnVoiceInputError;
    // 演示面板只通过拥有此组件的玩家接收当前世界的观察快照
    void WatchNpcDiagnostics(ADeliveryNpcCharacter* Resident);
    void StopWatchingNpcDiagnostics();
    FDeliveryNpcDecisionTraceChanged OnDiagnosticsChanged;
    FName GetVoiceInputError() const { return FPlatformTime::Seconds()<VoiceErrorExpiresAt ? LastVoiceInputError : NAME_None; }
    // 返回 true 表示消息已发出，接收结果由服务器另行通知
    UFUNCTION(BlueprintCallable) bool Submit(ADeliveryNpcCharacter* Resident,const FString& Text);
    UFUNCTION(Client,Reliable) void ClientOpenConversation(ADeliveryNpcCharacter* Resident);
    UFUNCTION(Server,Reliable) void ServerOpenConversation(ADeliveryNpcCharacter* Resident,uint64 FacingSequence=0,APawn* FacingPawn=nullptr);
    UFUNCTION(Server,Reliable) void ServerSubmit(ADeliveryNpcCharacter* Resident,const FString& Text,FGuid SpeechId,uint64 InputSequence,uint64 FacingSequence=0,APawn* FacingPawn=nullptr);
    UFUNCTION(Client,Reliable) void ClientSubmissionBound(FGuid ClientToken,FGuid MessageId);
    UFUNCTION(Client,Reliable) void ClientResult(FName Result,FGuid SpeechId);
    // 玩家刚走到 NPC 附近时，通过自己的控制器请求正在播放的语音
    UFUNCTION(Server,Reliable) void ServerRequestNpcAudio(ADeliveryNpcCharacter* Npc,FGuid TurnId,int32 Offset=0);
    UFUNCTION(Client,Reliable) void ClientReceiveNpcAudio(ADeliveryNpcCharacter* Npc,FGuid TurnId,int32 Offset,int32 Total,const TArray<uint8>& Data);
    UFUNCTION(BlueprintPure) FName GetLastResult() const { return FPlatformTime::Seconds()<ResultExpiresAt ? LastResult : NAME_None; }
    FGuid GetLastSubmissionId() const { return LastSubmissionId; }
    // 交谈窗口按发言编号读取结果，短提示消失后仍能说明这句话有没有送到
    FName GetSubmissionResult(FGuid SpeechId) const { return SpeechId.IsValid() && SpeechId==LastSubmissionId ? LastResult : NAME_None; }
    bool CanSendNow() const { return FPlatformTime::Seconds()>=NextLocalSendAt; }
    bool CanTalkTo(const ADeliveryNpcCharacter* Resident) const;
    // 选择本机镜头前方可交谈的人物，优先靠近画面中心的人；范围内无人时返回空
    ADeliveryNpcCharacter* GetAimedNpc() const;
    // 快捷发言点明现场另一位 NPC，具体反应仍交给模型决定
    FString GetProvokeText(const ADeliveryNpcCharacter* Npc) const;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime,ELevelTick TickType,FActorComponentTickFunction* ThisTickFunction) override;
private:
    void BindInspectorInput();
    void ToggleInspector();
    void ShutdownInspector();
    void UnbindServerInspector();
    bool CanInspectNpc(const ADeliveryNpcCharacter* Resident) const;
    void HandleInspectorTrace(const FDeliveryNpcDecisionTrace& Trace);
    UFUNCTION(Server,Reliable) void ServerWatchNpcDiagnostics(ADeliveryNpcCharacter* Resident,uint64 Generation);
    UFUNCTION(Client,Reliable) void ClientNpcDiagnostics(uint64 Generation,const FDeliveryNpcDecisionTrace& Trace);
    uint64 LocalInspectorGeneration=0;
    uint64 ServerInspectorGeneration=0;
    TWeakObjectPtr<ADeliveryNpcCharacter> LocalInspectorTarget;
    TWeakObjectPtr<ADeliveryNpcCharacter> ServerInspectorTarget;
    TWeakObjectPtr<APawn> LocalInspectorPawn;
    TWeakObjectPtr<APawn> ServerInspectorPawn;
    FGuid InspectorWorldEpoch;
    int32 InspectorRevision=-1;
    FDelegateHandle InspectorTraceHandle;
    TWeakObjectPtr<class UEnhancedInputComponent> BoundInspectorInput;
    TWeakObjectPtr<class UEnhancedInputLocalPlayerSubsystem> InspectorInputSubsystem;
    TWeakObjectPtr<class UInputMappingContext> InspectorMapping;
    uint32 InspectorInputHandle=0;
    bool bOwnsInspectorMapping=false;
    UFUNCTION(Server,Reliable) void ServerBeginVoice(ADeliveryNpcCharacter* Resident,FGuid ClientToken,uint64 InputSequence,uint64 FacingSequence=0,APawn* FacingPawn=nullptr);
    UFUNCTION(Server,Reliable) void ServerFinishVoice(FGuid ClientToken,uint64 InputSequence);
    UFUNCTION(Server,Reliable) void ServerCancelVoice(FGuid ClientToken,uint64 InputSequence);
    UFUNCTION(Server,Reliable) void ServerSubmitVoice(ADeliveryNpcCharacter* Resident,const FString& Text,FDeliveryConversationTurnHandle Handle);
    UFUNCTION(Client,Reliable) void ClientVoiceGranted(FGuid ClientToken,uint64 InputSequence,FDeliveryConversationTurnHandle Handle);
    UFUNCTION(Client,Reliable) void ClientVoiceRejected(FGuid ClientToken,uint64 InputSequence,FName Reason);
    void BindSpeechInput();
    void StartVoiceFromKey();
    void FinishVoiceFromKey();
    void CancelVoiceKeyHold();
    void CancelVoiceFromKey();
    void HandleVoiceStarted(const FDeliverySpeechTurnToken& Token,ADeliveryNpcCharacter* Resident);
    void HandleVoiceReleased(const FDeliverySpeechTurnToken& Token);
    void HandleVoiceCancelled(const FDeliverySpeechTurnToken& Token);
    void HandleVoiceText(const FDeliverySpeechTurnToken& Token,ADeliveryNpcCharacter* Resident,const FString& Text);
    void SubmitReadyVoiceText();
    void ResetLocalVoice();
    void RejectLocalVoice(FName Reason,bool bNotifyServer);
    void ResetServerVoice(const FDeliveryConversationTurnHandle& ExpectedHandle);
    void FlushDeferredVoiceSubmission();
    void ClearDeferredVoiceSubmission();
    uint64 NextInputSequence();
    // 朝向序号独立于发言轮次，打开窗口不改变语音、回复或去重状态
    uint64 PrepareConversationFacingInput();
    void RegisterConversationFacingInput(ADeliveryNpcCharacter* Resident,FGuid ClientToken,uint64 Sequence,APawn* PawnAtInput);
    void ConfirmConversationFacingInput(ADeliveryNpcCharacter* Resident,FGuid ClientToken,uint64 Sequence=0);
    void CancelConversationFacingInput();
    uint64 LastLocalFacingSequence=0;
    uint64 LastServerFacingSequence=0;
    FGuid ServerFacingClientToken;
    TWeakObjectPtr<class APawn> LocalFacingPawn;
    TWeakObjectPtr<class APawn> ServerFacingPawn;
    TWeakObjectPtr<ADeliveryNpcCharacter> ServerFacingTarget;
    void WatchReplies(ADeliveryNpcCharacter* Resident);
    void HandleLeaseExpired(const FDeliveryConversationTurnLease& Lease,EDeliveryConversationLeaseEnd Reason);
    void CancelReplacedLeases(const TArray<FDeliveryConversationTurnLease>& Leases);
    void CancelNpcMessageTurns(ADeliveryNpcCharacter* Resident);
    bool DeliverFinalText(ADeliveryNpcCharacter* Resident,const FString& Text,
        const FDeliveryConversationTurnHandle& Handle);
    void OpenAimedConversation();
    ADeliveryNpcCharacter* FindNpcInView(const FVector& ViewLocation,const FVector& ViewDirection) const;
    friend class FDeliveryNpcConversationAimTest;
    friend class FDeliveryConversationFeedbackTest;
    friend class FDeliveryConversationVoiceBoundaryTest;
    friend class FDeliveryConversationVoiceKeyTest;
    double NextSendAt=0;
    double NextAudioRequestAt=0;
    // 快捷按钮和文本框使用相同的发送间隔，服务器再检查一次
    double NextLocalSendAt=0;
    // 用发言编号找到对应回复，交谈距离和遮挡由服务器检查
    FGuid LastSubmissionId;
    FName LastResult;
    // 按现实经过的时间关闭提示，游戏暂停时继续计时
    double ResultExpiresAt=0;
    TWeakObjectPtr<ADeliveryNpcCharacter> ConversationTarget;
    TWeakObjectPtr<UDeliverySpeechInputComponent> BoundSpeech;
    TSet<TWeakObjectPtr<UDeliveryNpcAgentComponent>> WatchedAgents;
    TWeakObjectPtr<UDeliveryConversationTurnSubsystem> BoundTurns;
    TArray<uint32> VoiceInputBindingHandles;
    TWeakObjectPtr<class UEnhancedInputComponent> BoundVoiceInput;
    TWeakObjectPtr<class UInputMappingContext> VoiceMapping;
    TWeakObjectPtr<class UEnhancedInputLocalPlayerSubsystem> VoiceInputSubsystem;
    TWeakObjectPtr<class UEnhancedInputComponent> BoundTalkInput;
    uint32 TalkInputBindingHandle=0;
    bool bOwnsVoiceMapping=false;
    bool bEndingPlay=false;
    uint64 LastLocalInputSequence=0;
    FDeliverySpeechTurnToken LocalVoiceToken;
    FDeliverySpeechTurnToken KeyVoiceToken;
    uint64 LocalVoiceSequence=0;
    FDeliveryConversationTurnHandle LocalVoiceHandle;
    TWeakObjectPtr<ADeliveryNpcCharacter> LocalVoiceTarget;
    FDeliverySpeechPlayerContext LocalVoiceContext;
    double LocalVoiceContextPendingSince=-1;
    FString FinalVoiceText;
    bool bVoiceTextReady=false;
    bool bVoiceSubmissionSent=false;
    bool bVoiceReleased=false;
    double LocalVoiceDeadline=0;
    FName LastVoiceInputError;
    double VoiceErrorExpiresAt=0;
    // 服务器只保留此 PC 最新的录音请求，松键和取消都核对原客户端编号与序号
    FGuid ServerVoiceClientToken;
    FDeliveryConversationTurnHandle ServerVoiceHandle;
    TWeakObjectPtr<ADeliveryNpcCharacter> ServerVoiceTarget;
    FTimerHandle VoiceSubmitTimer;
    FDeliveryConversationTurnHandle DeferredVoiceHandle;
    TWeakObjectPtr<ADeliveryNpcCharacter> DeferredVoiceTarget;
    FString DeferredVoiceText;
    struct FVoiceTurn
    {
        FDeliveryConversationTurnHandle Handle;
        TWeakObjectPtr<ADeliveryNpcCharacter> Npc;
    };
    TMap<FGuid,FVoiceTurn> VoiceTurns;
    mutable TWeakObjectPtr<ADeliveryNpcCharacter> CachedVoiceCompanion;
    void PruneVoiceTurns();
    static constexpr int32 MaxVoiceTurns=8;
    friend class FDeliveryConversationVoiceTargetTest;
    friend class FDeliveryConversationVehicleVoiceTest;
    friend class FDeliveryNpcVoiceLinkTest;
    friend class FDeliveryAiInspectorLifecycleTest;
};
