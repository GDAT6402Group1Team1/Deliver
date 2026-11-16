#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/Speech/DeliveryNpcSpeechTiming.h"
#include "AI/Presentation/Speech/DeliveryNpcVoiceCodec.h"
#include "AI/Presentation/Speech/DeliveryNpcVoiceSet.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Audio/DeliveryDialogueVoiceComponent.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundWaveProcedural.h"
#include "Sound/SoundAttenuation.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Async/Async.h"
#include "VoiceModule.h"

void UDeliveryNpcPresentationComponent::BeginReply(FGuid ActionId, FGuid MessageId, int32 Depth,AActor* Listener)
{
    if (!GetOwner()->HasAuthority() || !ActionId.IsValid() || HasReply(ActionId)) return;
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (Npc && !Npc->Agent->CanRetainReplySpeech(ActionId)) return;
    const auto State=Npc?Npc->Agent->GetPublicState():FDeliveryNpcPublicState();
    const FGuid OriginalHead=ReplyAction;
    const FGuid OriginalSpeech=Speech.TurnId;
    if (ReplyAction.IsValid() || Speech.bActive)
    {
        // 同一请求者可以更新待播内容，另一玩家的回复不能被无条件覆盖
        if (PendingReply.ReplyId.IsValid())
        {
            if (!Npc || !Npc->Agent->CanReplacePendingReply(PendingReply.ReplyId,ActionId)) return;
            DiscardPendingReply();
            if (!IsValid(Npc) || Npc->IsActorBeingDestroyed() || !Npc->Agent->CanRetainReplySpeech(ActionId)) return;
            if (PendingReply.ReplyId.IsValid() || ReplyAction!=OriginalHead || Speech.TurnId!=OriginalSpeech) return;
        }
        PendingReply.ReplyId=ActionId; PendingReply.MessageId=MessageId; PendingReply.Depth=FMath::Clamp(Depth,0,3);
        PendingReply.ActionId=State.ActionId; PendingReply.Target=State.Target;
        PendingReply.Listener=Listener?Listener:State.SpeechTarget.Get();
        return;
    }
    if (ReplyAction.IsValid() || Speech.bActive || PendingReply.ReplyId.IsValid()) CancelSpeech();
    Sentences.Reset(); SpokenReply.Reset(); NextSentence=0; ExpectedSentences=INDEX_NONE;
    ReplyAction=ActionId; ReplyMessage=MessageId;
    ReplyGameplayAction=State.ActionId; ReplyTarget=State.Target; bReplyHadTarget=IsValid(State.Target);
    SpeechDepth=FMath::Clamp(Depth,0,3);
    GetOwner()->ForceNetUpdate();
    ReplyListener=Listener?Listener:State.SpeechTarget.Get();
}

void UDeliveryNpcPresentationComponent::DiscardPendingReply()
{
    const FGuid Removed=PendingReply.ReplyId;
    PendingReply={};
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->Agent->CancelReply(Removed);
}

void UDeliveryNpcPresentationComponent::CancelReply(FGuid ReplyId)
{
    CancelReplies({ReplyId});
}

void UDeliveryNpcPresentationComponent::CancelReplies(const TArray<FGuid>& ReplyIds)
{
    if (!GetOwner()->HasAuthority() || ReplyIds.IsEmpty()) return;
    if (ReplyIds.Contains(PendingReply.ReplyId)) DiscardPendingReply();
    FDeliveryNpcPendingReply Waiting=MoveTemp(PendingReply);
    PendingReply={};
    if (ReplyIds.Contains(ReplyAction) || ReplyIds.Contains(Speech.ActionId) ||
        (!Speech.ActionId.IsValid() && ReplyIds.Contains(Speech.TurnId))) CancelSpeech(false,true);
    // 即使旧回复已离开服务器播放队首，远端仍须可靠地收到该 ReplyId 的取消
    for (const FGuid& ReplyId:ReplyIds) if (ReplyId.IsValid()) StopStreamSpeech(ReplyId,false);
    if (!IsValid(this) || !IsValid(GetOwner()) || GetOwner()->IsActorBeingDestroyed()) return;
    if (const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()); Npc && Waiting.ReplyId.IsValid())
    {
        if (!PendingReply.ReplyId.IsValid() && Npc->Agent->CanRetainReplySpeech(Waiting.ReplyId)) PendingReply=MoveTemp(Waiting);
        else if (PendingReply.ReplyId!=Waiting.ReplyId) Npc->Agent->CancelReply(Waiting.ReplyId);
    }
    if (!ReplyAction.IsValid()) PromotePendingReply();
}

void UDeliveryNpcPresentationComponent::SetReplySentenceCount(FGuid ReplyId,int32 Count)
{
    if (!GetOwner()->HasAuthority() || !ReplyId.IsValid() || Count<0 || Count>3) return;
    if (Count==0) LastSilentReply=ReplyId;
    if (ReplyId==ReplyAction) ExpectedSentences=Count;
    else if (ReplyId==PendingReply.ReplyId) PendingReply.SentenceCount=Count;
}

void UDeliveryNpcPresentationComponent::PromotePendingReply()
{
    if (!PendingReply.ReplyId.IsValid() || Speech.bActive) return;
    if (const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()); Npc && !Npc->Agent->CanRetainReplySpeech(PendingReply.ReplyId))
    { DiscardPendingReply(); return; }
    // A→B→A 的子回复不能在 A 首句结束时挤掉仍未播完的祖先
    if (const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
        Npc && !Npc->Agent->CanStartReplyMessageAudio(PendingReply.ReplyId)) return;
    FDeliveryNpcPendingReply Waiting=MoveTemp(PendingReply);
    PendingReply={};
    // 当前句已经说完，只记下已经播出的内容；其余旧台词不再播放
    CancelSpeech(true);
    if (!IsValid(this) || !IsValid(GetOwner()) || GetOwner()->IsActorBeingDestroyed()) return;
    if (const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()); Npc && !Npc->Agent->CanRetainReplySpeech(Waiting.ReplyId)) return;
    if (ReplyAction.IsValid() || Speech.bActive || PendingReply.ReplyId.IsValid())
    {
        if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->Agent->CancelReply(Waiting.ReplyId);
        return;
    }
    BeginReply(Waiting.ReplyId,Waiting.MessageId,Waiting.Depth,Waiting.Listener.Get());
    if (Waiting.CompleteReply.ReplyId.IsValid()) { QueueCompleteReply(Waiting.CompleteReply); return; }
    for (int32 Sequence=0;Sequence<3;++Sequence)
    {
        if (const auto* Whole=Waiting.Whole.Find(Sequence)) QueueSentence(Waiting.ReplyId,Waiting.MessageId,*Whole);
        if (const auto* Begin=Waiting.Begins.Find(Sequence))
        {
            BeginSentenceAudio(Waiting.ReplyId,Waiting.MessageId,*Begin);
            if (const auto* Parts=Waiting.Chunks.Find(Sequence))
                for (const auto& Part:*Parts) AppendSentenceAudio(Waiting.ReplyId,Waiting.MessageId,Part);
            if (const auto* End=Waiting.Ends.Find(Sequence)) EndSentenceAudio(Waiting.ReplyId,Waiting.MessageId,*End);
        }
    }
    if (Waiting.SentenceCount!=INDEX_NONE) SetReplySentenceCount(Waiting.ReplyId,Waiting.SentenceCount);
    if (Waiting.bTransferFinished) FinishSentences(Waiting.ReplyId,Waiting.SentenceCount);
}

void UDeliveryNpcPresentationComponent::QueueSentence(FGuid ActionId, FGuid MessageId, const FDeliveryNpcSentence& Sentence)
{
    if (GetOwner()->HasAuthority() && ActionId.IsValid() && ActionId==PendingReply.ReplyId && MessageId==PendingReply.MessageId)
    {
        if (Sentence.Sequence>=0 && Sentence.Sequence<3 && !PendingReply.Whole.Contains(Sentence.Sequence) &&
            !PendingReply.Begins.Contains(Sentence.Sequence) && Sentence.Pcm.Num()<=DeliveryNpcVoice::GetMaxPcmBytes(Sentence.SampleRate) && Sentence.Pcm.Num()%2==0 &&
            !Sentence.Text.IsEmpty() && Sentence.Text.Len()<=120 &&
            (PendingReply.SentenceCount==INDEX_NONE || Sentence.Sequence<PendingReply.SentenceCount))
            PendingReply.Whole.Add(Sentence.Sequence,Sentence);
        return;
    }
    if (!GetOwner()->HasAuthority() || ActionId!=ReplyAction || MessageId!=ReplyMessage ||
        Sentence.Sequence<NextSentence || Sentence.Sequence>2 || Sentences.Contains(Sentence.Sequence) || GeneratingSentences.Contains(Sentence.Sequence) ||
        Sentence.Pcm.Num()>DeliveryNpcVoice::GetMaxPcmBytes(Sentence.SampleRate) || Sentence.Pcm.Num()%2 || Sentence.Text.IsEmpty()) return;
    const double SubmitStarted=FPlatformTime::Seconds();
    Sentences.Add(Sentence.Sequence,Sentence);
    if (Sentence.Pcm.IsEmpty())
    {
        EncodedSentences.Add(Sentence.Sequence);
        return;
    }
    // 先在游戏线程加载 UE 模块，后台任务只处理独立的声音数据
    FVoiceModule::Get();
    const TWeakObjectPtr<UDeliveryNpcPresentationComponent> WeakThis(this);
    const int32 Sequence=Sentence.Sequence, SampleRate=Sentence.SampleRate;
    ++PendingEncodeTasks;
    Async(EAsyncExecution::ThreadPool,[WeakThis,ActionId,MessageId,Sequence,SampleRate,Pcm=Sentence.Pcm]()
    {
        FDeliveryNpcEncodedSentence Result;
        const double Started=FPlatformTime::Seconds();
        if (!DeliveryNpcVoice::Encode(Pcm,SampleRate,Result.Audio,Result.Error)) Result.Audio.Reset();
        Result.EncodeMs=(FPlatformTime::Seconds()-Started)*1000;
        AsyncTask(ENamedThreads::GameThread,[WeakThis,ActionId,MessageId,Sequence,Result=MoveTemp(Result)]() mutable
        {
            auto* Component=WeakThis.Get();
            if (!Component) return;
            --Component->PendingEncodeTasks;
            // 插话、倒地或结束关卡后，旧任务即使完成也不能重新安排播放
            if (Component->ReplyAction!=ActionId || Component->ReplyMessage!=MessageId || !Component->Sentences.Contains(Sequence)) return;
            UE_LOG(LogTemp,Log,TEXT("NpcSpeech encode_ready action=%s sequence=%d worker_ms=%.2f bytes=%d"),
                *ActionId.ToString(),Sequence,Result.EncodeMs,Result.Audio.Num());
            Component->EncodedSentences.Add(Sequence,MoveTemp(Result));
        });
    });
    UE_LOG(LogTemp,Log,TEXT("NpcSpeech encode_submitted action=%s sequence=%d pcm=%d game_thread_ms=%.3f"),
        *ActionId.ToString(),Sequence,Sentence.Pcm.Num(),(FPlatformTime::Seconds()-SubmitStarted)*1000);
}

void UDeliveryNpcPresentationComponent::FinishSentences(FGuid ActionId, int32 Count)
{
    if (GetOwner()->HasAuthority() && ActionId.IsValid() && ActionId==PendingReply.ReplyId)
    {
        if (Count<0 || Count>3 || (PendingReply.SentenceCount!=INDEX_NONE && PendingReply.SentenceCount!=Count))
        { DiscardPendingReply(); return; }
        for (int32 Index=0;Index<Count;++Index)
            if (!PendingReply.Whole.Contains(Index) && !PendingReply.Ends.Contains(Index)) { DiscardPendingReply(); return; }
        for (const auto& Pair:PendingReply.Whole)
            if (Pair.Key>=Count) { DiscardPendingReply(); return; }
        for (const auto& Pair:PendingReply.Begins)
            if (Pair.Key>=Count) { DiscardPendingReply(); return; }
        PendingReply.SentenceCount=Count; PendingReply.bTransferFinished=true;
        return;
    }
    if (!GetOwner()->HasAuthority() || ActionId!=ReplyAction) return;
    // 检查本次回复是否收齐，缺少句子时取消播放
    if (Count<NextSentence || Count>3) { CancelSpeech(); return; }
    for (int32 Index=NextSentence;Index<Count;++Index)
        if (!Sentences.Contains(Index) && !GeneratingSentences.Contains(Index)) { CancelSpeech(); return; }
    for (const auto& Pair:Sentences)
        if (Pair.Key>=Count) { CancelSpeech(); return; }
    for (const auto& Pair:GeneratingSentences)
        if (Pair.Key>=Count || !Pair.Value.bInputEnded) { CancelSpeech(); return; }
    ExpectedSentences=Count;
}

void UDeliveryNpcPresentationComponent::TrimVoiceCache()
{
    const double Now=FPlatformTime::Seconds();
    for (auto It=ServerVoices.CreateIterator();It;++It)
        if (It.Value().ExpiresAt>0 && Now>=It.Value().ExpiresAt) It.RemoveCurrent();
    // 最多保留三句已经说完的声音，当前正在播放的一句另由播放状态管理
    while (true)
    {
        int32 Completed=0;
        FGuid Oldest;
        double OldestExpiry=TNumericLimits<double>::Max();
        for (const auto& Entry:ServerVoices)
            if (Entry.Value.ExpiresAt>0)
            {
                ++Completed;
                if (Entry.Value.ExpiresAt<OldestExpiry) { Oldest=Entry.Key; OldestExpiry=Entry.Value.ExpiresAt; }
            }
        if (Completed<=3) break;
        ServerVoices.Remove(Oldest);
    }
}

void UDeliveryNpcPresentationComponent::TickSentences()
{
    auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc) return;
    TrimVoiceCache();
    // 流式父回复上一帧结束后队首已空，子回复仍须有提升机会
    if (GetOwner()->HasAuthority() && !ReplyAction.IsValid() && !Speech.bActive && PendingReply.ReplyId.IsValid())
    {
        PromotePendingReply();
        if (!IsValid(this) || !IsValid(Npc) || Npc->IsActorBeingDestroyed()) return;
    }
    if (GetOwner()->HasAuthority() && ReplyAction.IsValid())
    {
        if (!Npc->Agent->CanRetainReplySpeech(ReplyAction)) { CancelReply(ReplyAction); return; }
        if (Npc->IsKnockedDown() && !Npc->Combat->IsDeliberatelyProne()) { CancelSpeech(); return; }
        if (DeferredCompleteReply.ReplyId.IsValid())
        { StartDeferredCompleteReply(); return; }
        FinishSentence();
        if (!Speech.bActive && PendingReply.ReplyId.IsValid()) PromotePendingReply();
        if (!Speech.bActive && ExpectedSentences==NextSentence)
        {
            const FGuid CompletedReply=ReplyAction;
            const FGuid CompletedSpeech=Speech.TurnId;
            RecordSpokenReply(true);
            if (ReplyAction==CompletedReply) { ReplyAction.Invalidate(); ReplyMessage.Invalidate(); }
            Npc->Agent->FinishReplyPlayback(CompletedReply,CompletedSpeech);
            GetOwner()->ForceNetUpdate();
        }
        else if (!Speech.bActive && Npc->Agent->CanStartReplySpeech(ReplyAction))
        {
            FDeliveryNpcSentence Sentence;FDeliveryNpcEncodedSentence Encoded;
            if (!TryStartGeneratedSentence() && EncodedSentences.Contains(NextSentence) && Sentences.RemoveAndCopyValue(NextSentence,Sentence))
            {
                EncodedSentences.RemoveAndCopyValue(NextSentence,Encoded);
                Speech={}; Speech.TurnId=FGuid::NewGuid(); Speech.ActionId=ReplyAction; Speech.ReplyToEventId=ReplyMessage;
                Speech.RadioListener=ResolveRadioListener(ReplyMessage);
                Speech.Sequence=NextSentence++; Speech.Text=Sentence.Text; Speech.Emotion=Sentence.Emotion;
                Speech.SampleRate=Sentence.SampleRate;
                SendingAudio=MoveTemp(Encoded.Audio); SendingOffset=0;
                Speech.bRecordedVoice=!Sentence.Pcm.IsEmpty() && !SendingAudio.IsEmpty();
                if (Speech.bRecordedVoice)
                {
                    Speech.AudioFormatVersion=DeliveryNpcVoice::FormatVersion;
                    Speech.PcmBytes=Sentence.Pcm.Num(); Speech.EncodedBytes=SendingAudio.Num();
                    UE_LOG(LogTemp,Log,TEXT("NpcSpeech encoded turn=%s pcm=%d encoded=%d rate=%d encode_ms=%.2f"),
                        *Speech.TurnId.ToString(),Speech.PcmBytes,Speech.EncodedBytes,Speech.SampleRate,Encoded.EncodeMs);
                }
                else if (!Sentence.Pcm.IsEmpty())
                {
                    SendingAudio.Reset();
                    UE_LOG(LogTemp,Warning,TEXT("NpcSpeech text_only turn=%s reason=EncodeFailed detail=%s"),*Speech.TurnId.ToString(),*Encoded.Error);
                }
                const int32 PlaybackBytes=Speech.bRecordedVoice?DeliveryNpcVoice::GetDecodedPcmBytes(Speech.PcmBytes,Speech.SampleRate):0;
                Speech.Duration=Speech.bRecordedVoice?float(PlaybackBytes)/(2*Sentence.SampleRate):FMath::Clamp(Sentence.Text.Len()*.075f+.5f,1.f,10.f);
                Speech.StartTime=ServerTime()+.25; Speech.bActive=true;
                if (!SendingAudio.IsEmpty())
                {
                    auto& SavedVoice=ServerVoices.Add(Speech.TurnId);
                    SavedVoice.RadioListener=Speech.RadioListener;
                    SavedVoice.Pcm=Sentence.Pcm;
                    SavedVoice.Encoded=SendingAudio;
                }
                GetOwner()->ForceNetUpdate(); OnRep_Speech();
                StartStreamSpeech(Speech);
                if (auto* Local=LocalVoices.FindByPredicate([this](const auto& Item){ return Item.State.TurnId==Speech.TurnId; }))
                {
                    if (Speech.bRecordedVoice)
                    {
                        Local->Pcm=MoveTemp(Sentence.Pcm);
                        Local->Pcm.AddZeroed(PlaybackBytes-Local->Pcm.Num());
                    }
                    Local->TotalBytes=PlaybackBytes; Local->LastDataAt=FPlatformTime::Seconds();
                }
                UE_LOG(LogTemp,Log,TEXT("NpcLatency speech=%s turn=%s stage=play_scheduled sequence=%d bytes=%d"),
                    *ReplyMessage.ToString(),*Speech.TurnId.ToString(),Speech.Sequence,SendingAudio.Num());
            }
        }
    }
    // 按播放进度发送语音，提前准备约 0.4 秒的声音
    // 网络只传 Opus 压缩数据，主机播放保留的原始声音
    if (GetOwner()->HasAuthority() && Speech.bActive)
    {
        const bool bGenerating=Speech.AudioFormatVersion==DeliveryNpcVoice::StreamingFormatVersion;
        // 生成中的声音尽快发出，单帧仍保持 2 KB 上限
        int32 SendBytes=bGenerating?FMath::Clamp(SendingAudio.Num()-SendingOffset,0,2048):
            DeliveryNpcSpeech::GetSendBytes(ServerTime()-Speech.StartTime,Speech.Duration,SendingAudio.Num(),SendingOffset);
        while (SendBytes>0)
        {
            const int32 Size=FMath::Min(2048,SendBytes);
            TArray<uint8> Chunk; Chunk.Append(SendingAudio.GetData()+SendingOffset,Size);
            ReceiveAudio(Speech.TurnId,SendingOffset,bGenerating && !Speech.bAudioComplete?0:SendingAudio.Num(),Chunk);
            SendingOffset+=Size;
            SendBytes-=Size;
        }
    }
    if (GetNetMode()==NM_Client && !LocalVoices.IsEmpty())
    {
        auto& Local=LocalVoices[0];
        if (RequestedAudioTurn!=Local.State.TurnId)
        { AudioAttempts=0;NextAudioRetry=0;RequestedAudioTurn=Local.State.TurnId; }
        const double Now=FPlatformTime::Seconds();
        // 收到的声音停在同一位置超过半秒时，请求下一小段内容
        if (Local.State.bRecordedVoice && AudioAttempts<32 && Now>=NextAudioRetry && Now-Local.LastDataAt>.5 &&
            ((Local.State.AudioFormatVersion==DeliveryNpcVoice::StreamingFormatVersion && !Local.State.bAudioComplete) ||
             Local.Encoded.Num()<Local.State.EncodedBytes))
            if (const auto* PC=GetWorld()->GetFirstPlayerController())
                if (auto* Conversation=PC->FindComponentByClass<UDeliveryConversationComponent>())
                {
                    ++AudioAttempts; NextAudioRetry=Now+.25;
                    Conversation->ServerRequestNpcAudio(Npc,Local.State.TurnId,Local.Encoded.Num());
                }
    }
    if (GetNetMode()!=NM_DedicatedServer) PlayRecordedVoice();
}

APlayerState* UDeliveryNpcPresentationComponent::ResolveRadioListener(FGuid MessageId) const
{
    const auto* Companion=Cast<ADeliveryCompanionCharacter>(GetOwner());
    if (!IsValid(Companion) || !Companion->HasAuthority() || Companion->IsActorBeingDestroyed() ||
        !Companion->Companion || !MessageId.IsValid()) return nullptr;
    auto* Listener=Companion->Companion->GetOwnerPlayerState();
    if (!IsValid(Listener) || !GetWorld()) return nullptr;
    for (auto It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        const auto* Player=It->Get();
        if (!IsRadioListenerForPlayer(Player,Listener)) continue;
        const auto* Conversation=Player->FindComponentByClass<UDeliveryConversationComponent>();
        if (Conversation && Conversation->IsVoiceReplyMessage(MessageId,Companion)) return Listener;
    }
    return nullptr;
}

bool UDeliveryNpcPresentationComponent::IsRadioListenerForPlayer(const APlayerController* Player,const APlayerState* Listener) const
{
    const auto* Companion=Cast<ADeliveryCompanionCharacter>(GetOwner());
    return IsValid(Companion) && !Companion->IsActorBeingDestroyed() && Companion->Companion &&
        IsValid(Player) && !Player->IsActorBeingDestroyed() && Player->GetWorld()==GetWorld() &&
        IsValid(Listener) && !Listener->IsActorBeingDestroyed() && !Listener->IsInactive() &&
        Listener->GetWorld()==GetWorld() && Player->PlayerState==Listener &&
        Companion->Companion->GetOwnerPlayerState()==Listener;
}

bool UDeliveryNpcPresentationComponent::IsLocalRadioListener(const FDeliveryNpcSpeechState& State) const
{
    if (!State.RadioListener || !GetWorld()) return false;
    for (auto It=GetWorld()->GetPlayerControllerIterator();It;++It)
        if (const auto* Player=It->Get();Player && Player->IsLocalController() &&
            IsRadioListenerForPlayer(Player,State.RadioListener)) return true;
    return false;
}

bool UDeliveryNpcPresentationComponent::CanPlayerReceiveLinkedAudio(const APlayerController* Player,FGuid TurnId) const
{
    const auto* Saved=ServerVoices.Find(TurnId);
    return GetOwner() && GetOwner()->HasAuthority() && TurnId.IsValid() && Saved &&
        (Saved->ExpiresAt<=0 || FPlatformTime::Seconds()<Saved->ExpiresAt) &&
        IsRadioListenerForPlayer(Player,Saved->RadioListener.Get());
}

bool UDeliveryNpcPresentationComponent::GetSpeechAudioChunk(FGuid TurnId,int32 Offset,int32 MaxBytes,TArray<uint8>& OutData,int32& OutTotal) const
{
    OutData.Reset(); OutTotal=0;
    const auto* Saved=ServerVoices.Find(TurnId);
    if (!GetOwner()->HasAuthority() || !Saved || Offset<0 || Offset>=Saved->Encoded.Num() ||
        (Saved->ExpiresAt>0 && FPlatformTime::Seconds()>=Saved->ExpiresAt)) return false;
    const int32 Size=FMath::Min(FMath::Clamp(MaxBytes,0,2048),Saved->Encoded.Num()-Offset);
    if (Size==0) return false;
    OutTotal=Saved->bComplete?Saved->Encoded.Num():0;OutData.Append(Saved->Encoded.GetData()+Offset,Size);
    return true;
}

bool UDeliveryNpcPresentationComponent::IsRecordedVoicePlaying() const
{ return RecordedVoice && RecordedVoice->IsPlaying(); }

void UDeliveryNpcPresentationComponent::StartStreamSpeech_Implementation(FDeliveryNpcSpeechState State)
{
    // 先接收台词和编号，随后收到的声音归入对应句子
    if (!State.bActive || CancelledVoiceActions.Contains(State.ActionId) || FinishedVoiceTurns.Contains(State.TurnId)) return;
    // 开始通知只更新本机队列，复制字段仍由服务器更新，避免随后到达的字段差量混入旧值
    AcceptLocalSpeech(State,false);
}

void UDeliveryNpcPresentationComponent::AcceptLocalSpeech(const FDeliveryNpcSpeechState& State,bool bFromReplication)
{
    if (GetNetMode()==NM_DedicatedServer || !State.bActive ||
        !State.ActionId.IsValid() || CancelledVoiceActions.Contains(State.ActionId) || FinishedVoiceTurns.Contains(State.TurnId)) return;
    const bool bGenerating=State.AudioFormatVersion==DeliveryNpcVoice::StreamingFormatVersion;
    const bool bUnknownLength=bGenerating && !State.bAudioComplete && State.PcmBytes==0 && State.EncodedBytes==0;
    const bool bKnownLength=State.PcmBytes>0 && State.PcmBytes<=DeliveryNpcVoice::GetMaxPcmBytes(State.SampleRate) && State.PcmBytes%2==0 &&
        State.EncodedBytes>0 && State.EncodedBytes<=DeliveryNpcVoice::MaxEncodedBytes;
    if (State.bRecordedVoice && ((State.AudioFormatVersion!=DeliveryNpcVoice::FormatVersion && !bGenerating) ||
        (State.SampleRate!=16000 && State.SampleRate!=24000) ||
        !(bGenerating?(State.bAudioComplete?bKnownLength:bUnknownLength):bKnownLength)))
    {
        UE_LOG(LogTemp,Warning,TEXT("NpcSpeech invalid_audio_format turn=%s version=%d complete=%d pcm=%d encoded=%d rate=%d replicated=%d"),
            *State.TurnId.ToString(),State.AudioFormatVersion,int32(State.bAudioComplete),State.PcmBytes,State.EncodedBytes,State.SampleRate,int32(bFromReplication));
        return;
    }
    if (LocalVoices.ContainsByPredicate([&](const auto& Item){ return Item.State.TurnId==State.TurnId; }))
    {
        // 复制与可靠通知的先后可能不同，同一句只补上句末信息，不重新排队
        if (bGenerating && State.bAudioComplete)
            FinishStreamAudio_Implementation(State.TurnId,State.PcmBytes,State.EncodedBytes,State.bRecordedVoice);
        return;
    }
    // 刚进入附近的玩家略过已经结束的句子，随后接收新的对白
    if (bFromReplication && !bGenerating && ServerTime()>=State.StartTime+State.Duration)
    {
        UE_LOG(LogTemp,Log,TEXT("NpcSpeech joined_after_sentence turn=%s"),*State.TurnId.ToString());
        return;
    }
    if (LocalVoices.Num()>=3)
    {
        UE_LOG(LogTemp,Warning,TEXT("NpcSpeech playback_queue_full turn=%s"),*State.TurnId.ToString());
        return;
    }
    auto& Local=LocalVoices.AddDefaulted_GetRef();
    Local.State=State;
    Local.TotalBytes=State.bRecordedVoice?DeliveryNpcVoice::GetDecodedPcmBytes(State.PcmBytes,State.SampleRate):0;
    Local.LastDataAt=FPlatformTime::Seconds();
    if (State.bRecordedVoice && !GetOwner()->HasAuthority()) Local.Decoder=MakeShared<FDeliveryNpcVoiceDecoder>();
}

void UDeliveryNpcPresentationComponent::FinishStreamAudio_Implementation(FGuid TurnId,int32 PcmBytes,int32 EncodedBytes,bool bSucceeded)
{
    auto* Local=LocalVoices.FindByPredicate([&](const auto& Item){ return Item.State.TurnId==TurnId; });
    if (!Local || Local->State.AudioFormatVersion!=DeliveryNpcVoice::StreamingFormatVersion ||
        !CanReceiveVoiceData(*Local) || !Local->State.bRecordedVoice) return;
    const bool bValid=bSucceeded && PcmBytes>0 && PcmBytes<=DeliveryNpcVoice::GetMaxPcmBytes(Local->State.SampleRate) && PcmBytes%2==0 &&
        EncodedBytes>0 && EncodedBytes<=DeliveryNpcVoice::MaxEncodedBytes && Local->Encoded.Num()<=EncodedBytes;
    if (!bValid || (Local->State.bAudioComplete &&
        (Local->State.PcmBytes!=PcmBytes || Local->State.EncodedBytes!=EncodedBytes)))
    {
        UseTextForLocalVoice(*Local);
        return;
    }
    if (DisplayedSpeech.bActive && DisplayedSpeech.TurnId==TurnId)
        Local->CaptionProgress=GetDisplayedSpeechProgress();
    Local->State.bAudioComplete=true;
    Local->State.PcmBytes=PcmBytes; Local->State.EncodedBytes=EncodedBytes;
    Local->TotalBytes=DeliveryNpcVoice::GetDecodedPcmBytes(PcmBytes,Local->State.SampleRate);
    Local->State.Duration=float(Local->TotalBytes)/(2*Local->State.SampleRate);
    if (Local->Pcm.Num()>Local->TotalBytes || (Local->Decoder && Local->Decoder->IsComplete() &&
        (Local->Decoder->GetSourcePcmBytes()!=PcmBytes || Local->Decoder->GetTotalPcmBytes()!=Local->TotalBytes)))
    {
        UseTextForLocalVoice(*Local);
        return;
    }
    if (DisplayedSpeech.TurnId==TurnId && DisplayedSpeech.bActive) DisplayedSpeech=Local->State;
}

void UDeliveryNpcPresentationComponent::UseTextForLocalVoice(FDeliveryNpcLocalVoice& Local)
{
    const bool bDisplayed=DisplayedSpeech.bActive && DisplayedSpeech.TurnId==Local.State.TurnId;
    const float DisplayedProgress=bDisplayed?GetDisplayedSpeechProgress():0.f;
    // 单句声音失败时保留已经确认的文字，后句继续使用自己的声音
    Local.State.bRecordedVoice=false; Local.State.bAudioComplete=true;
    Local.State.Duration=FMath::Clamp(Local.State.Text.Len()*.075f+.5f,1.f,10.f);
    Local.Pcm.Reset();Local.Encoded.Reset();Local.Decoder.Reset();
    Local.DecodeOffset=Local.TotalBytes=Local.QueuedBytes=0;
    Local.FrontSince=Local.DrainedAt=Local.TextStartedAt=0;
    Local.LastDataAt=FPlatformTime::Seconds();Local.bRebuffering=false;
    if (bDisplayed)
    {
        if (RecordedVoice) RecordedVoice->Stop();
        if (RecordedWave) RecordedWave->ResetAudio();
        RecordedWave=nullptr;
        Local.TextStartedAt=Local.LastDataAt-DisplayedProgress*Local.State.Duration;
        DisplayedSpeech=Local.State;
    }
}

void UDeliveryNpcPresentationComponent::FinishReplyAfterSentence_Implementation(FGuid ActionId)
{
    if (!ActionId.IsValid()) return;
    CancelledVoiceActions.AddUnique(ActionId);
    if (CancelledVoiceActions.Num()>16) CancelledVoiceActions.RemoveAt(0);
    if (GetOwner()->HasAuthority() && Speech.ActionId==ActionId) Speech.bActive=false;
    // 远端可能比主机晚几帧播完，保留它正在说的这句，后续旧句直接移除
    const FGuid KeepTurn=!LocalVoices.IsEmpty() && LocalVoices[0].State.ActionId==ActionId?LocalVoices[0].State.TurnId:FGuid();
    LocalVoices.RemoveAll([&](const auto& Item){ return Item.State.ActionId==ActionId && Item.State.TurnId!=KeepTurn; });
    if (KeepTurn.IsValid() && LocalVoices[0].ReceiveUntil==0)
        LocalVoices[0].ReceiveUntil=FPlatformTime::Seconds()+5;
}

bool UDeliveryNpcPresentationComponent::CanReceiveVoiceData(const FDeliveryNpcLocalVoice& Local) const
{
    // 只补齐普通续接时保留下来的那一句，不能借补发重新开始其他旧句
    if (Local.ReceiveUntil>0) return Local.ReceiveUntil>FPlatformTime::Seconds();
    return !CancelledVoiceActions.Contains(Local.State.ActionId);
}

void UDeliveryNpcPresentationComponent::StopStreamSpeech_Implementation(FGuid ActionId,bool bStopAll)
{
    auto RememberCancellation=[this](FGuid Id)
    {
        if (!Id.IsValid() || CancelledVoiceActions.Contains(Id)) return;
        CancelledVoiceActions.Add(Id);
        if (CancelledVoiceActions.Num()>16) CancelledVoiceActions.RemoveAt(0);
    };
    RememberCancellation(ActionId);
    if (!Speech.ActionId.IsValid() && Speech.TurnId==ActionId)
    {
        if (GetOwner()->HasAuthority()) Speech.bActive=false;
        if (Voice) Voice->StopSpeaking();
        if (RecordedVoice && !DisplayedSpeech.bActive && LocalVoices.IsEmpty()) RecordedVoice->Stop();
    }
    if (GetOwner()->HasAuthority() && Speech.ActionId==ActionId) Speech.bActive=false;
    if (bStopAll)
    {
        // 玩家插话时也停止前一轮尚未播完的尾音
        for (const auto& Local:LocalVoices) RememberCancellation(Local.State.ActionId);
        RememberCancellation(DisplayedSpeech.ActionId);
        RememberCancellation(Speech.ActionId.IsValid()?Speech.ActionId:Speech.TurnId);
        // 复制字段只在服务器修改，本机取消结果由播放队列和取消编号保存
        if (GetOwner()->HasAuthority()) Speech.bActive=false;
        UE_LOG(LogTemp,Log,TEXT("NpcSpeech stream_stop_received role=%d turn=%s action=%s"),
            int32(GetOwner()->GetLocalRole()),*Speech.TurnId.ToString(),*ActionId.ToString());
    }
    if (bStopAll || DisplayedSpeech.ActionId==ActionId)
    {
        bPlayingRadioVoice=false;
        DisplayedSpeech.bActive=false;
        if (RecordedVoice) RecordedVoice->Stop();
        if (RecordedWave) RecordedWave->ResetAudio();
        RecordedWave=nullptr;
    }
    LocalVoices.RemoveAll([&](const auto& Item){ return bStopAll || Item.State.ActionId==ActionId; });
}

FDeliveryNpcSpeechState UDeliveryNpcPresentationComponent::GetDisplayedSpeech() const
{
    if (DisplayedSpeech.bActive || !LocalVoices.IsEmpty() || Speech.ActionId.IsValid()) return DisplayedSpeech;
    if (CancelledVoiceActions.Contains(Speech.TurnId)) return {};
    return Speech;
}

bool UDeliveryNpcPresentationComponent::IsWaitingForSpeechPlayback() const
{
    if (DisplayedSpeech.bActive) return false;
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || !Npc->Agent) return false;
    const auto State=Npc->Agent->GetPublicState();
    if (!State.GetReplyId().IsValid() || CancelledVoiceActions.Contains(State.GetReplyId())) return false;
    // 后台结束时声音可能还在编码或传输，服务器会在播放结束或取消时清除本轮编号
    if (LocalVoices.IsEmpty()) return ReplyAction==State.GetReplyId();
    const auto& Local=LocalVoices[0];
    // 只查看客户端收到的当前回复，旧句子不能让新一轮交谈继续保持等待姿态
    if (Local.State.ActionId!=State.GetReplyId() ||
        Local.State.ReplyToEventId!=State.ReplyToEventId || !Local.State.bActive ||
        FinishedVoiceTurns.Contains(Local.State.TurnId)) return false;
    const bool bGenerating=Local.State.AudioFormatVersion==DeliveryNpcVoice::StreamingFormatVersion && Local.State.bRecordedVoice;
    const double WaitingSince=bGenerating?FMath::Max(Local.FrontSince,Local.LastDataAt):(Local.FrontSince>0?Local.FrontSince:Local.LastDataAt);
    // 与播放器使用相同的超时范围，音频中断后不继续保持注视
    return WaitingSince>0 && FPlatformTime::Seconds()-WaitingSince<=(bGenerating?5:Local.State.Duration+5);
}

int32 UDeliveryNpcPresentationComponent::GetConsumedVoiceBytes() const
{
    if (!RecordedWave || LocalVoices.IsEmpty() || !DisplayedSpeech.bActive || LocalVoices[0].State.TurnId!=DisplayedSpeech.TurnId) return 0;
    const auto& Local=LocalVoices[0];
    const int32 Limit=Local.TotalBytes>0?Local.TotalBytes:Local.Pcm.Num();
    return FMath::Clamp(Local.QueuedBytes-RecordedWave->GetAvailableAudioByteCount(),0,Limit);
}

float UDeliveryNpcPresentationComponent::GetDisplayedSpeechProgress() const
{
    if (DisplayedSpeech.bActive && !LocalVoices.IsEmpty())
    {
        const auto& Local=LocalVoices[0];
        if (!Local.State.bRecordedVoice)
            return FMath::Clamp(float((FPlatformTime::Seconds()-Local.TextStartedAt)/FMath::Max(.01f,Local.State.Duration)),0.f,1.f);
        if (Local.State.AudioFormatVersion==DeliveryNpcVoice::StreamingFormatVersion)
        {
            // 声音总长尚未确定时按阅读速度展开字幕，句末到达后保持已显示字数
            const float Estimate=FMath::Clamp(Local.State.Text.Len()*.075f+.5f,1.f,10.f)*Local.State.SampleRate*2;
            const float Progress=float(GetConsumedVoiceBytes())/(Local.TotalBytes>0?Local.TotalBytes:FMath::Max(1.f,Estimate));
            return FMath::Max(Local.CaptionProgress,FMath::Clamp(Progress,0.f,Local.State.bAudioComplete?1.f:.95f));
        }
        return float(GetConsumedVoiceBytes())/FMath::Max(1,Local.TotalBytes);
    }
    return Speech.bActive?FMath::Clamp(float((ServerTime()-Speech.StartTime)/FMath::Max(.01f,Speech.Duration)),0.f,1.f):0.f;
}

void UDeliveryNpcPresentationComponent::FinishLocalSentence()
{
    if (LocalVoices.IsEmpty()) return;
    bPlayingRadioVoice=false;
    FinishedVoiceTurns.Add(LocalVoices[0].State.TurnId);
    if (FinishedVoiceTurns.Num()>16) FinishedVoiceTurns.RemoveAt(0);
    UE_LOG(LogTemp,Log,TEXT("NpcSpeech audio_finished turn=%s role=%d consumed=%d total=%d"),
        *LocalVoices[0].State.TurnId.ToString(),int32(GetOwner()->GetLocalRole()),GetConsumedVoiceBytes(),LocalVoices[0].TotalBytes);
    if (RecordedVoice) RecordedVoice->Stop();
    if (RecordedWave) RecordedWave->ResetAudio();
    RecordedWave=nullptr; DisplayedSpeech.bActive=false;
    LocalVoices.RemoveAt(0);
}

bool UDeliveryNpcPresentationComponent::HasFinishedSentence() const
{
    if (!ReplyAction.IsValid() || Speech.ActionId!=ReplyAction || !Speech.bActive ||
        (Speech.AudioFormatVersion==DeliveryNpcVoice::StreamingFormatVersion && !Speech.bAudioComplete) ||
        ServerTime()<Speech.StartTime+Speech.Duration || SendingOffset<SendingAudio.Num()) return false;
    // 主机按自己的实际播放进度记录，远端玩家的接收速度不会阻止 NPC 接着行动
    // 专用服务器和关闭音频的世界按安排的播放时间结束
    const bool bHasLocalPlayback=GetNetMode()!=NM_DedicatedServer && GetWorld()->AllowAudioPlayback() &&
        (!Speech.bRecordedVoice || GetWorld()->GetAudioDeviceRaw()!=nullptr);
    return !bHasLocalPlayback || FinishedVoiceTurns.Contains(Speech.TurnId);
}

void UDeliveryNpcPresentationComponent::FinishSentence()
{
    if (!HasFinishedSentence()) return;
    Speech.bActive=false;
    if (const auto* Current=GeneratingSentences.Find(Speech.Sequence); Current && Current->bInputEnded)
        GeneratingSentences.Remove(Speech.Sequence);
    if (auto* VoiceData=ServerVoices.Find(Speech.TurnId)) VoiceData->ExpiresAt=FPlatformTime::Seconds()+5;
    TrimVoiceCache();
    SpokenReply+=Speech.Text;
    GetOwner()->ForceNetUpdate(); OnRep_Speech();
}

void UDeliveryNpcPresentationComponent::RecordSpokenReply(bool bCompleteReply)
{
    auto* Npc=CastChecked<ADeliveryNpcCharacter>(GetOwner());
    if (SpokenReply.IsEmpty()) return;
    FDeliveryNpcObservation Said; Said.Kind=TEXT("SelfSpeech"); Said.Source=EDeliveryNpcEvidenceSource::ActionResult;
    Said.Description=SpokenReply; Said.Lifetime=60;
    const TWeakObjectPtr<AActor> Listener(Npc->Agent->GetReplyListenerForPlayback(ReplyAction,ReplyListener.Get()));
    Said.Subject=Npc; Said.Addressee=Listener.Get();
    // 整段说完才标记玩家的问题已回答，中途停止仍保留前面说过的内容
    if (bCompleteReply)
    {
        Said.EventId=Speech.TurnId;
        if (Npc->Agent->CanLinkReplyPlayback(ReplyAction)) Said.ReplyToEventId=ReplyMessage;
    }
    Npc->Agent->ObserveEvent(Said);
    // 最后一轮仍让邻居记住完整台词，但不再安排下一轮接话
    if (bCompleteReply)
        for (TActorIterator<ADeliveryNpcCharacter> It(GetWorld());It;++It)
            if (*It!=Npc && !Npc->Agent->WasReplyTextDelivered(ReplyAction,*It) &&
                FVector::DistSquared(It->GetActorLocation(),Npc->GetActorLocation())<FMath::Square(800.f))
                It->HearSpeech(Npc,SpokenReply,Speech.TurnId,FMath::Min(SpeechDepth+1,3),
                    Listener.IsValid()?Listener.Get():Npc,Said.ReplyToEventId,SpeechDepth<3 && !Speech.RadioListener);
    if (bCompleteReply)
    {
        UE_LOG(LogTemp,Log,TEXT("NpcLatency speech=%s stage=reply_completed sentences=%d"),*ReplyMessage.ToString(),NextSentence);
    }
    else
    {
        UE_LOG(LogTemp,Log,TEXT("NpcSpeech saved_finished_sentences action=%s chars=%d"),*ReplyAction.ToString(),SpokenReply.Len());
    }
    SpokenReply.Reset();
}

void UDeliveryNpcPresentationComponent::ReceiveAudio_Implementation(FGuid TurnId,int32 Offset,int32 Total,const TArray<uint8>& Data)
{
    if (GetOwner()->HasAuthority() || !TurnId.IsValid() || Total<0 || Total>DeliveryNpcVoice::MaxEncodedBytes ||
        Data.IsEmpty() || Data.Num()>2048 || Offset<0 || Offset>DeliveryNpcVoice::MaxEncodedBytes-Data.Num()) return;
    auto* Local=LocalVoices.FindByPredicate([&](const auto& Item){ return Item.State.TurnId==TurnId; });
    if (!Local || !CanReceiveVoiceData(*Local) || !Local->Decoder) return;
    const bool bGenerating=Local->State.AudioFormatVersion==DeliveryNpcVoice::StreamingFormatVersion;
    if ((!bGenerating && (Total<=0 || Local->State.EncodedBytes!=Total)) ||
        (Total>0 && (Offset>Total || Data.Num()>Total-Offset)) ||
        (bGenerating && Local->State.bAudioComplete && (Offset+Data.Num()>Local->State.EncodedBytes ||
            (Total>0 && Total!=Local->State.EncodedBytes)))) return;
    // 补发内容可能与已收到的部分重叠，只添加后面的新内容
    if (Offset>Local->Encoded.Num() || Offset+Data.Num()<=Local->Encoded.Num()) return;
    const int32 AlreadyReceived=Local->Encoded.Num()-Offset;
    Local->Encoded.Append(Data.GetData()+AlreadyReceived,Data.Num()-AlreadyReceived);
    TArray<uint8> Decoded; FString Error; const double DecodeStarted=FPlatformTime::Seconds();
    bool bValid=Local->Decoder->TryDecodeAvailable(Local->Encoded,Local->DecodeOffset,Decoded,Error);
    if (bValid && Local->Decoder->GetSampleRate()>0)
    {
        bValid=Local->Decoder->GetSampleRate()==Local->State.SampleRate;
        if (!bGenerating || (Local->State.bAudioComplete && Local->Decoder->IsComplete()))
            bValid=bValid && Local->Decoder->GetSourcePcmBytes()==Local->State.PcmBytes &&
                Local->Decoder->GetTotalPcmBytes()==Local->TotalBytes;
    }
    const int32 Limit=bGenerating && !Local->State.bAudioComplete?DeliveryNpcVoice::MaxDecodedPcmBytes:Local->TotalBytes;
    bValid=bValid && Decoded.Num()<=Limit-Local->Pcm.Num();
    if (bValid && Total>0 && Local->Encoded.Num()==Total)
        bValid=Local->Decoder->IsComplete() && Local->Pcm.Num()+Decoded.Num()==Local->Decoder->GetTotalPcmBytes();
    if (!bValid)
    {
        UE_LOG(LogTemp,Warning,TEXT("NpcSpeech decode_failed turn=%s detail=%s"),*TurnId.ToString(),*Error);
        UseTextForLocalVoice(*Local);
        return;
    }
    Local->Pcm.Append(Decoded);
    Local->LastDataAt=FPlatformTime::Seconds();
    // 编码数据本身也带句末通知，可靠 RPC 或状态复制较晚到达时仍能确认总长
    if (bGenerating && Local->Decoder->IsComplete())
        FinishStreamAudio_Implementation(TurnId,Local->Decoder->GetSourcePcmBytes(),Local->Encoded.Num(),true);
    if (Offset==0 || Local->Encoded.Num()==Total)
        UE_LOG(LogTemp,Log,TEXT("NpcLatency turn=%s stage=audio_receive role=%d encoded=%d total=%d pcm=%d decode_ms=%.2f"),
            *TurnId.ToString(),int32(GetOwner()->GetLocalRole()),Local->Encoded.Num(),Total,Local->Pcm.Num(),(FPlatformTime::Seconds()-DecodeStarted)*1000);
}

void UDeliveryNpcPresentationComponent::PlayRecordedVoice()
{
    if (LocalVoices.IsEmpty()) return;
    if (bPlayingRadioVoice && !IsLocalRadioListener(DisplayedSpeech))
    {
        // 已离开本玩家归属的同伴不能继续使用旧句子的连麦播放权限
        UE_LOG(LogTemp,Log,TEXT("NpcSpeech radio_listener_revoked turn=%s"),*DisplayedSpeech.TurnId.ToString());
        FinishLocalSentence();
        return;
    }
    auto& Local=LocalVoices[0];
    const double Now=FPlatformTime::Seconds();
    if (Local.FrontSince==0) Local.FrontSince=Now;
    const bool bGenerating=Local.State.AudioFormatVersion==DeliveryNpcVoice::StreamingFormatVersion && Local.State.bRecordedVoice;
    const bool bHasBufferedAudio=RecordedWave && RecordedWave->GetAvailableAudioByteCount()>0;
    // 整句已经收齐后不再等网络数据，让播放器正常播完尾音
    const bool bHasCompleteAudio=Local.State.bAudioComplete && Local.TotalBytes>0 && Local.Pcm.Num()==Local.TotalBytes;
    // 后一句可能早已收齐，轮到它播放时重新计算缺音等待时间
    const double DataWaitingSince=FMath::Max(Local.FrontSince,Local.LastDataAt);
    const bool bTimedOut=bGenerating?
        (Now-Local.FrontSince>30 || (!bHasCompleteAudio && !bHasBufferedAudio && Now-DataWaitingSince>5)):
        Now-Local.FrontSince>Local.State.Duration+5;
    // 网络中断时结束本机播放，界面不会一直保留等待中的句子
    if (bTimedOut)
    {
        UE_LOG(LogTemp,Warning,TEXT("NpcSpeech audio_timeout turn=%s received=%d total=%d"),*Local.State.TurnId.ToString(),Local.Pcm.Num(),Local.TotalBytes);
        if (GetOwner()->HasAuthority()) CancelSpeech();
        else StopStreamSpeech_Implementation(Local.State.ActionId,false);
        return;
    }
    if (!Local.State.bRecordedVoice)
    {
        if (ServerTime()<Local.State.StartTime) return;
        if (Local.TextStartedAt==0)
        {
            StopReaction();
            Local.TextStartedAt=Now;DisplayedSpeech=Local.State;DisplayedSpeech.bActive=true;
            UE_LOG(LogTemp,Warning,TEXT("NpcSpeech text_only turn=%s reason=NoSynthesizedAudio"),*Local.State.TurnId.ToString());
        }
        if (Now-Local.TextStartedAt>=Local.State.Duration) FinishLocalSentence();
        return;
    }
    if (DisplayedSpeech.bActive && DisplayedSpeech.TurnId==Local.State.TurnId)
    {
        Local.CaptionProgress=GetDisplayedSpeechProgress();
        // 句末通知可能先于剩余音频到达，已知总长时也要检查本机是否真正播完
        if (bGenerating && RecordedWave && !bHasBufferedAudio)
        {
            const bool bNeedsMoreAudio=!Local.State.bAudioComplete || GetConsumedVoiceBytes()<Local.TotalBytes;
            // 记录游戏帧观察到的缺音，和后台生成时间一起定位句中停顿
            if (bNeedsMoreAudio && !Local.bRebuffering)
                UE_LOG(LogTemp,Log,TEXT("NpcLatency turn=%s stage=audio_buffer_empty role=%d received=%d queued=%d complete=%d"),
                    *Local.State.TurnId.ToString(),int32(GetOwner()->GetLocalRole()),Local.Pcm.Num(),Local.QueuedBytes,int32(Local.State.bAudioComplete));
            Local.bRebuffering=bNeedsMoreAudio;
        }
        const float ConfiguredResume=VoiceSet?VoiceSet->StreamingResumeBufferSeconds:.32f;
        const float ResumeSeconds=FMath::IsFinite(ConfiguredResume)?FMath::Clamp(ConfiguredResume,.08f,1.f):.32f;
        const float Pitch=RecordedVoice?RecordedVoice->PitchMultiplier:1.f;
        const int32 ResumeBytes=FMath::CeilToInt(Local.State.SampleRate*ResumeSeconds*Pitch)*2;
        const int32 PendingBytes=Local.Pcm.Num()-Local.QueuedBytes;
        const bool bHasWholeTail=Local.State.bAudioComplete && Local.TotalBytes>0 && Local.Pcm.Num()==Local.TotalBytes;
        const bool bCanResume=!Local.bRebuffering || PendingBytes>=ResumeBytes || bHasWholeTail;
        if (RecordedWave && PendingBytes>0 && bCanResume)
        {
            // 短暂缺音后先积累一小段，再沿用同一个声音对象接着播放
            RecordedWave->QueueAudio(Local.Pcm.GetData()+Local.QueuedBytes,PendingBytes);
            UE_LOG(LogTemp,Log,TEXT("NpcLatency turn=%s stage=audio_buffer_fill role=%d bytes=%d buffered_ms=%d resumed=%d"),
                *Local.State.TurnId.ToString(),int32(GetOwner()->GetLocalRole()),PendingBytes,
                int32(RecordedWave->GetAvailableAudioByteCount()*500.0/FMath::Max(1,Local.State.SampleRate)),int32(Local.bRebuffering));
            Local.QueuedBytes=Local.Pcm.Num();
            Local.bRebuffering=false;
        }
        if ((!bGenerating || Local.State.bAudioComplete) && Local.TotalBytes>0 && GetConsumedVoiceBytes()==Local.TotalBytes)
        {
            // 最后一段声音进入混音器后，留出一个短缓冲时间再停止组件
            if (Local.DrainedAt==0) Local.DrainedAt=Now;
            if (Now-Local.DrainedAt>=.15) FinishLocalSentence();
        }
        else Local.DrainedAt=0;
        return;
    }
    const float DefaultStart=GetDefault<UDeliveryNpcVoiceSet>()->StreamingStartBufferSeconds;
    const float ConfiguredStart=VoiceSet?VoiceSet->StreamingStartBufferSeconds:DefaultStart;
    const float StartSeconds=bGenerating?(FMath::IsFinite(ConfiguredStart)?FMath::Clamp(ConfiguredStart,.16f,2.f):DefaultStart):.12f;
    const auto* Companion=Cast<ADeliveryCompanionCharacter>(GetOwner());
    const float Pitch=Companion && FMath::IsFinite(Companion->DialoguePitch)?
        FMath::Clamp(Companion->DialoguePitch,.95f,1.08f):1.f;
    int32 InitialBuffer=FMath::CeilToInt(Local.State.SampleRate*StartSeconds*Pitch)*2;
    if (Local.TotalBytes>0) InitialBuffer=FMath::Min(InitialBuffer,Local.TotalBytes);
    if ((!bGenerating && Local.TotalBytes<=0) || Local.Pcm.Num()<InitialBuffer || ServerTime()<Local.State.StartTime) return;
    if (!RecordedVoice)
    {
        RecordedVoice=NewObject<UAudioComponent>(GetOwner());
        RecordedVoice->bAutoActivate=false;
        RecordedVoice->SetupAttachment(GetOwner()->GetRootComponent());
        RecordedVoice->RegisterComponent();
    }
    RecordedVoice->Stop();
    ConfigureRecordedVoice(IsLocalRadioListener(Local.State));
    RecordedVoice->SetPitchMultiplier(Pitch);
    RecordedWave=NewObject<USoundWaveProcedural>(this);
    RecordedWave->SetSampleRate(Local.State.SampleRate); RecordedWave->NumChannels=1;
    RecordedWave->Duration=INDEFINITELY_LOOPING_DURATION;
    RecordedWave->SoundGroup=SOUNDGROUP_Voice;
    // 主机离得较远时仍推进声音，保证句末和记忆记录能够正常完成
    RecordedWave->VirtualizationMode=EVirtualizationMode::PlayWhenSilent;
    // 从这句话的开头播放，字幕按音频组件实际读取的进度显示
    RecordedWave->QueueAudio(Local.Pcm.GetData(),Local.Pcm.Num());
    Local.QueuedBytes=Local.Pcm.Num();
    Local.bRebuffering=false;
    DisplayedSpeech=Local.State; DisplayedSpeech.bActive=true;
    StopReaction();
    RecordedVoice->SetSound(RecordedWave); RecordedVoice->Play();
    UE_LOG(LogTemp,Log,TEXT("NpcLatency speech=%s turn=%s stage=audio_play role=%d late_ms=%d"),
        *Local.State.ReplyToEventId.ToString(),*Local.State.TurnId.ToString(),int32(GetOwner()->GetLocalRole()),int32((ServerTime()-Local.State.StartTime)*1000));
}

void UDeliveryNpcPresentationComponent::ConfigureRecordedVoice(bool bRadio)
{
    if (!RecordedVoice) return;
    bPlayingRadioVoice=bRadio;
    // 每句恢复人物配置，上一句连麦不能改变下一句普通对白的音量和范围
    const float Volume=VoiceSet?VoiceSet->DialogueVolume:1.f;
    RecordedVoice->SetVolumeMultiplier(FMath::IsFinite(Volume)?FMath::Clamp(Volume,0.f,2.f):1.f);
    RecordedVoice->bAllowSpatialization=!bRadio;
    RecordedVoice->AttenuationSettings=!bRadio && VoiceSet?VoiceSet->DialogueAttenuation.Get():nullptr;
    RecordedVoice->bOverrideAttenuation=RecordedVoice->AttenuationSettings==nullptr;
    if (RecordedVoice->bOverrideAttenuation)
    {
        RecordedVoice->AttenuationOverrides=FSoundAttenuationSettings();
        RecordedVoice->AttenuationOverrides.bAttenuate=!bRadio;
        RecordedVoice->AttenuationOverrides.bSpatialize=!bRadio;
        RecordedVoice->AttenuationOverrides.DistanceAlgorithm=EAttenuationDistanceModel::Linear;
        RecordedVoice->AttenuationOverrides.AttenuationShape=EAttenuationShape::Sphere;
        RecordedVoice->AttenuationOverrides.AttenuationShapeExtents=FVector(600,0,0);
        RecordedVoice->AttenuationOverrides.FalloffDistance=1200;
    }
}

float UDeliveryNpcPresentationComponent::GetVoiceAmplitude() const
{
    if (!DisplayedSpeech.bActive || !DisplayedSpeech.bRecordedVoice || LocalVoices.IsEmpty() || LocalVoices[0].bRebuffering) return 0;
    const auto& Pcm=LocalVoices[0].Pcm;
    const int32 Start=GetConsumedVoiceBytes();
    const int32 End=FMath::Min(Start+640,Pcm.Num());
    double Power=0;
    for (int32 Index=Start;Index+1<End;Index+=2)
    {
        const int16 Sample=int16(uint16(Pcm[Index]) | (uint16(Pcm[Index+1])<<8));
        Power+=FMath::Square(double(Sample)/32768.0);
    }
    return End>Start?FMath::Clamp(float(FMath::Sqrt(Power/FMath::Max(1,(End-Start)/2))*5),0.f,1.f):0.f;
}
