#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "AI/Decision/DeliveryNpcTypes.h"
#include "AI/Decision/DeliveryNpcDecisionTrace.h"
#include "DeliveryNpcAgentComponent.generated.h"
class APlayerController;
class APlayerState;
class ADeliveryNpcCharacter;
class UDeliveryConversationTurnSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryNpcStateChanged, const FDeliveryNpcPublicState&, State);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryNpcDecisionFinished, FName, Reason);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FDeliveryNpcPlayerReplyEnded,APlayerController*,FGuid,FName);

// 保存 NPC 看到、听到的事件，向模型请求回复，并记录当前动作
// Gateway 负责发送请求，行为树负责执行通过检查的动作
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryNpcAgentComponent : public UActorComponent
{
    GENERATED_BODY()
    friend class FDeliveryNpcConversationContextTest;
    friend class FDeliveryAiContextTest;
    friend class FDeliveryCompanionRulesTest;
    friend class FDeliveryCompanionErrandTest;
    friend class FDeliveryConversationFeedbackTest;
    friend class FDeliveryNpcConversationOcclusionTest;
    friend class FDeliveryNpcStrikeApproachTest;
    friend class FDeliveryNpcStrikeNavigationTest;
    friend class FDeliveryNpcBodyAnimationTest;
    friend class FDeliveryNpcFightTest;
    friend class FDeliveryNpcSpeechStreamTest;
    friend class FDeliveryResidentIntegrationTest;
    friend class FDeliveryResidentPIECommand;
    friend class FDeliveryNpcAudioStreamingTest;
    friend class FDeliveryNpcLiveAudioCommand;
    friend class FDeliveryNpcSentenceAudioProtocolTest;
    friend class FDeliveryNpcModelSpeechSeparationTest;
    friend class FDeliveryNpcPlayerInterruptionTest;
public:
    UDeliveryNpcAgentComponent();
    // 诊断只读最近一次实际请求，订阅者不得用它推进玩法
    const FDeliveryNpcDecisionTrace& GetDecisionTrace() const { return DecisionTrace; }
    FDeliveryNpcDecisionTraceChanged OnDecisionTraceChanged;

    // 在服务器记录一个事件，最近 128 个事件中相同编号只接收一次
    // 返回 true 表示记录成功，还没有向模型发送请求
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI")
    bool ObserveEvent(const FDeliveryNpcObservation& Observation);
    // 返回 true 表示已安排请求；如果正在等待回复，会在当前请求结束后处理新信息
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI") bool RequestDecision();
    // 停止等待当前回复，后端已经开始的计算不一定同时停止
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI") void CancelDecision();
    // 同时取消请求和当前动作，不撤销已经发生的命中
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI") void CancelPlan(FName Reason);
    // 同伴更换归属时清掉旧主人的对话与经历，旧请求同时失效
    void ResetContext(FName Reason);
    // 服务器凭真实事件安排短轮护主，返回 true 仅表示已接收，命中仍由战斗确认
    // 角色负责核验主人与规则代次，Agent 核验事件和目标，模型不能调用此入口
    bool RequestGameplayRuleAction(EDeliveryNpcAction Action,AActor* Target,int32 RuleRevision,FGuid EvidenceId,float Duration);

    // 行为树开始动作前再次检查目标，返回 true 后才能执行
    // 动作结束时使用同一 ActionId 报告结果，防止旧回调结束新动作
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI")
    bool StartAction(FDeliveryNpcPublicState& OutAction);
    // 记录当前动作的结果；动作编号不符或已结束时返回 false
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI")
    bool TryFinishAction(FGuid ActionId, EDeliveryNpcActionStatus Result, FName Reason);
    UFUNCTION(BlueprintPure, Category="Delivery|AI") FDeliveryNpcPublicState GetPublicState() const { return PublicState; }
    // 当前回复沿用请求发出时的接话轮数，之后听到的话留给下一次请求
    int32 GetActionConversationDepth() const { return ActionConversationDepth; }
    UFUNCTION(BlueprintPure, Category="Delivery|AI") bool HasActiveAction() const;
    // 只读当前持续安排，短身体反应结束后让位给原来的执行分支
    bool HasCompanionActivity() const;
    // 模型交战和逃跑可在执行中请求下一步，服务器护主规则不走这个入口
    bool CanUpdateRunningAction() const;
    double GetActionTimeRemaining() const;
    // 已校验的新攻击替换旧目标时省去整场冷却，普通结束仍保留冷却
    bool IsReplacingFight() const { return bReplacingFight; }
    UFUNCTION(BlueprintPure, Category="Delivery|AI") bool IsDecisionPending() const { return bDecisionPending; }
    bool IsWaitingToReplyToPlayer() const { return bPlayerRequest; }
    // 当前模型请求是否已经包含这个人的攻击，避免每次轻拳都重新开始思考
    bool IsRespondingToAttackFrom(const AActor* Attacker) const;
    // 玩家插话时停止旧交谈，记下说话者并开始倾听
    bool BeginListening(AActor* Speaker, FGuid MessageId);
    // 只取消这个控制器的旧请求和回复，不停止当前玩法活动
    void InterruptPlayerReply(APlayerController* Requester);
    bool IsPlayerReplyActive(APlayerController* Requester) const;
    FGuid GetReplyMessage(APlayerController* Requester) const;
    bool ShouldPauseForConversation() const;
    FDeliveryNpcPlayerReplyEnded OnPlayerReplyEnded;
    // 表现组件确认整轮播放完成后调用，模型结束不能代替声音结束
    void FinishReplyPlayback(FGuid ReplyId);
    bool CanReplacePendingReply(FGuid ExistingReplyId,FGuid NewReplyId) const;
    bool CanStartReplySpeech(FGuid ReplyId) const;
    bool CanStartReplyMessageAudio(FGuid ReplyId) const;
    bool CanRetainReplySpeech(FGuid ReplyId) const;
    bool HasPendingNpcMessage() const;
    void EndListening();
    UFUNCTION(BlueprintPure) AActor* GetListeningSpeaker() const { return ListeningSpeaker; }
    UFUNCTION(BlueprintPure) bool IsListening() const { return IsValid(ListeningSpeaker); }
    double GetListeningStartTime() const { return ListeningStartedAt; }
    bool ReceiveStreamDecision(FGuid RequestId, const FDeliveryNpcDecisionProposal& Proposal);
    void ReceiveSentence(FGuid RequestId, const FDeliveryNpcSentence& Sentence);
    // 返回 true 表示内容属于当前回复且表现组件已接收，不表示音频已经播放
    bool ReceiveSpeechBegin(FGuid RequestId, const FDeliveryNpcSpeechBegin& Begin);
    bool ReceiveAudioChunk(FGuid RequestId, const FDeliveryNpcAudioChunk& Chunk);
    bool ReceiveAudioEnd(FGuid RequestId, const FDeliveryNpcAudioEnd& End);
    void FinishReply(FGuid RequestId, bool bSucceeded, int32 SentenceCount=0);
    // 模型已决定台词和动作，后续声音仍按原请求编号接收
    bool FinishModelReply(FGuid RequestId, int32 SentenceCount, TOptional<FString> ApprovedText={});
    // 定向文本确实送达与声音播放分开记录，不走 HearSpeech
    bool ReceiveNpcMessage(const FDeliveryNpcMessageHandle& Handle);
    bool WasReplyTextDelivered(FGuid ReplyId,const ADeliveryNpcCharacter* Receiver) const;
    bool GetCompleteReply(FGuid ReplyId,FDeliveryNpcCompleteReply& OutReply) const;
    void InvalidateNpcMessageTurns(FName Reason,bool bLeavingWorld=false);
    // 表现组件替换待播回复时，只取消这一轮的后台合成
    void CancelReply(FGuid ReplyId);

    // 人设影响模型的语气和选择，可执行的动作仍由游戏决定
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|AI") FString Persona = TEXT("普通路人，注意安全并遵守当前动作能力");
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|AI", meta=(ClampMin="100", ClampMax="3000", Units="cm"))
    float MaxLookAtDistance = 1200.f;

    // 动作变化时通知本机界面和表现组件，客户端收到同步后也会通知
    // 玩家加入游戏时读取 NPC 当前动作，随后接收动作变化通知
    UPROPERTY(BlueprintAssignable, Category="Delivery|AI") FDeliveryNpcStateChanged OnPublicStateChanged;
    UPROPERTY(BlueprintAssignable, Category="Delivery|AI") FDeliveryNpcDecisionFinished OnDecisionFinished;

    // 由 Gateway 在服务器上准备请求，并接收模型返回的结果
    bool BuildDecisionRequest(FGuid WorldEpoch, FDeliveryNpcDecisionRequest& OutRequest);
    void ReceiveDecision(FGuid RequestId, const FDeliveryNpcDecisionProposal* Proposal, FName Error);
    FGuid GetNpcId() const { return NpcId; }
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
    friend class UDeliveryAiGatewaySubsystem;
    friend class FDeliveryNpcDecisionTraceTest;
    void BeginDecisionTrace(const FDeliveryNpcDecisionRequest& Request);
    bool IsDecisionTraceCurrent(FGuid WorldEpoch, FGuid RequestId) const;
    void TraceRequestEvent(FGuid WorldEpoch, FGuid RequestId, FName Stage, FName Error = NAME_None);
    void TraceDiagnostics(FGuid WorldEpoch, FGuid RequestId, const FDeliveryNpcDecisionTrace& Diagnostics);
    void TraceProposal(const FDeliveryNpcDecisionProposal& Proposal);
    void TraceValidation(FGuid WorldEpoch, FGuid RequestId, bool bAccepted, FName Error);
    void TraceReplyText(FGuid RequestId, const FString& Text);
    void TraceAction(FGuid ActionId, FName Stage);
    void PublishDecisionTrace();
    FDeliveryNpcDecisionTrace DecisionTrace;
    double TraceQueuedAt = -1;
    double TraceDispatchedAt = -1;
    UFUNCTION() void OnRep_PublicState();
    bool IsAuthority() const;
    bool CanReceiveReply(FGuid RequestId) const;
    struct FReply
    {
        FDeliveryNpcMessageHandle ParentNpcMessage;
        FString ApprovedText;
        FName Emotion;
        bool bHasApprovedText=false;
        bool bNpcMessageAttempted=false;
        bool bNpcMessageTracked=false;
        bool bPlainText=false;
        bool bOwnsAction=false;
        bool bPlayerReply=false;
        FGuid ReplyId;
        FGuid MessageId;
        FGuid ActionId;
        EDeliveryNpcAction Action=EDeliveryNpcAction::Wait;
        TWeakObjectPtr<AActor> Target;
        TWeakObjectPtr<AActor> Listener;
        TWeakObjectPtr<APlayerController> RequesterPC;
        TWeakObjectPtr<APlayerState> RequesterPlayerState;
        bool bContinueCurrent=false;
        bool bActionStarted=false;
        bool bHadTarget=false;
        bool bModelFinished=false;
        bool bTransferFinished=false;
        int32 SentenceCount=INDEX_NONE;
        int32 Depth=0;
        int32 PlanRevision=0;
    };
    void BindNpcMessageTurns();
    bool IsNpcMessageParentCurrent(const FDeliveryNpcMessageHandle& Parent) const;
    bool IsPlayerInputBeingRecorded() const;
    void AttachNpcMessageContext(FDeliveryNpcDecisionRequest& Request) const;
    bool RegisterReplyMessageContext(FReply& Reply,const FDeliveryNpcDecisionRequest& Request,bool bPlainText);
    bool CanReceiveNpcMessage(const FDeliveryNpcMessage& Message) const;
    void TryDeliverReplyMessage(FGuid RequestId);
    void ReportNpcReplyPresentation(FGuid ReplyId,EDeliveryNpcReplyPresentation State);
    void HandleNpcMessageCancellations(const TArray<FDeliveryNpcMessageCancellation>& Cancelled);
    TWeakObjectPtr<UDeliveryConversationTurnSubsystem> BoundNpcTurns;
    FDeliveryNpcMessageHandle NpcMessageActionParent;
    FGuid NpcMessageActionId;
    int32 NpcMessageActionPlan=0;
    bool bHandlingNpcMessageCancellation=false;
    TArray<FDeliveryNpcMessageCancellation> PendingNpcMessageCancellations;
    bool StartReplyPresentation(FReply Reply);
    bool IsActivitySnapshotCurrent(const FDeliveryNpcDecisionRequest& Request) const;
    bool AcceptContinueCurrent(const FDeliveryNpcDecisionProposal& Proposal,FName& Reason);
    void RemoveReply(FGuid ReplyId,FName Reason);
    void FinishDecisionWait(FGuid RequestId, bool bSucceeded, int32 SentenceCount);
    void PruneEvidence();
    bool CanLookAt(AActor* Target) const;
    bool HasGameplayRuleEvidence(AActor* Target,FGuid EvidenceId) const;
    bool IsGameplayRuleActionValid() const;
    bool TryAcceptProposal(const FDeliveryNpcDecisionProposal& Proposal, FName& Reason);
    bool CanChooseAction(EDeliveryNpcAction Action,AActor* Target,APlayerState* Requester) const;
    void ScheduleNextActionDecision();
    void RecordActionReply();
    void PublishState();
    void GetRequestedPlayerMessage(FGuid& OutMessage, AActor*& OutSpeaker) const;
    void FinishFailedPlayerReply(FGuid MessageId, AActor* Speaker, bool bWillRetry);
    double ServerTime() const;

    UPROPERTY(Replicated) bool bDecisionPending=false;
    UPROPERTY(ReplicatedUsing=OnRep_PublicState) FDeliveryNpcPublicState PublicState;
    FGuid NpcId;
    int32 ContextRevision = 0;
    int32 NextPlanRevision = 0;
    FDeliveryNpcDecisionRequest ActiveRequest;
    TWeakObjectPtr<APlayerState> RequestPlayerState;
    TArray<FGuid> LastDecisionEventIds;
    int32 ActionConversationDepth = 0;
    TArray<FDeliveryNpcEvidence> Evidence;
    TArray<FGuid> RecentEventIds;
    TMap<FGuid, TWeakObjectPtr<AActor>> KnownSubjects;
    TArray<FGuid> ActionEvidenceIds;
    FGuid ActionTargetId;
    bool bDecisionQueued = false;
    bool bEndingPlay = false;
    // 停止旧动作时的能力与状态通知不能重入并替换尚未清理完的安排
    bool bChangingPlan = false;
    bool bApplyingActionUpdate=false;
    bool bReplacingFight=false;
    double ActionDeadline = 0;
    double ActionEndsAt=0;
    double CombatDeadline=0;
    double NextActionDecisionAt=0;
    UPROPERTY(Replicated) TObjectPtr<AActor> ListeningSpeaker;
    UPROPERTY(Replicated) double ListeningStartedAt = -1;
    FGuid ListeningMessage;
    // 模型选好动作后会结束倾听，这个编号保留到整轮回复结束
    FGuid PlayerReplyMessage;
    TWeakObjectPtr<APlayerController> PlayerReplyController;
    double ListeningUntil = 0;
    // 模型可以继续决定下一步，已确认回复的声音仍属于各自原来的请求
    TMap<FGuid,FReply> Replies;
    int32 LatestSpeechPlanRevision=0;
    bool bPlayerRequest = false;
};
