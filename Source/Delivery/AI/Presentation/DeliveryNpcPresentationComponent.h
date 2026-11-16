#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AI/Decision/DeliveryNpcTypes.h"
#include "AI/Presentation/Speech/DeliveryNpcVoiceSet.h"
#include "DeliveryNpcPresentationComponent.generated.h"
class UAnimSequence;
class UDeliveryDialogueVoiceComponent;
class UWidgetComponent;
class UAudioComponent;
class USoundWaveProcedural;
class UDeliveryNpcExpressionSet;
class FDeliveryNpcVoiceDecoder;
class APlayerController;
class APlayerState;
struct FDeliveryNpcVoiceEncodingJob;

/// 正在说的一句话：文字、情绪、开始时间、声音格式，服务器同步给所有客户端
USTRUCT(BlueprintType)
struct FDeliveryNpcSpeechState
{
    GENERATED_BODY()

    /// 这一句的编号
    UPROPERTY(BlueprintReadOnly) FGuid TurnId;

    /// 这句属于哪一轮回复，值是 PublicState.ReplyId；沿用旧的字段名
    UPROPERTY(BlueprintReadOnly) FGuid ActionId;

    /// 在回答哪一句话
    UPROPERTY(BlueprintReadOnly) FGuid ReplyToEventId;

    /// 开麦对话时，这句话直接传给主人；其他玩家仍按距离听见
    UPROPERTY(BlueprintReadOnly) TObjectPtr<APlayerState> RadioListener;

    /// 台词
    UPROPERTY(BlueprintReadOnly) FString Text;

    /// 模型选的情绪，决定表情
    UPROPERTY(BlueprintReadOnly) FName Emotion;

    /// 开始播的服务器时间
    UPROPERTY(BlueprintReadOnly) double StartTime=0;

    /// 预计播多久
    UPROPERTY(BlueprintReadOnly) float Duration=0;

    /// 正在说
    UPROPERTY(BlueprintReadOnly) bool bActive=false;

    /// 有合成好的真人声音；为 false 时用电子音按文字播
    UPROPERTY(BlueprintReadOnly) bool bRecordedVoice=false;

    /// 这轮回复里的第几句
    UPROPERTY(BlueprintReadOnly) int32 Sequence=0;

    /// 声音采样率
    UPROPERTY(BlueprintReadOnly) int32 SampleRate=16000;

    /// 声音格式版本，边收边播的格式不知道总长
    UPROPERTY(BlueprintReadOnly) int32 AudioFormatVersion=0;

    /// 解压后的总字节数
    UPROPERTY(BlueprintReadOnly) int32 PcmBytes=0;

    /// 压缩后的总字节数
    UPROPERTY(BlueprintReadOnly) int32 EncodedBytes=0;

    /// 收到句末通知才为 true；声音暂时断了不代表说完
    UPROPERTY(BlueprintReadOnly) bool bAudioComplete=false;
};

/// 客户端收到的一句语音，排队播放，前一句播完再播下一句
struct FDeliveryNpcLocalVoice
{
    /// 这句的信息
    FDeliveryNpcSpeechState State;

    /// 已解压的声音
    TArray<uint8> Pcm;

    /// 收到的压缩声音
    TArray<uint8> Encoded;

    /// 边收边解压的解码器
    TSharedPtr<FDeliveryNpcVoiceDecoder> Decoder;

    /// 压缩数据已解到哪
    int32 DecodeOffset=0;

    /// 解压后的总长，未知时为 0
    int32 TotalBytes=0;

    /// 已经交给声卡的字节数
    int32 QueuedBytes=0;

    /// 最近一次收到数据的时间
    double LastDataAt=0;

    /// 这句排到队首的时间
    double FrontSince=0;

    /// 声音读空的时间
    double DrainedAt=0;

    /// 用文字计时开始的时间
    double TextStartedAt=0;

    /// 被新回复接上后，还能补收这句的声音，过了这个时间就不收
    double ReceiveUntil=0;

    /// 字幕显示到几成
    float CaptionProgress=0;

    /// 声音断了，正在等数据
    bool bRebuffering=false;
};

/// 服务器留着的一句声音，给后来走近的玩家补发
struct FDeliveryNpcServerVoice
{
    /// 开麦对话时这句传给哪位主人
    TWeakObjectPtr<APlayerState> RadioListener;

    /// 解压后的声音
    TArray<uint8> Pcm;

    /// 压缩的声音，补发时按段发出
    TArray<uint8> Encoded;

    /// 什么时候删，说完后留 5 秒
    double ExpiresAt=0;

    /// 这句声音已经收全
    bool bComplete=true;
};

/// 后台线程压缩好的一句声音；什么时候播仍由游戏线程决定
struct FDeliveryNpcEncodedSentence
{
    /// 压缩好的声音
    TArray<uint8> Audio;

    /// 压缩失败的原因
    FString Error;

    /// 压缩用了多少毫秒
    double EncodeMs=0;
};

/// 服务器上正在边收边压缩的一句话；检查和同步都在游戏线程
struct FDeliveryNpcGeneratingSentence
{
    /// 句子文字和格式
    FDeliveryNpcSentence Sentence;

    /// 这一句的编号
    FGuid TurnId;

    /// 后台压缩任务
    TSharedPtr<FDeliveryNpcVoiceEncodingJob, ESPMode::ThreadSafe> Encoding;

    /// 已经压缩好的部分
    TArray<uint8> Encoded;

    /// 下一段的序号
    int32 NextChunk=0;

    /// 已收到的原始声音字节数
    int32 ReceivedPcmBytes=0;

    /// 网关那边这句已经发完
    bool bInputEnded=false;

    /// 压缩已经完成
    bool bEncodingEnded=false;

    /// 这句失败了，改用文字
    bool bFailed=false;
};

/// 当前这句说完前收到的下一轮回复，只留一轮，再来新的就替换掉
struct FDeliveryNpcPendingReply
{
    /// 纯文字回复的完整内容
    FDeliveryNpcCompleteReply CompleteReply;

    /// 回复编号
    FGuid ReplyId;

    /// 在回答哪句话
    FGuid MessageId;

    /// 随回复一起采纳的动作
    FGuid ActionId;

    /// 动作目标
    TWeakObjectPtr<AActor> Target;

    /// 说给谁听
    TWeakObjectPtr<AActor> Listener;

    /// 接话层数
    int32 Depth=0;

    /// 共几句，还不知道时为 INDEX_NONE
    int32 SentenceCount=INDEX_NONE;

    /// 数据已经收全
    bool bTransferFinished=false;

    /// 一次性收到的整句
    TMap<int32,FDeliveryNpcSentence> Whole;

    /// 各句的开头
    TMap<int32,FDeliveryNpcSpeechBegin> Begins;

    /// 各句的声音分段
    TMap<int32,TArray<FDeliveryNpcAudioChunk>> Chunks;

    /// 各句的结尾
    TMap<int32,FDeliveryNpcAudioEnd> Ends;
};

/**
 * @brief NPC 的嘴和脸：把台词、声音和播放时间同步给所有玩家，各自播出声音、头顶字幕、表情和动作
 *
 * 回复：Agent 采纳后调 BeginReply，网关送来的句子和声音经 QueueSentence、BeginSentenceAudio 等进来
 * 播放：服务器 TickSentences 逐句开播，StartStreamSpeech 通知客户端，客户端 PlayRecordedVoice 播声音
 * 说完：FinishSentence 一句句结束，整轮说完 RecordSpokenReply 记进记忆，并让 800 cm 内的 NPC 听见
 * 反应：挨打、被吓等短促的声音从 PlayReaction 开始
 * 表情：TickExpression 每帧按倒地、挨打、说话情绪、倾听等定表情，并眨眼
 * 打没打中、给不给奖励不归这里管
 */
UCLASS(ClassGroup=(Delivery),meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryNpcPresentationComponent : public UActorComponent
{
    GENERATED_BODY()
    friend class FDeliveryNpcSpeechStreamTest;
    friend class FDeliveryNpcCompleteSpeechTest;
    friend class UDeliveryConversationComponent;
    friend class FDeliveryNpcProfileTest;
    friend class FDeliveryNpcFightTest;
    friend class FDeliveryNpcAudioStreamingTest;
    friend class FDeliveryNpcLiveAudioCommand;
    friend class FDeliveryConversationFeedbackTest;
    friend class FDeliveryNpcLookTest;
    friend class FDeliveryNpcModelSpeechSeparationTest;
    friend class FDeliveryNpcPlayerInterruptionTest;
    friend class FDeliveryNpcVoiceLinkTest;
public:
    UDeliveryNpcPresentationComponent();

    // ── 回复 ──
    // Agent 采纳回复后 BeginReply 开一轮；网关的句子和声音经下面几个函数进来；当前一句没说完时新回复先排队

    /// @brief 服务器直接让 NPC 说一句完整的话，不经网关，比如规则安排的台词
    /// @return 已安排播放时返回 true
    bool PublishSpeech(const FString& Text,FName Emotion,int32 Depth /* 第几层接话 */,FGuid ReplyToEventId=FGuid() /* 在回答哪件事 */,FGuid ModelReplyId=FGuid() /* 模型给的回复编号，空表示不是模型说的 */);

    /// @brief 停止说话，让各客户端停掉声音和字幕
    /// bFinishCurrentSentence 时把当前这句说完再停；bPreserveOtherReplies 时只停当前这轮
    void CancelSpeech(bool bFinishCurrentSentence=false /* true 时说完当前这句再停 */,bool bPreserveOtherReplies=false /* true 时只停当前这轮，待播的保留 */);

    /// @brief Agent 采纳回复后开一轮；当前这句还没说完时放进待播
    void BeginReply(FGuid ActionId /* 回复编号 */, FGuid MessageId /* 在回答哪句话 */, int32 Depth /* 第几层接话 */, AActor* Listener=nullptr);

    /// @brief 这轮是当前的或待播的
    bool HasReply(FGuid ReplyId) const { return ReplyId.IsValid() && (ReplyId==ReplyAction || ReplyId==PendingReply.ReplyId); }

    /// @brief 这轮正在播
    bool IsReplyPlaying(FGuid ReplyId) const { return ReplyId.IsValid() && Speech.bActive && (Speech.ActionId==ReplyId || Speech.TurnId==ReplyId); }

    /// @brief 取消一轮回复
    void CancelReply(FGuid ReplyId);

    /// @brief 一次取消多轮回复：先清掉待播的，再停当前这句，免得取消途中把待播的提上来
    void CancelReplies(const TArray<FGuid>& ReplyIds);

    /// @brief 告诉这一轮共几句，最多 3 句；0 句表示只有动作没有台词
    void SetReplySentenceCount(FGuid ReplyId,int32 Count);

    /// @brief 收到一整句（文字加完整声音）
    void QueueSentence(FGuid ActionId /* 回复编号 */, FGuid MessageId /* 在回答哪句话 */, const FDeliveryNpcSentence& Sentence);

    /// @brief 一句声音开始：记下文字和格式，开始边收边压缩
    /// @return 属于当前回复、数据合法时返回 true，这时声音可能还在压缩或排队
    bool BeginSentenceAudio(FGuid ActionId /* 回复编号 */, FGuid MessageId /* 在回答哪句话 */, const FDeliveryNpcSpeechBegin& Begin);

    /// @brief 收到一句声音的一段
    bool AppendSentenceAudio(FGuid ActionId /* 回复编号 */, FGuid MessageId /* 在回答哪句话 */, const FDeliveryNpcAudioChunk& Chunk);

    /// @brief 一句声音收完
    bool EndSentenceAudio(FGuid ActionId /* 回复编号 */, FGuid MessageId /* 在回答哪句话 */, const FDeliveryNpcAudioEnd& End);

    /// @brief 这一轮数据收完，共 Count 句
    void FinishSentences(FGuid ActionId, int32 Count);

    /// @brief 模型说完了，而且这一轮明确没有台词
    bool IsReplyWithoutSpeech(FGuid ActionId) const { return ActionId.IsValid() && (LastSilentReply==ActionId || (ReplyAction==ActionId && ExpectedSentences==0)); }

    /// @brief 还有正在说或等着说的话
    bool HasPendingSpeech() const { return ReplyAction.IsValid() || PendingReply.ReplyId.IsValid() || Speech.bActive; }

    /// @brief 本机正在播合成的声音
    bool IsRecordedVoicePlaying() const;

    // ── 补听 ──
    // 玩家走近时，交谈组件按段向这里要还在播的声音；说完后再留 5 秒，被打断就删

    /// @brief 取某句声音从 Offset 开始的一段
    /// @return 还留着这句、Offset 合法时返回 true
    bool GetSpeechAudioChunk(FGuid TurnId /* 哪一句 */,int32 Offset /* 从第几个字节开始 */,int32 MaxBytes /* 这段最多多少字节 */,TArray<uint8>& OutData,int32& OutTotal /* 整句多少字节 */) const;

    /// @brief 开麦对话时，主人离得再远也能收这句声音；换了主人或缓存过期就不行
    bool CanPlayerReceiveLinkedAudio(const APlayerController* Player,FGuid TurnId) const;

    // ── 声音配置 ──

    /// 后端合成语音用的音色名
    UPROPERTY(EditAnywhere,Category="Delivery|语音") FName TtsVoice=TEXT("male");

    /// 这位 NPC 的声音资产：短反应音和音量
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|语音") TObjectPtr<UDeliveryNpcVoiceSet> VoiceSet;

    // ── 反应 ──
    // 挨打、被吓时的短促叫声；正在说话时不播，同一件事只播一次

    /// @brief 服务器为一件事播一声短反应
    /// @return 播出时返回 true；正在说话、这件事播过、没配声音时返回 false
    UFUNCTION(BlueprintCallable,BlueprintAuthorityOnly,Category="Delivery|语音") bool PlayReaction(EDeliveryNpcVoiceReaction Reaction,FGuid EventId);

    /// @brief 停掉正在播的短反应
    void StopReaction();

    // ── 表情 ──

    /// 各种情绪对应的形态键权重
    UPROPERTY(EditAnywhere,Category="Delivery|表情") TObjectPtr<UDeliveryNpcExpressionSet> Expressions;

    // ── 播放 ──
    // 字幕、动画和表情都从这里读本机正在说的那句

    /// @brief 服务器同步过来的当前这句
    UFUNCTION(BlueprintPure) FDeliveryNpcSpeechState GetSpeech() const { return Speech; }

    /// @brief 本机正在播的这句，字幕和动作都读它；可能比 GetSpeech 慢一点
    UFUNCTION(BlueprintPure) FDeliveryNpcSpeechState GetDisplayedSpeech() const;

    /// @brief 回复已经到了本机、还没开始播时返回 true，表情代码据此让 NPC 显示 Thinking 表情
    bool IsWaitingForSpeechPlayback() const;

    /// @brief 是否在刚听见玩家说话后的短暂倾听姿势里
    bool IsShowingListeningReaction() const;

    /// @brief 这句已经开播，但声音暂时断了，在等后面的数据
    bool IsWaitingForMoreAudio() const;

    /// @brief 动作做完、这轮话还没说完时，继续用模型选的表情
    FName GetActionEmotion() const;

    /// @brief 本机这句播到几成，0 到 1，字幕据此逐字显示
    UFUNCTION(BlueprintPure) float GetDisplayedSpeechProgress() const;

    // ── 旧动画配置 ──
    // 还没接 ABP_NpcBody 的旧 NPC 才用这些，正式街坊的动作在动画蓝图里

    /// 站着不动
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> IdleAnimation;

    /// 走路
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> WalkAnimation;

    /// 踱步
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> PaceAnimation;

    /// 快步走
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> HurryAnimation;

    /// 走路动画原本对应的速度，按实际速度调播放快慢
    UPROPERTY(EditAnywhere,Category="Delivery|表演",meta=(ClampMin="10",Units="cm/s")) float WalkAnimationSpeed=180;

    /// 踱步动画原本对应的速度
    UPROPERTY(EditAnywhere,Category="Delivery|表演",meta=(ClampMin="10",Units="cm/s")) float PaceAnimationSpeed=80;

    /// 快步动画原本对应的速度
    UPROPERTY(EditAnywhere,Category="Delivery|表演",meta=(ClampMin="10",Units="cm/s")) float HurryAnimationSpeed=240;

    /// 出拳
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> AttackAnimation;

    /// 倒地
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> DownAnimation;

    /// 说话
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> TalkAnimation;

    /// 起身
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> GetUpAnimation;

    /// 挨打后仰
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> HitAnimation;

    /// 拽车
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> HoldAnimation;

    /// 起跳
    UPROPERTY(EditAnywhere,Category="Delivery|表演|跳跃") TObjectPtr<UAnimSequence> JumpStartAnimation;

    /// 空中
    UPROPERTY(EditAnywhere,Category="Delivery|表演|跳跃") TObjectPtr<UAnimSequence> JumpLoopAnimation;

    /// 落地
    UPROPERTY(EditAnywhere,Category="Delivery|表演|跳跃") TObjectPtr<UAnimSequence> JumpLandAnimation;

    /// 模型上张嘴的形态键名，说话时按音量调
    UPROPERTY(EditAnywhere,Category="Delivery|表演") FName MouthMorph;

    /// 生气的形态键名
    UPROPERTY(EditAnywhere,Category="Delivery|表演") FName AngryMorph;

    /// 电子音的基础音高
    UPROPERTY(EditAnywhere,Category="Delivery|表演",meta=(ClampMin="80",ClampMax="500",Units="Hz")) float VoiceFrequency=200;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn) override;
private:
    // ── 旧动画配置 ──

    /// 给没接动画蓝图的旧 NPC 挑动画播，正式街坊由 ABP_NpcBody 处理
    void UpdateLegacyAnimation(class ADeliveryNpcCharacter* Resident);

    // ── 播放 ──
    // 服务器写 Speech 同步出去，客户端收到后排进 LocalVoices，按顺序播声音和字幕

    /// 服务器当前这句
    UPROPERTY(ReplicatedUsing=OnRep_Speech) FDeliveryNpcSpeechState Speech;

    /// 客户端收到新的一句，排进本机播放队列
    UFUNCTION() void OnRep_Speech();

    /// 没有合成声音时，按文字播电子音
    UPROPERTY(Transient) TObjectPtr<UDeliveryDialogueVoiceComponent> Voice;

    /// 头顶字幕
    UPROPERTY(Transient) TObjectPtr<UWidgetComponent> Caption;

    /// 本机镜头离 NPC 超过这个距离就不显示头顶字幕，声音照常按距离变小
    UPROPERTY(EditAnywhere,Category="Delivery|语音",meta=(ClampMin="300",ClampMax="10000",Units="cm")) float CaptionVisibleDistance=1500.f;

    /// 播合成声音的音频组件
    UPROPERTY(Transient) TObjectPtr<UAudioComponent> RecordedVoice;

    /// 边收边往里写的声音
    UPROPERTY(Transient) TObjectPtr<USoundWaveProcedural> RecordedWave;

    /// 播短反应的音频组件
    UPROPERTY(Transient) TObjectPtr<UAudioComponent> ReactionAudio;

    // ── 反应 ──

    /// 所有端按服务器时间播这声短反应
    UFUNCTION(NetMulticast,Reliable) void MulticastPlayReaction(EDeliveryNpcVoiceReaction Reaction,FGuid EventId /* 因为哪件事，同一件事只播一次 */,double StartTime /* 服务器开始时间 */);

    /// 所有端停掉这声短反应；停止先到也记下，迟到的播放通知不再播
    UFUNCTION(NetMulticast,Reliable) void MulticastStopReaction(FGuid EventId);

    /// 短反应播完就停
    void TickReaction();

    /// 正在播的反应属于哪件事
    FGuid ReactionEventId;

    /// 正在播的出拳声属于哪一拳
    FGuid PlayingStrike;

    /// 播过的事，同一件只播一次
    TArray<FGuid> SeenReactionEvents;

    /// 被停掉的事
    TArray<FGuid> CancelledReactionEvents;

    /// 短反应什么时候播完
    double ReactionEndsAt=0;

    /// 组件结束，不再播反应
    bool bReactionsEnded=false;

    // ── 播放 ──
    // 服务器 → 客户端：开一句、分段发声音、说完、停掉

    /// 收到一段压缩声音，拼进对应那句
    UFUNCTION(NetMulticast,Reliable) void ReceiveAudio(FGuid TurnId /* 哪一句 */,int32 Offset /* 这段从第几个字节开始 */,int32 Total /* 整句多少字节 */,const TArray<uint8>& Data);

    /// 开一句：先收文字和编号，之后的声音归到这句
    UFUNCTION(NetMulticast,Reliable) void StartStreamSpeech(FDeliveryNpcSpeechState State);

    /// 停掉某一轮，bStopAll 时全部停
    UFUNCTION(NetMulticast,Reliable) void StopStreamSpeech(FGuid ActionId,bool bStopAll=false /* true 时所有回复都停 */);

    /// 这一轮说完当前这句就结束
    UFUNCTION(NetMulticast,Reliable) void FinishReplyAfterSentence(FGuid ActionId);

    /// 一句声音发完，带上总长和是否成功
    UFUNCTION(NetMulticast,Reliable) void FinishStreamAudio(FGuid TurnId,int32 PcmBytes /* 解压后共多少字节 */,int32 EncodedBytes /* 压缩后共多少字节 */,bool bSucceeded /* 合成成功了没有 */);

    // ── 回复 ──

    /// 当前这轮回复的编号；第一句还在合成时也同步，客户端据此先摆出模型选的表情
    UPROPERTY(Replicated) FGuid ReplyAction;

    /// 当前这轮在回答哪句话
    FGuid ReplyMessage;

    /// 当前这轮一起采纳的动作
    FGuid ReplyGameplayAction;

    /// 动作目标
    TWeakObjectPtr<AActor> ReplyTarget;

    /// 采纳时有目标
    bool bReplyHadTarget=false;

    /// 最近一轮没有台词的回复
    FGuid LastSilentReply;

    /// 等当前这句说完再播的下一轮
    FDeliveryNpcPendingReply PendingReply;

    /// 等着开播的纯文字回复
    FDeliveryNpcCompleteReply DeferredCompleteReply;

    /// 排一轮纯文字回复
    bool QueueCompleteReply(const FDeliveryNpcCompleteReply& Reply);

    /// 开播一轮纯文字回复
    bool StartCompleteReply(const FDeliveryNpcCompleteReply& Reply);

    /// 能播了就开播等着的纯文字回复
    bool StartDeferredCompleteReply();

    /// 扔掉待播的那一轮，并告诉 Agent
    void DiscardPendingReply();

    /// 当前这轮说完，把待播的提成当前
    void PromotePendingReply();

    /// 当前这轮说给谁听
    TWeakObjectPtr<AActor> ReplyListener;

    /// 客户端：正在向服务器要哪句的声音
    FGuid RequestedAudioTurn;

    /// 下次重新要声音的时间
    double NextAudioRetry=0;

    /// 已经要了几次
    int32 AudioAttempts=0;

    /// 当前这轮收到的整句，按序号
    TMap<int32,FDeliveryNpcSentence> Sentences;

    /// 已经压缩好的整句
    TMap<int32,FDeliveryNpcEncodedSentence> EncodedSentences;

    /// 正在边收边压缩的句子
    TMap<int32,FDeliveryNpcGeneratingSentence> GeneratingSentences;

    /// 还没完成的后台压缩任务数
    int32 PendingEncodeTasks=0;

    /// 服务器正在分段发出的声音
    TArray<uint8> SendingAudio;

    /// 服务器留着补发的声音，最多三句
    TMap<FGuid,FDeliveryNpcServerVoice> ServerVoices;

    /// 本机播放队列
    TArray<FDeliveryNpcLocalVoice> LocalVoices;

    /// 被停掉的回复，迟到的数据直接丢；最多记 16 个
    TArray<FGuid> CancelledVoiceActions;

    /// 播完的句子，最多记 16 个
    TArray<FGuid> FinishedVoiceTurns;

    /// 本机正在播的这句
    FDeliveryNpcSpeechState DisplayedSpeech;

    /// 这轮已经说出口的台词，说完后记进记忆
    FString SpokenReply;

    /// 下一句的序号、这轮共几句、已发出多少字节
    int32 NextSentence=0, ExpectedSentences=INDEX_NONE, SendingOffset=0;

    /// 服务器每帧：删掉过期的补听声音；当前没在说就把待播的一轮提上来；按序号开播下一句并分段发出声音；
    /// 一句播完调用 FinishSentence，整轮播完调用 RecordSpokenReply 记进记忆，再通知 Agent 的 FinishReplyPlayback
    void TickSentences();

    /// 客户端每帧：把收到的声音写进播放器，播完一句换下一句
    void PlayRecordedVoice();

    /// 开麦对话时找到这句要传给的主人
    APlayerState* ResolveRadioListener(FGuid MessageId) const;

    /// 这位玩家就是二狗现在的主人
    bool IsRadioListenerForPlayer(const APlayerController* Player,const APlayerState* Listener) const;

    /// 本机玩家是这句的开麦收听者
    bool IsLocalRadioListener(const FDeliveryNpcSpeechState& State) const;

    /// 设音量和是否按距离衰减；开麦对话时直接传到耳边
    void ConfigureRecordedVoice(bool bRadio /* true 表示开麦对话，声音直接传到主人耳边 */);

    /// 正在按开麦方式播
    bool bPlayingRadioVoice=false;

    /// 客户端把一句排进本机队列
    void AcceptLocalSpeech(const FDeliveryNpcSpeechState& State,bool bFromReplication /* true 表示来自同步的 Speech，false 表示来自开播通知 */);

    /// 声音失败时改用文字和电子音播完这句
    void UseTextForLocalVoice(FDeliveryNpcLocalVoice& Local);

    /// 这句还收不收声音
    bool CanReceiveVoiceData(const FDeliveryNpcLocalVoice& Local) const;

    /// 删掉过期的补发声音，最多留三句
    void TrimVoiceCache();

    /// 取消所有后台压缩
    void CancelSentenceGeneration();

    /// 下一句压缩出第一段就开播
    bool TryStartGeneratedSentence();

    /// 边压缩边把新的部分发出去
    void RefreshGeneratedSentence(int32 Sequence);

    /// 本机这句播完，从队列移走
    void FinishLocalSentence();

    /// 本机这句已经播了多少字节
    int32 GetConsumedVoiceBytes() const;

    /// 服务器这句是否真的说完：时间到了、发完了、主机自己也播完了
    bool HasFinishedSentence() const;

    /// 结束这句，留 5 秒给补发，排下一句
    void FinishSentence();

    /// 这轮说完：记成自己说过的话，并让 800 cm 内的 NPC 听见
    void RecordSpokenReply(bool bCompleteReply /* true 表示整轮都说完了 */);

    /// 当前声音的响度，用来张嘴
    float GetVoiceAmplitude() const;

    /// 每帧定表情：倒地 > 挨打 > 说话情绪 > 倾听 > 受惊 > 动作情绪 > 思考，再眨眼
    void TickExpression(float DeltaTime);

    /// 各形态键当前的权重，平滑过渡
    TMap<FName,float> ExpressionWeights;

    /// 下次眨眼和这次眨眼开始的时间
    double NextBlink=0, BlinkStarted=-1;

    /// 旧动画：正在播的那段
    TWeakObjectPtr<UAnimSequence> Playing;

    /// 旧动画：说话动作对应的那句
    FGuid PlayedTurn;

    /// 当前这轮的接话层数
    int32 SpeechDepth=0;

    /// 服务器时间，各端按它对齐
    double ServerTime() const;
};
