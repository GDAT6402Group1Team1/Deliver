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

/// @brief NPC 当前动作变化时通知；服务器和收到同步的客户端都会发
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryNpcStateChanged, const FDeliveryNpcPublicState&, State);

/// @brief 一次模型请求结束时通知，Reason 是结束原因
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryNpcDecisionFinished, FName, Reason);

/// @brief NPC 回复某位玩家的整轮话结束时，通知交谈组件
DECLARE_MULTICAST_DELEGATE_ThreeParams(FDeliveryNpcPlayerReplyEnded,APlayerController* /* 玩家 */,FGuid /* 玩家那句话的编号 */,FName /* 结束原因 */);

/**
 * @brief NPC 的大脑：记住看到听到的事，问模型该说什么做什么，再检查模型的回答能不能照做
 *
 * 记忆：角色调用 ObserveEvent 记下见闻和听到的话
 * 决策：RequestDecision 安排请求，网关调 BuildDecisionRequest 列出事件和候选动作，发给后端
 * 采纳：网关收到回答调 ReceiveStreamDecision，TryAcceptProposal 逐项检查，通过后写入 PublicState
 * 执行：行为树 StartAction 领取动作，做完 TryFinishAction 写回结果
 * 回复：台词和声音由网关送进 ReceiveSentence 等函数，转给表现组件播放
 * 接话：回复要说给另一位 NPC 时，TryDeliverReplyMessage 经轮次子系统送过去
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryNpcAgentComponent : public UActorComponent
{
    GENERATED_BODY()
    friend class FDeliveryNpcConversationContextTest;
    friend class FDeliveryAiContextTest;
    friend class FDeliveryCompanionRulesTest;
    friend class FDeliveryCompanionErrandTest;
    friend class FDeliveryCompanionAssistTest;
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

    // ── 诊断 ──
    // 保存最近一次请求的全过程给 AI 检查器看，只用来显示，不能用它推进玩法

    /// @brief 最近一次请求的全过程
    const FDeliveryNpcDecisionTrace& GetDecisionTrace() const { return DecisionTrace; }

    /// 诊断记录更新时通知
    FDeliveryNpcDecisionTraceChanged OnDecisionTraceChanged;

    // ── 记忆 ──
    // 角色把看到听到的事交到这里，需要回应时再调用决策分区的 RequestDecision

    /// @brief 服务器记下一件事，最近 128 件里同一编号只记一次
    /// @return 记下时返回 true，这时还没有问模型
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI")
    bool ObserveEvent(const FDeliveryNpcObservation& Observation);

    // ── 决策 ──
    // 安排一次模型请求，网关空闲时调用 BuildDecisionRequest 取走；回答回来后交给采纳分区的 TryAcceptProposal

    /// @brief 安排问一次模型
    /// @return 已安排时返回 true；正在等上一次回答时，新信息留到那次结束后再问
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI") bool RequestDecision();

    /// @brief 不再等当前这次回答，后端已经开始的计算不一定马上停
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI") void CancelDecision();

    /// @brief 同时取消请求和当前动作，已经打中的拳不会撤销
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI") void CancelPlan(FName Reason);

    /// @brief 二狗换主人时清掉旧主人的对话和经历，旧请求一并作废
    void ResetContext(FName Reason);

    /// @brief 不经模型、按游戏规则直接安排一段动作，比如二狗护主
    /// 角色先确认是主人的事、规则版本号没变，这里再确认真有这件事、目标还在；模型不能走这个入口
    /// @return 已安排时返回 true，打没打中仍由战斗组件判定
    bool RequestGameplayRuleAction(EDeliveryNpcAction Action,AActor* Target,int32 RuleRevision /* 规则的版本号，主人取消后就对不上 */,FGuid EvidenceId /* 触发这次规则的事件，比如主人挨打 */,float Duration /* 最多做几秒 */);

    /// @brief 居民被玩家或二狗打中后，不等模型立即追打动手的人；目标必须有一条真实的受击记录
    /// 居民之间互殴不走这里，仍由各自的模型决定；开打后继续、换人、逃跑或停手仍由模型选
    /// @return 已安排交战时返回 true；正在逃跑、执行规则动作或已经在打这个人时返回 false，原安排不变
    bool RequestRetaliation(AActor* Attacker);

    // ── 执行 ──
    // 行为树 StartAction 领取已采纳的动作并去做，做完用同一个 ActionId 调 TryFinishAction 交回结果

    /// @brief 行为树领取当前动作，领取前再检查一次目标还在不在
    /// @return 返回 true 才能开始执行
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI")
    bool StartAction(FDeliveryNpcPublicState& OutAction);

    /// @brief 交回动作结果，写成一件新事记进这位 NPC 的记忆
    /// @return 动作编号对不上或已经结束时返回 false，防止旧回调结束新动作
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI")
    bool TryFinishAction(FGuid ActionId /* 领取时拿到的动作编号 */, EDeliveryNpcActionStatus Result, FName Reason);

    /// @brief 当前已采纳的动作、目标、台词情绪，服务器同步给所有客户端
    UFUNCTION(BlueprintPure, Category="Delivery|AI") FDeliveryNpcPublicState GetPublicState() const { return PublicState; }

    /// @brief 当前回复是第几层接话，按请求发出时算
    int32 GetActionConversationDepth() const { return ActionConversationDepth; }

    /// @brief 是否有已采纳或正在执行的动作
    UFUNCTION(BlueprintPure, Category="Delivery|AI") bool HasActiveAction() const;

    /// @brief 二狗有没有跟随、跑腿、家居这类持续安排
    bool HasCompanionActivity() const;

    /// @brief 二狗边说话边继续原来的跟随或跑腿；当前动作只是等待或看着人说话时返回 true
    bool CanContinueCompanionMovement() const;

    /// @brief 交战和逃跑途中能不能再问模型下一步；规则安排的护主不能
    bool CanUpdateRunningAction() const;

    /// @brief 当前动作还剩多少秒
    double GetActionTimeRemaining() const;

    /// @brief 是不是换目标接着打，换目标时不加整场冷却
    bool IsReplacingFight() const { return bReplacingFight; }

    /// @brief 是否正在等模型回答
    UFUNCTION(BlueprintPure, Category="Delivery|AI") bool IsDecisionPending() const { return bDecisionPending; }

    /// @brief 正在问的这次是不是在回答玩家
    bool IsWaitingToReplyToPlayer() const { return bPlayerRequest; }

    /// @brief 正在问的这次是否已经包含这个人的攻击，避免每挨一拳都重新问一次
    bool IsRespondingToAttackFrom(const AActor* Attacker) const;

    // ── 倾听 ──
    // 交谈组件把玩家的话交过来时开始倾听，NPC 转向说话的人，随后调用 RequestDecision 问模型怎么回

    /// @brief 开始听这个人说话，玩家插话时停掉旧的交谈
    bool BeginListening(AActor* Speaker, FGuid MessageId);

    /// @brief 只打断回给这个玩家的旧回复，不停 NPC 正在做的事
    void InterruptPlayerReply(APlayerController* Requester);

    /// @brief 是否正在回复这位玩家
    bool IsPlayerReplyActive(APlayerController* Requester) const;

    /// @brief 正在回复这位玩家的哪一句话
    FGuid GetReplyMessage(APlayerController* Requester) const;

    /// @brief 是否要停下手头的事听玩家说话
    bool ShouldPauseForConversation() const;

    /// 回复某位玩家的整轮话结束时通知交谈组件
    FDeliveryNpcPlayerReplyEnded OnPlayerReplyEnded;

    // ── 回复 ──
    // 网关送来台词和声音 → 这里核对属于当前回复 → 转给表现组件播放 → 播完 FinishReplyPlayback

    /// @brief 表现组件确认这一轮真的播完时调用；模型返回完毕不算播完
    void FinishReplyPlayback(FGuid ReplyId /* 播完的回复 */,FGuid CompletedSpeechEventId=FGuid() /* 最后一句台词的事件编号 */);

    // 表现组件播放前逐项询问：这一轮还能不能接上、替换、开口、保留
    bool CanLinkReplyPlayback(FGuid ReplyId) const;

    /// @brief 新回复能不能顶掉还没开始播的旧回复
    bool CanReplacePendingReply(FGuid ExistingReplyId /* 正在等着播的回复 */,FGuid NewReplyId /* 新来的回复 */) const;

    /// @brief 这一轮回复能不能开始播
    bool CanStartReplySpeech(FGuid ReplyId) const;

    /// @brief 播放时说给谁听，初遇回应固定说给主人
    AActor* GetReplyListenerForPlayback(FGuid ReplyId,AActor* Fallback) const;

    /// @brief 说给另一位 NPC 的回复能不能开始播
    bool CanStartReplyMessageAudio(FGuid ReplyId) const;

    /// @brief 接话链作废后，已经在播的这一轮能不能说完
    bool CanRetainReplySpeech(FGuid ReplyId) const;

    /// @brief 有没有别的 NPC 对我说了话、还没回应
    bool HasPendingNpcMessage() const;

    /// @brief 结束倾听
    void EndListening();

    /// @brief 正在听谁说话
    UFUNCTION(BlueprintPure) AActor* GetListeningSpeaker() const { return ListeningSpeaker; }

    /// @brief 是否正在倾听
    UFUNCTION(BlueprintPure) bool IsListening() const { return IsValid(ListeningSpeaker); }

    /// @brief 开始倾听的服务器时间，客户端据此对齐动画
    double GetListeningStartTime() const { return ListeningStartedAt; }

    // 网关按到达顺序调用：建议 → 句子 → 声音开始、分段、结束 → 整轮结束

    /// @brief 收到模型的建议，交给 TryAcceptProposal 检查
    bool ReceiveStreamDecision(FGuid RequestId, const FDeliveryNpcDecisionProposal& Proposal);

    /// @brief 收到一整句台词，转给表现组件
    void ReceiveSentence(FGuid RequestId, const FDeliveryNpcSentence& Sentence);

    /// @return 内容属于当前回复、表现组件已收下时返回 true，不代表已经播出来
    bool ReceiveSpeechBegin(FGuid RequestId, const FDeliveryNpcSpeechBegin& Begin);

    /// @brief 收到一段声音，转给表现组件
    bool ReceiveAudioChunk(FGuid RequestId, const FDeliveryNpcAudioChunk& Chunk);

    /// @brief 一句声音结束，转给表现组件
    bool ReceiveAudioEnd(FGuid RequestId, const FDeliveryNpcAudioEnd& End);

    /// @brief 整轮数据收完：成功时等表现组件播完，失败时清理
    void FinishReply(FGuid RequestId, bool bSucceeded /* 数据有没有完整收到 */, int32 SentenceCount=0 /* 共收到几句 */);

    /// @brief 模型说完了，台词和动作定下来；之后的声音仍按原请求编号收
    bool FinishModelReply(FGuid RequestId, int32 SentenceCount, TOptional<FString> ApprovedText={});

    /// @brief 收到另一位 NPC 专门说给自己的话，不经过 HearSpeech
    bool ReceiveNpcMessage(const FDeliveryNpcMessageHandle& Handle);

    /// @brief 这一轮的文字有没有送到某位 NPC
    bool WasReplyTextDelivered(FGuid ReplyId,const ADeliveryNpcCharacter* Receiver) const;

    /// @brief 取整轮回复的完整文字，纯文字回复用
    bool GetCompleteReply(FGuid ReplyId,FDeliveryNpcCompleteReply& OutReply) const;

    /// @brief 作废这位 NPC 参与的所有接话
    void InvalidateNpcMessageTurns(FName Reason,bool bLeavingWorld=false /* true 表示 NPC 正在离开这个世界 */);

    /// @brief 表现组件换掉待播的回复时，只取消这一轮的语音合成
    void CancelReply(FGuid ReplyId);

    // ── 配置 ──

    /// 人设，影响模型的语气和选择，能做什么动作仍由游戏决定
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|AI") FString Persona = TEXT("普通路人，注意安全并遵守当前动作能力");

    /// 最远能看向多远的人
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|AI", meta=(ClampMin="100", ClampMax="3000", Units="cm"))
    float MaxLookAtDistance = 1200.f;

    // ── 执行 ──
    // 动作变化时通知界面和表现组件；新加入的玩家先读 GetPublicState，再等通知

    /// 当前动作变化
    UPROPERTY(BlueprintAssignable, Category="Delivery|AI") FDeliveryNpcStateChanged OnPublicStateChanged;

    /// 一次请求结束，角色据此安排重试
    UPROPERTY(BlueprintAssignable, Category="Delivery|AI") FDeliveryNpcDecisionFinished OnDecisionFinished;

    // ── 决策 ──
    // 网关在服务器上调用：BuildDecisionRequest 取走请求，ReceiveDecision 交回一次性结果

    /// @brief 整理当前记忆和候选动作，生成发给模型的请求
    bool BuildDecisionRequest(FGuid WorldEpoch, FDeliveryNpcDecisionRequest& OutRequest);

    /// @brief 非流式回答或请求失败时的入口：检查建议、记录结果、按需重试
    void ReceiveDecision(FGuid RequestId, const FDeliveryNpcDecisionProposal* Proposal /* 模型的回答，失败时为空 */, FName Error /* 失败原因，成功时为 None */);

    /// @brief 这位 NPC 的编号，服务器开局时生成
    FGuid GetNpcId() const { return NpcId; }

    /// @brief 按对象查它在模型眼里的编号
    FGuid FindKnownSubjectId(const AActor* Actor) const;

    /// @brief 这件事是否还记得
    bool HasObservationEvent(FGuid EventId) const;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
    friend class UDeliveryAiGatewaySubsystem;
    friend class FDeliveryNpcDecisionTraceTest;

    // ── 诊断 ──
    // 请求的每一步都记一笔，PublishDecisionTrace 推给检查器

    /// 请求入队时开一份记录：事件、候选、记忆和当前活动
    void BeginDecisionTrace(const FDeliveryNpcDecisionRequest& Request);

    /// 这份记录属于这次请求和当前世界
    bool IsDecisionTraceCurrent(FGuid WorldEpoch /* 世界编号 */, FGuid RequestId /* 请求编号 */) const;

    /// 记请求阶段：发出、完成、失败、取消，并算耗时
    void TraceRequestEvent(FGuid WorldEpoch /* 世界编号 */, FGuid RequestId /* 请求编号 */, FName Stage /* 发出、完成、失败或取消 */, FName Error = NAME_None);

    /// 记后端返回的诊断：实际模型、路由、各段耗时和 token 数
    void TraceDiagnostics(FGuid WorldEpoch /* 世界编号 */, FGuid RequestId /* 请求编号 */, const FDeliveryNpcDecisionTrace& Diagnostics);

    /// 记模型建议的动作和目标
    void TraceProposal(const FDeliveryNpcDecisionProposal& Proposal);

    /// 记检查结果和拒绝原因
    void TraceValidation(FGuid WorldEpoch /* 世界编号 */, FGuid RequestId /* 请求编号 */, bool bAccepted /* 检查通过了没有 */, FName Error);

    /// 记第一句台词
    void TraceReplyText(FGuid RequestId, const FString& Text);

    /// 记动作开始和结束
    void TraceAction(FGuid ActionId, FName Stage);

    /// 修订号加一并通知订阅者
    void PublishDecisionTrace();

    /// 最近一次请求的记录
    FDeliveryNpcDecisionTrace DecisionTrace;

    /// 请求入队时间
    double TraceQueuedAt = -1;

    /// 请求发出时间
    double TraceDispatchedAt = -1;

    /// 客户端收到新的动作状态，通知界面和表现组件
    UFUNCTION() void OnRep_PublicState();

    /// 是否在服务器上
    bool IsAuthority() const;

    /// 这个请求编号的回复是否还在等数据
    bool CanReceiveReply(FGuid RequestId) const;

    // ── 回复 ──
    // 一轮已采纳的回复，按请求编号保存，声音播完前不删除

    /// 一轮已采纳的回复：台词、动作、说给谁、播到哪了
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

        // 二狗初遇时的回应，关联玩家救下它的那次真实事件；玩家插话也不丢
        FGuid JoinedEventId;
        TWeakObjectPtr<APlayerState> JoinedOwner;
        double JoinedOutOfReachSince=-1;
        bool bJoinedPlaybackMissed=false;
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
        bool RequiresCompanionCommandReceipt() const { return bOwnsAction && DeliveryNpc::IsCompanionCommand(Action); }
    };

    // ── 接话 ──
    // 回复被采纳后登记，播出时 TryDeliverReplyMessage 把这句话递给对方 NPC；实现在 DeliveryNpcMessages.cpp

    /// 服务器订阅轮次子系统的接话作废通知
    void BindNpcMessageTurns();

    /// 这条回复所接的上一句话还算数
    bool IsNpcMessageParentCurrent(const FDeliveryNpcMessageHandle& Parent) const;

    /// 玩家是否正在对这位 NPC 录音或等识别文字
    bool IsPlayerInputBeingRecorded() const;

    /// 请求里附上正在回应哪位 NPC 的哪句话；回答玩家时不附
    void AttachNpcMessageContext(FDeliveryNpcDecisionRequest& Request) const;

    /// 采纳后向轮次子系统登记这一轮，之后才能把台词递给另一位 NPC
    bool RegisterReplyMessageContext(FReply& Reply,const FDeliveryNpcDecisionRequest& Request,bool bPlainText /* true 表示这轮只有文字，没有合成声音 */);

    /// 能不能收下另一位 NPC 递来的话
    bool CanReceiveNpcMessage(const FDeliveryNpcMessage& Message) const;

    /// 把这一轮的完整台词递给说话对象，对方据此接话
    void TryDeliverReplyMessage(FGuid RequestId);

    /// 把这条回复播到哪了告诉轮次子系统；回复被打断时，接在它后面的接话会被取消
    void ReportNpcReplyPresentation(FGuid ReplyId,EDeliveryNpcReplyPresentation State);

    /// 接话作废：取消对应的回复和由它引出的动作
    void HandleNpcMessageCancellations(const TArray<FDeliveryNpcMessageCancellation>& Cancelled);

    /// 已订阅的轮次子系统
    TWeakObjectPtr<UDeliveryConversationTurnSubsystem> BoundNpcTurns;

    /// 当前动作是由哪句接话引出的
    FDeliveryNpcMessageHandle NpcMessageActionParent;

    /// 由接话引出的动作编号
    FGuid NpcMessageActionId;

    /// 那个动作所属的计划修订号
    int32 NpcMessageActionPlan=0;

    /// 正在执行一批取消通知；这期间又来的通知先放进下面的队列，等这批做完再执行
    bool bHandlingNpcMessageCancellation=false;

    /// 等着执行的取消通知
    TArray<FDeliveryNpcMessageCancellation> PendingNpcMessageCancellations;

    // ── 回复 ──
    // StartReplyPresentation 把采纳的回复交给表现组件；二狗初遇时的回应没说完时先留着

    /// 把回复交给表现组件开始播，初遇回应过期时不播
    bool StartReplyPresentation(FReply Reply);

    /// 回复失败：清理队列，播放失败反馈
    void FailReply(FGuid RequestId);

    /// 请求发出时记下的当前活动，现在是否还一样
    bool IsActivitySnapshotCurrent(const FDeliveryNpcDecisionRequest& Request) const;

    /// 二狗的委托在模型思考期间变了，需要重新问时返回原因
    FName GetRequestedCompanionErrandRefreshReason(EDeliveryNpcAction ProposedAction) const;

    /// 检查 ContinueCurrent：当前活动没变才保留原安排
    bool AcceptContinueCurrent(const FDeliveryNpcDecisionProposal& Proposal,EDeliveryCompanionSpeechPreference Preference,FName& Reason);

    /// 删除一轮回复，取消语音合成，让 NPC 不再被这一轮对话占着
    void RemoveReply(FGuid ReplyId,FName Reason);

    /// 初遇回应还属于当前主人
    bool IsJoinedResponseOwnerCurrent() const;

    /// 初遇回应说给主人当前控制的角色
    APawn* GetJoinedResponseListener() const;

    /// 主人在不在听得到的范围内
    bool CanHearJoinedResponse(const APawn* Listener) const;

    /// 模型选的说话对象是不是主人
    bool IsJoinedReplyTarget(const FDeliveryNpcDecisionRequest& Request,FGuid SubjectId /* 模型选的说话对象编号 */,const APawn& Listener) const;

    /// 把初遇事件关联到这一轮回复
    void BindJoinedResponse(FReply& Reply,const FDeliveryNpcDecisionRequest& Request) const;

    /// 这次请求要不要回应初遇事件
    FGuid GetJoinedResponseEvent(const FDeliveryNpcDecisionRequest& Request) const;

    /// 主人插话时推迟初遇回应，6 秒后再试
    void DeferJoinedResponse();

    // 没说完的初遇回应，最多主动再试三次，之后等玩家下次来说话
    FGuid PendingJoinedEvent;

    /// 初遇回应的主人
    TWeakObjectPtr<APlayerState> PendingJoinedOwner;

    /// 初遇时救它的那个角色
    TWeakObjectPtr<APawn> PendingJoinedSubject;

    /// 初遇回应什么时候过期
    double JoinedResponseExpiresAt=0;

    /// 下次重试初遇回应的时间
    double NextJoinedResponseRetryAt=0;

    /// 已经试了几次
    int32 JoinedResponseAttempts=0;

    // ── 采纳 ──
    // TryAcceptProposal 检查通过后写 PublicState 并调用 PublishState，行为树读到新状态后执行动作

    /// 一次请求结束等待：失败时按需重试，成功时安排下一次
    void FinishDecisionWait(FGuid RequestId, bool bSucceeded /* 请求成功了没有 */, int32 SentenceCount /* 共几句台词 */);

    /// 删过期事件和失效对象，正被动作或回复用着的保留
    void PruneEvidence();

    /// 能不能看向这个对象：看得见、不超过 MaxLookAtDistance
    bool CanLookAt(AActor* Target) const;

    /// 规则动作引用的受击事件还有效
    bool HasGameplayRuleEvidence(AActor* Target,FGuid EvidenceId) const;

    /// 当前规则动作的目标和事件还有效
    bool IsGameplayRuleActionValid() const;

    /// @brief 逐项检查模型的建议：请求没过期、动作在候选里、引用的事真有、目标还在、时长合理
    /// @return 全部通过返回 true；不通过时 Reason 写拒绝原因
    bool TryAcceptProposal(const FDeliveryNpcDecisionProposal& Proposal, FName& Reason);

    /// @brief 这个动作此刻能不能做，列候选和检查建议都用它
    bool CanChooseAction(EDeliveryNpcAction Action,AActor* Target,APlayerState* Requester) const;

    /// 交战中安排下一次问模型的时间
    void ScheduleNextActionDecision();

    /// 动作成功但没有台词时，在 Evidence 里记一条 Kind 为 ActionReply 的事件，表示已经用动作回应了 ReplyToEventId 那件事
    /// 有台词的回应不在这里记，播放完成时另外记录
    void RecordActionReply();

    /// 状态变了强制同步，并在服务器本机发通知
    void PublishState();

    /// 当前请求在回答哪位玩家的哪句话
    void GetRequestedPlayerMessage(FGuid& OutMessage, AActor*& OutSpeaker) const;

    /// 回答玩家失败：会重试就先留着，不再重试就通知交谈面板
    void FinishFailedPlayerReply(FGuid MessageId /* 玩家那句话的编号 */, AActor* Speaker, bool bWillRetry /* true 表示还会再问一次模型 */);

    /// 服务器时间，客户端按它对齐动画和声音
    double ServerTime() const;

    // ── 执行 ──
    // 当前动作和请求的状态

    /// 是否正在等模型回答，同步给客户端显示思考表情
    UPROPERTY(Replicated) bool bDecisionPending=false;

    /// 当前采纳的动作，同步给所有客户端
    UPROPERTY(ReplicatedUsing=OnRep_PublicState) FDeliveryNpcPublicState PublicState;

    /// 这位 NPC 的编号
    FGuid NpcId;

    /// 记忆的版本，换主人等大变动时加一，旧请求作废
    int32 ContextRevision = 0;

    /// 下一个计划修订号
    int32 NextPlanRevision = 0;

    /// 正在等回答的请求
    FDeliveryNpcDecisionRequest ActiveRequest;

    /// 这次请求是替哪位玩家问的
    TWeakObjectPtr<APlayerState> RequestPlayerState;

    /// 上次请求包含的事件
    TArray<FGuid> LastDecisionEventIds;

    /// 当前回复的接话层数
    int32 ActionConversationDepth = 0;

    // ── 记忆 ──
    // 记住的事，发请求时由 BuildDecisionRequest 整理给模型

    /// 记住的每件事
    TArray<FDeliveryNpcEvidence> Evidence;

    /// 最近 128 个事件编号；同一件事被报告两次时，第二次不再记
    TArray<FGuid> RecentEventIds;

    // 已感知对象的编号表，模型只能用这些编号指代对象
    TMap<FGuid, TWeakObjectPtr<AActor>> KnownSubjects;

    /// 当前动作引用的事件
    TArray<FGuid> ActionEvidenceIds;

    /// 当前动作的目标编号
    FGuid ActionTargetId;

    /// 等上一次结束后还要再问一次
    bool bDecisionQueued = false;

    /// 正在结束，不再发新请求
    bool bEndingPlay = false;

    // 正在换安排时为 true，这期间的回调不能再换一次
    bool bChangingPlan = false;

    /// 正在更新交战中的下一步
    bool bApplyingActionUpdate=false;

    /// 正在换交战目标
    bool bReplacingFight=false;

    // 已经还手、模型还没说话，开打后尽快问一次
    bool bRetaliationNeedsReply=false;

    /// 动作最晚开始的时间
    double ActionDeadline = 0;

    /// 动作预计结束时间
    double ActionEndsAt=0;

    /// 交战最晚结束时间
    double CombatDeadline=0;

    /// 交战中下次问模型的时间
    double NextActionDecisionAt=0;

    /// 交战中出了新状况，最早什么时候问
    double CombatEventDecisionAt=-1;

    /// 交战中上次问模型的时间
    double LastCombatDecisionAt=-1;

    // ── 倾听 ──

    /// 正在听谁说话
    UPROPERTY(Replicated) TObjectPtr<AActor> ListeningSpeaker;

    /// 开始倾听的时间
    UPROPERTY(Replicated) double ListeningStartedAt = -1;

    /// 正在听的那句话的编号
    FGuid ListeningMessage;

    // 模型选好动作后倾听就结束了，这个编号留到整轮回复结束
    FGuid PlayerReplyMessage;

    /// 正在回复的玩家
    TWeakObjectPtr<APlayerController> PlayerReplyController;

    /// 倾听最晚到什么时候
    double ListeningUntil = 0;

    // ── 回复 ──
    // 每轮回复各自保存，模型继续决定下一步时，之前那轮的声音仍按原请求播放

    /// 按请求编号存的回复
    TMap<FGuid,FReply> Replies;

    /// 最新一轮台词所属的计划修订号
    int32 LatestSpeechPlanRevision=0;

    /// 正在问的这次是不是在回答玩家
    bool bPlayerRequest = false;
};
