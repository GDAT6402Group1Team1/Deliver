#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AI/Decision/DeliveryNpcTypes.h"
#include "DeliveryNpcAgentComponent.generated.h"
class APlayerController;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryNpcStateChanged, const FDeliveryNpcPublicState&, State);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryNpcDecisionFinished, FName, Reason);

// 保存 NPC 看到、听到的事件，向模型请求回复，并记录当前动作
// Gateway 负责发送请求，行为树负责执行通过检查的动作
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryNpcAgentComponent : public UActorComponent
{
    GENERATED_BODY()
    friend class FDeliveryNpcConversationContextTest;
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
public:
    UDeliveryNpcAgentComponent();

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

    // 行为树开始动作前再次检查目标，返回 true 后才能执行
    // 动作结束时使用同一 ActionId 报告结果，防止旧回调结束新动作
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI")
    bool StartAction(FDeliveryNpcPublicState& OutAction);
    // 记录当前动作的结果；动作编号不符或已结束时返回 false
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI")
    bool TryFinishAction(FGuid ActionId, EDeliveryNpcActionStatus Result, FName Reason);
    UFUNCTION(BlueprintPure, Category="Delivery|AI") FDeliveryNpcPublicState GetPublicState() const { return PublicState; }
    UFUNCTION(BlueprintPure, Category="Delivery|AI") bool HasActiveAction() const;
    UFUNCTION(BlueprintPure, Category="Delivery|AI") bool IsDecisionPending() const { return bDecisionPending; }
    bool IsWaitingToReplyToPlayer() const { return bPlayerRequest; }
    // 当前模型请求是否已经包含这个人的攻击，避免每次轻拳都重新开始思考
    bool IsRespondingToAttackFrom(const AActor* Attacker) const;
    // 玩家插话时停止旧交谈，记下说话者并开始倾听
    void BeginListening(AActor* Speaker, FGuid MessageId);
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
    UFUNCTION() void OnRep_PublicState();
    bool IsAuthority() const;
    bool CanReceiveReply(FGuid RequestId) const;
    void PruneEvidence();
    bool CanLookAt(AActor* Target) const;
    bool TryAcceptProposal(const FDeliveryNpcDecisionProposal& Proposal, FName& Reason);
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
    TArray<FDeliveryNpcEvidence> Evidence;
    TArray<FGuid> RecentEventIds;
    TMap<FGuid, TWeakObjectPtr<AActor>> KnownSubjects;
    TArray<FGuid> ActionEvidenceIds;
    FGuid ActionTargetId;
    bool bDecisionQueued = false;
    bool bEndingPlay = false;
    double ActionDeadline = 0;
    UPROPERTY(Replicated) TObjectPtr<AActor> ListeningSpeaker;
    UPROPERTY(Replicated) double ListeningStartedAt = -1;
    FGuid ListeningMessage;
    // 模型选好动作后会结束倾听，这个编号保留到整轮回复结束
    FGuid PlayerReplyMessage;
    TWeakObjectPtr<APlayerController> PlayerReplyController;
    double ListeningUntil = 0;
    FGuid ReplyRequest;
    FGuid ReplyAction;
    bool bPlayerRequest = false;
};
