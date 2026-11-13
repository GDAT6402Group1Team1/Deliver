#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AI/Decision/DeliveryNpcTypes.h"
#include "AI/Presentation/DeliveryNpcVoiceSet.h"
#include "DeliveryNpcPresentationComponent.generated.h"
class UAnimSequence;
class UDeliveryDialogueVoiceComponent;
class UWidgetComponent;
class UAudioComponent;
class USoundWaveProcedural;
class UDeliveryNpcExpressionSet;
class FDeliveryNpcVoiceDecoder;

USTRUCT(BlueprintType)
struct FDeliveryNpcSpeechState
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FGuid TurnId;
    UPROPERTY(BlueprintReadOnly) FGuid ActionId;
    UPROPERTY(BlueprintReadOnly) FGuid ReplyToEventId;
    UPROPERTY(BlueprintReadOnly) FString Text;
    UPROPERTY(BlueprintReadOnly) FName Emotion;
    UPROPERTY(BlueprintReadOnly) double StartTime=0;
    UPROPERTY(BlueprintReadOnly) float Duration=0;
    UPROPERTY(BlueprintReadOnly) bool bActive=false;
    UPROPERTY(BlueprintReadOnly) bool bRecordedVoice=false;
    UPROPERTY(BlueprintReadOnly) int32 Sequence=0;
    UPROPERTY(BlueprintReadOnly) int32 SampleRate=16000;
    UPROPERTY(BlueprintReadOnly) int32 AudioFormatVersion=0;
    UPROPERTY(BlueprintReadOnly) int32 PcmBytes=0;
    UPROPERTY(BlueprintReadOnly) int32 EncodedBytes=0;
};

// 保存本机收到的一句语音，前一句播放结束后再开始下一句
struct FDeliveryNpcLocalVoice
{
    FDeliveryNpcSpeechState State;
    TArray<uint8> Pcm;
    TArray<uint8> Encoded;
    TSharedPtr<FDeliveryNpcVoiceDecoder> Decoder;
    int32 DecodeOffset=0;
    int32 TotalBytes=0;
    int32 QueuedBytes=0;
    double LastDataAt=0;
    double FrontSince=0;
    double DrainedAt=0;
    double TextStartedAt=0;
};

struct FDeliveryNpcServerVoice
{
    TArray<uint8> Pcm;
    TArray<uint8> Encoded;
    double ExpiresAt=0;
};

// 后台只生成压缩数据，播放顺序仍由游戏线程决定
struct FDeliveryNpcEncodedSentence
{
    TArray<uint8> Audio;
    FString Error;
    double EncodeMs=0;
};

// 将 NPC 的台词和播放时间同步给所有玩家，在各自客户端播放声音、字幕和动画
// 攻击结果和任务奖励由对应的玩法系统处理
UCLASS(ClassGroup=(Delivery),meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryNpcPresentationComponent : public UActorComponent
{
    GENERATED_BODY()
    friend class FDeliveryNpcSpeechStreamTest;
    friend class FDeliveryNpcCompleteSpeechTest;
    friend class UDeliveryConversationComponent;
    friend class FDeliveryNpcProfileTest;
public:
    UDeliveryNpcPresentationComponent();
    // 服务器设置台词并通知客户端播放，返回 true 表示已安排播放
    bool PublishSpeech(const FString& Text,FName Emotion,int32 Depth,FGuid ReplyToEventId=FGuid());
    // 停止对白，并让各客户端停止对应声音和字幕
    void CancelSpeech();
    void BeginReply(FGuid ActionId, FGuid MessageId);
    void QueueSentence(FGuid ActionId, FGuid MessageId, const FDeliveryNpcSentence& Sentence);
    void FinishSentences(FGuid ActionId, int32 Count);
    // 模型已结束这次回复并明确没有台词时返回 true
    bool IsReplyWithoutSpeech(FGuid ActionId) const { return ActionId.IsValid() && ReplyAction==ActionId && ExpectedSentences==0; }
    bool HasPendingSpeech() const { return ReplyAction.IsValid() || Speech.bActive; }
    bool IsRecordedVoicePlaying() const;
    // 查询某句仍可补发的声音，句末保留五秒，打断后移除
    bool GetSpeechAudioChunk(FGuid TurnId,int32 Offset,int32 MaxBytes,TArray<uint8>& OutData,int32& OutTotal) const;
    UPROPERTY(EditAnywhere,Category="Delivery|语音") FName TtsVoice=TEXT("male");
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|语音") TObjectPtr<UDeliveryNpcVoiceSet> VoiceSet;
    // 服务器为一次事件播放短反应音，重复编号不会再次播放
    UFUNCTION(BlueprintCallable,BlueprintAuthorityOnly,Category="Delivery|语音") bool PlayReaction(EDeliveryNpcVoiceReaction Reaction,FGuid EventId);
    void StopReaction();
    UPROPERTY(EditAnywhere,Category="Delivery|表情") TObjectPtr<UDeliveryNpcExpressionSet> Expressions;
    UFUNCTION(BlueprintPure) FDeliveryNpcSpeechState GetSpeech() const { return Speech; }
    // 返回本机正在播放的句子，供字幕和人物表演使用
    UFUNCTION(BlueprintPure) FDeliveryNpcSpeechState GetDisplayedSpeech() const;
    // 当前回复已到达本机、但声音或文字还未开始播放时返回 true
    bool IsWaitingForSpeechPlayback() const;
    // 动作结束后若同一轮语音还没播完，继续使用模型选定的表情
    FName GetActionEmotion() const;
    UFUNCTION(BlueprintPure) float GetDisplayedSpeechProgress() const;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> IdleAnimation;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> WalkAnimation;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> PaceAnimation;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> HurryAnimation;
    UPROPERTY(EditAnywhere,Category="Delivery|表演",meta=(ClampMin="10",Units="cm/s")) float WalkAnimationSpeed=180;
    UPROPERTY(EditAnywhere,Category="Delivery|表演",meta=(ClampMin="10",Units="cm/s")) float PaceAnimationSpeed=80;
    UPROPERTY(EditAnywhere,Category="Delivery|表演",meta=(ClampMin="10",Units="cm/s")) float HurryAnimationSpeed=240;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> AttackAnimation;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> DownAnimation;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> TalkAnimation;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> GetUpAnimation;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> HitAnimation;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> HoldAnimation;
    // 在蓝图中填写模型已有的表情名称，播放时调整对应的形态键
    UPROPERTY(EditAnywhere,Category="Delivery|表演") FName MouthMorph;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") FName AngryMorph;
    UPROPERTY(EditAnywhere,Category="Delivery|表演",meta=(ClampMin="80",ClampMax="500",Units="Hz")) float VoiceFrequency=200;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn) override;
private:
    // 尚未配置主动画蓝图的旧 NPC 保留原播放入口，正式街坊由 ABP_NpcBody 处理
    void UpdateLegacyAnimation(class ADeliveryNpcCharacter* Resident);
    UPROPERTY(ReplicatedUsing=OnRep_Speech) FDeliveryNpcSpeechState Speech;
    UFUNCTION() void OnRep_Speech();
    UPROPERTY(Transient) TObjectPtr<UDeliveryDialogueVoiceComponent> Voice;
    UPROPERTY(Transient) TObjectPtr<UWidgetComponent> Caption;
    UPROPERTY(Transient) TObjectPtr<UAudioComponent> RecordedVoice;
    UPROPERTY(Transient) TObjectPtr<USoundWaveProcedural> RecordedWave;
    UPROPERTY(Transient) TObjectPtr<UAudioComponent> ReactionAudio;
    UFUNCTION(NetMulticast,Reliable) void MulticastPlayReaction(EDeliveryNpcVoiceReaction Reaction,FGuid EventId,double StartTime);
    UFUNCTION(NetMulticast,Reliable) void MulticastStopReaction(FGuid EventId);
    void TickReaction();
    FGuid ReactionEventId;
    FGuid PlayingStrike;
    TArray<FGuid> SeenReactionEvents;
    TArray<FGuid> CancelledReactionEvents;
    double ReactionEndsAt=0;
    bool bReactionsEnded=false;
    UFUNCTION(NetMulticast,Reliable) void ReceiveAudio(FGuid TurnId,int32 Offset,int32 Total,const TArray<uint8>& Data);
    UFUNCTION(NetMulticast,Reliable) void StartStreamSpeech(FDeliveryNpcSpeechState State);
    UFUNCTION(NetMulticast,Reliable) void StopStreamSpeech(FGuid ActionId,bool bStopAll=false);
    // 后台还在合成首句时也同步本轮编号，客户端据此保留模型已经选定的表情
    UPROPERTY(Replicated) FGuid ReplyAction;
    FGuid ReplyMessage;
    TWeakObjectPtr<AActor> ReplyListener;
    FGuid RequestedAudioTurn;
    double NextAudioRetry=0;
    int32 AudioAttempts=0;
    TMap<int32,FDeliveryNpcSentence> Sentences;
    TMap<int32,FDeliveryNpcEncodedSentence> EncodedSentences;
    int32 PendingEncodeTasks=0;
    TArray<uint8> SendingAudio;
    TMap<FGuid,FDeliveryNpcServerVoice> ServerVoices;
    TArray<FDeliveryNpcLocalVoice> LocalVoices;
    TArray<FGuid> CancelledVoiceActions;
    TArray<FGuid> FinishedVoiceTurns;
    FDeliveryNpcSpeechState DisplayedSpeech;
    FString SpokenReply;
    int32 NextSentence=0, ExpectedSentences=INDEX_NONE, SendingOffset=0;
    void TickSentences();
    void PlayRecordedVoice();
    void AcceptLocalSpeech(const FDeliveryNpcSpeechState& State,bool bFromReplication);
    void FinishLocalSentence();
    int32 GetConsumedVoiceBytes() const;
    bool HasFinishedSentence() const;
    void FinishSentence();
    void RecordSpokenReply(bool bCompleteReply);
    float GetVoiceAmplitude() const;
    void TickExpression(float DeltaTime);
    TMap<FName,float> ExpressionWeights;
    double NextBlink=0, BlinkStarted=-1;
    TWeakObjectPtr<UAnimSequence> Playing;
    FGuid PlayedTurn;
    int32 SpeechDepth=0;
    double ServerTime() const;
};
