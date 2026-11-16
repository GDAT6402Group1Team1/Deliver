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

// ── 回复 ──
// 一轮回复最多 3 句。Agent 收到模型的回答后调用 BeginReply 登记这一轮，之后每句的数据有两种来法：
// 整句一次到（QueueSentence，后台用 DeliveryNpcVoice::Encode 一次压完），或者边合成边到（Speech 生成文件里的 BeginSentenceAudio 等三个函数）
// 服务器每帧在 TickSentences 里按顺序开始下一句，把压缩后的声音分段发给客户端；句子说完、这一轮的句数也够了，就调用 Agent->FinishReplyPlayback
// NPC 还在说上一轮时，新的一轮先放进 PendingReply 排队，上一轮说完由 PromotePendingReply 接上；CancelReply 和 CancelReplies 打断指定的回复

/// @brief 服务器：登记一轮新回复，ActionId 是这轮回复的编号，MessageId 是在回答哪句话
/// 正在说别的话时排进 PendingReply；没在说话时这一轮马上成为当前轮 ReplyAction
/// Agent 的 CanRetainReplySpeech 不同意（这轮已经作废）时不登记
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
        // PendingReply 已经有一轮在等：Agent 的 CanReplacePendingReply 同意才换掉它（例如同一位玩家又说了一句），否则这一轮不登记
        // DiscardPendingReply 会通知 Agent，收到通知的代码可能改动状态，所以之后再检查一遍 NPC、PendingReply、当前轮和当前句都没变
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

    // 没在说话：清掉残留的旧记录，这一轮成为当前轮，收话人取 Listener，没有时取 PublicState.SpeechTarget
    if (ReplyAction.IsValid() || Speech.bActive || PendingReply.ReplyId.IsValid()) CancelSpeech();
    Sentences.Reset(); SpokenReply.Reset(); NextSentence=0; ExpectedSentences=INDEX_NONE;
    ReplyAction=ActionId; ReplyMessage=MessageId;
    ReplyGameplayAction=State.ActionId; ReplyTarget=State.Target; bReplyHadTarget=IsValid(State.Target);
    SpeechDepth=FMath::Clamp(Depth,0,3);
    GetOwner()->ForceNetUpdate();
    ReplyListener=Listener?Listener:State.SpeechTarget.Get();
}

/// @brief 丢掉排队的下一轮，并调用 Agent->CancelReply 告诉 Agent 这一轮不播了
void UDeliveryNpcPresentationComponent::DiscardPendingReply()
{
    const FGuid Removed=PendingReply.ReplyId;
    PendingReply={};
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->Agent->CancelReply(Removed);
}

/// @brief 打断一轮回复，转给 CancelReplies
void UDeliveryNpcPresentationComponent::CancelReply(FGuid ReplyId)
{
    CancelReplies({ReplyId});
}

/// @brief 服务器：打断 ReplyIds 里的回复，可能是当前轮、排队的下一轮，或者已经说完但客户端还在播的旧轮
/// 排队的下一轮不在 ReplyIds 里时保留下来，当前轮停掉后由 PromotePendingReply 接上
void UDeliveryNpcPresentationComponent::CancelReplies(const TArray<FGuid>& ReplyIds)
{
    if (!GetOwner()->HasAuthority() || ReplyIds.IsEmpty()) return;
    if (ReplyIds.Contains(PendingReply.ReplyId)) DiscardPendingReply();
    FDeliveryNpcPendingReply Waiting=MoveTemp(PendingReply);
    PendingReply={};
    if (ReplyIds.Contains(ReplyAction) || ReplyIds.Contains(Speech.ActionId) ||
        (!Speech.ActionId.IsValid() && ReplyIds.Contains(Speech.TurnId))) CancelSpeech(false,true);

    // 每个编号都调用一次 StopStreamSpeech：服务器上这轮已经说完，客户端可能还在播，也要收到停止通知
    for (const FGuid& ReplyId:ReplyIds) if (ReplyId.IsValid()) StopStreamSpeech(ReplyId,false);
    if (!IsValid(this) || !IsValid(GetOwner()) || GetOwner()->IsActorBeingDestroyed()) return;

    // 把先取出来的下一轮放回 PendingReply；这期间又有新的一轮排进来，或者 Agent 不再保留它时，通知 Agent 取消
    if (const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()); Npc && Waiting.ReplyId.IsValid())
    {
        if (!PendingReply.ReplyId.IsValid() && Npc->Agent->CanRetainReplySpeech(Waiting.ReplyId)) PendingReply=MoveTemp(Waiting);
        else if (PendingReply.ReplyId!=Waiting.ReplyId) Npc->Agent->CancelReply(Waiting.ReplyId);
    }
    if (!ReplyAction.IsValid()) PromotePendingReply();
}

/// @brief 服务器：模型说完了，这一轮共有 Count 句（0 到 3）；Count 为 0 时记进 LastSilentReply，表示这轮没有台词
void UDeliveryNpcPresentationComponent::SetReplySentenceCount(FGuid ReplyId,int32 Count)
{
    if (!GetOwner()->HasAuthority() || !ReplyId.IsValid() || Count<0 || Count>3) return;
    if (Count==0) LastSilentReply=ReplyId;
    if (ReplyId==ReplyAction) ExpectedSentences=Count;
    else if (ReplyId==PendingReply.ReplyId) PendingReply.SentenceCount=Count;
}

/// @brief 服务器：当前没在说话时，让排队的下一轮成为当前轮，并把它攒下的数据按句子顺序重新交给对应的函数
/// Agent 不再保留它时丢掉；它是在回答当前轮里一句还没播完的话时继续等
void UDeliveryNpcPresentationComponent::PromotePendingReply()
{
    if (!PendingReply.ReplyId.IsValid() || Speech.bActive) return;
    if (const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()); Npc && !Npc->Agent->CanRetainReplySpeech(PendingReply.ReplyId))
    { DiscardPendingReply(); return; }

    // NPC 之间接话时，A 说的话 B 回答、B 的话 A 再回答；CanStartReplyMessageAudio 不同意时，A 第一轮还有句子没说完，先不接上第二轮
    if (const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
        Npc && !Npc->Agent->CanStartReplyMessageAudio(PendingReply.ReplyId)) return;
    FDeliveryNpcPendingReply Waiting=MoveTemp(PendingReply);
    PendingReply={};

    // 调用 CancelSpeech(true)：当前轮已经播出的句子写进记忆，还没播的句子丢掉
    CancelSpeech(true);
    if (!IsValid(this) || !IsValid(GetOwner()) || GetOwner()->IsActorBeingDestroyed()) return;
    if (const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner()); Npc && !Npc->Agent->CanRetainReplySpeech(Waiting.ReplyId)) return;
    if (ReplyAction.IsValid() || Speech.bActive || PendingReply.ReplyId.IsValid())
    {
        if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner())) Npc->Agent->CancelReply(Waiting.ReplyId);
        return;
    }
    BeginReply(Waiting.ReplyId,Waiting.MessageId,Waiting.Depth,Waiting.Listener.Get());

    // 这一轮是只有文字的完整回复：交给 QueueCompleteReply；否则把攒下的整句、开始、分段、结束通知按句子顺序重放一遍
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

/// @brief 服务器：后端一次给出一整句（文字和可选的整段声音），属于排队的下一轮时先存进 PendingReply.Whole
/// 属于当前轮时存进 Sentences，有声音就交给线程池调用 DeliveryNpcVoice::Encode 一次压完，结果放进 EncodedSentences
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

    // UE 要求语音模块在游戏线程加载；后台任务只拿着这句声音的副本，不碰组件
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

            // 回到游戏线程：组件没了，或者当前轮已经换了（被插话、倒地、关卡结束打断），结果丢掉
            if (Component->ReplyAction!=ActionId || Component->ReplyMessage!=MessageId || !Component->Sentences.Contains(Sequence)) return;
            UE_LOG(LogTemp,Log,TEXT("NpcSpeech encode_ready action=%s sequence=%d worker_ms=%.2f bytes=%d"),
                *ActionId.ToString(),Sequence,Result.EncodeMs,Result.Audio.Num());
            Component->EncodedSentences.Add(Sequence,MoveTemp(Result));
        });
    });
    UE_LOG(LogTemp,Log,TEXT("NpcSpeech encode_submitted action=%s sequence=%d pcm=%d game_thread_ms=%.3f"),
        *ActionId.ToString(),Sequence,Sentence.Pcm.Num(),(FPlatformTime::Seconds()-SubmitStarted)*1000);
}

/// @brief 服务器：后端说这一轮的句子都发完了，共 Count 句；核对收到的句子，缺句或多句时取消这一轮
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

    // 还没说的每一句都要已经收到（整句或边合成边收的都算），边合成的还要已经收到结束通知；不对就调用 CancelSpeech
    if (Count<NextSentence || Count>3) { CancelSpeech(); return; }
    for (int32 Index=NextSentence;Index<Count;++Index)
        if (!Sentences.Contains(Index) && !GeneratingSentences.Contains(Index)) { CancelSpeech(); return; }
    for (const auto& Pair:Sentences)
        if (Pair.Key>=Count) { CancelSpeech(); return; }
    for (const auto& Pair:GeneratingSentences)
        if (Pair.Key>=Count || !Pair.Value.bInputEnded) { CancelSpeech(); return; }
    ExpectedSentences=Count;
}

// ── 补听 ──
// ServerVoices 留着已经说过的几句声音，刚走近的客户端可以通过交谈组件的 ServerRequestNpcAudio 补要；每句说完后最多再留 5 秒

/// @brief 删掉过期的声音，并只保留最近说完的 3 句
void UDeliveryNpcPresentationComponent::TrimVoiceCache()
{
    const double Now=FPlatformTime::Seconds();
    for (auto It=ServerVoices.CreateIterator();It;++It)
        if (It.Value().ExpiresAt>0 && Now>=It.Value().ExpiresAt) It.RemoveCurrent();

    // ExpiresAt 大于 0 的是已经说完的句子；超过 3 句时删掉最早过期的，正在说的那句 ExpiresAt 是 0，不算在内
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

// ── 播放 ──
// 服务器：TickSentences 每帧开始下一句，按播放进度把压缩声音分段调用 ReceiveAudio 发给客户端
// 客户端：收到的数据放进 LocalVoices 队列解压播放；数据停住超过半秒就向服务器补要

/// @brief 每帧调用：服务器上推进当前轮的句子和发送；客户端上检查要不要补要声音
void UDeliveryNpcPresentationComponent::TickSentences()
{
    auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc) return;
    TrimVoiceCache();

    // 当前轮已经结束而 PendingReply 里有一轮在等：先接上它（NPC 之间接话的下一层就是这样排进来的）
    if (GetOwner()->HasAuthority() && !ReplyAction.IsValid() && !Speech.bActive && PendingReply.ReplyId.IsValid())
    {
        PromotePendingReply();
        if (!IsValid(this) || !IsValid(Npc) || Npc->IsActorBeingDestroyed()) return;
    }

    // 当前轮：Agent 不再保留时打断；NPC 被打倒（故意躺下除外）时停止说话；有等着的完整回复就先开始它
    if (GetOwner()->HasAuthority() && ReplyAction.IsValid())
    {
        if (!Npc->Agent->CanRetainReplySpeech(ReplyAction)) { CancelReply(ReplyAction); return; }
        if (Npc->IsKnockedDown() && !Npc->Combat->IsDeliberatelyProne()) { CancelSpeech(); return; }
        if (DeferredCompleteReply.ReplyId.IsValid())
        { StartDeferredCompleteReply(); return; }

        // 结束到时间的句子；当前句说完、排队的下一轮在等时接上它
        FinishSentence();
        if (!Speech.bActive && PendingReply.ReplyId.IsValid()) PromotePendingReply();

        // 这一轮的句子都说完了：RecordSpokenReply 写进记忆，清掉 ReplyAction，调用 Agent->FinishReplyPlayback
        if (!Speech.bActive && ExpectedSentences==NextSentence)
        {
            const FGuid CompletedReply=ReplyAction;
            const FGuid CompletedSpeech=Speech.TurnId;
            RecordSpokenReply(true);
            if (ReplyAction==CompletedReply) { ReplyAction.Invalidate(); ReplyMessage.Invalidate(); }
            Npc->Agent->FinishReplyPlayback(CompletedReply,CompletedSpeech);
            GetOwner()->ForceNetUpdate();
        }

        // 还有句子要说，Agent 也同意开口：先试边合成边压的句子（TryStartGeneratedSentence），不行再用一次压完的整句
        // 整句：0.25 秒后开始，有声音时时长按声音算，没声音时按字数算；有声音就存进 ServerVoices 留着补发，本机直接播放原始声音
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

    // 服务器给客户端发 Opus 压缩数据，自己播放保留的原始声音；按播放进度发，比实际播到的位置提前约 0.4 秒
    if (GetOwner()->HasAuthority() && Speech.bActive)
    {
        const bool bGenerating=Speech.AudioFormatVersion==DeliveryNpcVoice::StreamingFormatVersion;

        // 边合成边压的句子：压好多少发多少，一帧最多 2 KB；一次压完的整句按 GetSendBytes 跟着播放进度发
        // 每段调用 ReceiveAudio，带上这段的起始位置和整句总长；边合成的句子没压完时总长传 0
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

    // 客户端：本机正在播的这句（LocalVoices[0]）换了，重置补要的次数和时间
    if (GetNetMode()==NM_Client && !LocalVoices.IsEmpty())
    {
        auto& Local=LocalVoices[0];
        if (RequestedAudioTurn!=Local.State.TurnId)
        { AudioAttempts=0;NextAudioRetry=0;RequestedAudioTurn=Local.State.TurnId; }
        const double Now=FPlatformTime::Seconds();

        // 收到的声音超过 0.5 秒没有新数据、而且还没收全时，调用 ServerRequestNpcAudio 从已收到的位置补要；每 0.25 秒最多一次，每句最多 32 次
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

    // 不是专用服务器时，把解压出的声音送进本机的播放器
    if (GetNetMode()!=NM_DedicatedServer) PlayRecordedVoice();
}

// ── 开麦收听 ──
// 二狗回答主人开麦说的话时，这句话直接传到主人耳边，不按距离衰减；其他玩家仍按距离听见
// ResolveRadioListener 在说这句前找出主人的 PlayerState，写进 Speech.RadioListener

/// @brief 服务器：这句回复是在回答主人开麦说的 MessageId 时，返回主人的 PlayerState；否则返回 nullptr
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

/// @brief Player 就是 Listener 这位主人，而且他现在仍是这只二狗的主人时返回 true
bool UDeliveryNpcPresentationComponent::IsRadioListenerForPlayer(const APlayerController* Player,const APlayerState* Listener) const
{
    const auto* Companion=Cast<ADeliveryCompanionCharacter>(GetOwner());
    return IsValid(Companion) && !Companion->IsActorBeingDestroyed() && Companion->Companion &&
        IsValid(Player) && !Player->IsActorBeingDestroyed() && Player->GetWorld()==GetWorld() &&
        IsValid(Listener) && !Listener->IsActorBeingDestroyed() && !Listener->IsInactive() &&
        Listener->GetWorld()==GetWorld() && Player->PlayerState==Listener &&
        Companion->Companion->GetOwnerPlayerState()==Listener;
}

/// @brief 本机玩家就是这句话的 RadioListener 时返回 true，本机播放时不按距离衰减
bool UDeliveryNpcPresentationComponent::IsLocalRadioListener(const FDeliveryNpcSpeechState& State) const
{
    if (!State.RadioListener || !GetWorld()) return false;
    for (auto It=GetWorld()->GetPlayerControllerIterator();It;++It)
        if (const auto* Player=It->Get();Player && Player->IsLocalController() &&
            IsRadioListenerForPlayer(Player,State.RadioListener)) return true;
    return false;
}

/// @brief 服务器：Player 是这句开麦回复的主人、而且这句声音还没过期时返回 true；离得远也可以补要这句声音
bool UDeliveryNpcPresentationComponent::CanPlayerReceiveLinkedAudio(const APlayerController* Player,FGuid TurnId) const
{
    const auto* Saved=ServerVoices.Find(TurnId);
    return GetOwner() && GetOwner()->HasAuthority() && TurnId.IsValid() && Saved &&
        (Saved->ExpiresAt<=0 || FPlatformTime::Seconds()<Saved->ExpiresAt) &&
        IsRadioListenerForPlayer(Player,Saved->RadioListener.Get());
}

/// @brief 服务器：从 ServerVoices 里取出 TurnId 这句从 Offset 开始、最多 2 KB 的压缩声音，给交谈组件补发
/// OutTotal 是这句压缩后的总长；边合成边压的句子还没压完时为 0
/// @return 没有这句、已经过期或 Offset 超出范围时返回 false
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

/// @brief 本机正在播放后端合成的声音时返回 true
bool UDeliveryNpcPresentationComponent::IsRecordedVoicePlaying() const
{ return RecordedVoice && RecordedVoice->IsPlaying(); }

// ── 客户端接收 ──
// 服务器开始一句时调用 StartStreamSpeech 通知所有客户端，之后 ReceiveAudio 分段送来压缩声音，FinishStreamAudio 送来句末的总长
// Speech 的属性同步也会调用 AcceptLocalSpeech；两条路先到的那条把这句排进 LocalVoices，后到的那条只补句末信息

/// @brief 多播：服务器开始说一句流式回复，客户端把这句排进 LocalVoices；已取消或已播完的句子不再排
void UDeliveryNpcPresentationComponent::StartStreamSpeech_Implementation(FDeliveryNpcSpeechState State)
{
    // 已经取消的轮、已经播完的句子不再排
    if (!State.bActive || CancelledVoiceActions.Contains(State.ActionId) || FinishedVoiceTurns.Contains(State.TurnId)) return;

    // 这里只调用 AcceptLocalSpeech 改本机的 LocalVoices，不改 Speech；Speech 只由服务器写，同步过来的差量才不会和本机改过的值混在一起
    AcceptLocalSpeech(State,false);
}

/// @brief 客户端（和主机本机）：检查这句的声音格式、采样率和长度，合法就排进 LocalVoices，最多排 3 句
/// @param bFromReplication true 表示由 Speech 的属性同步调用，false 表示由 StartStreamSpeech 调用
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
        // 这句已经在 LocalVoices 里（属性同步和 StartStreamSpeech 都到了）：边合成的句子带着句末信息时调用 FinishStreamAudio_Implementation，不重新排队
        if (bGenerating && State.bAudioComplete)
            FinishStreamAudio_Implementation(State.TurnId,State.PcmBytes,State.EncodedBytes,State.bRecordedVoice);
        return;
    }

    // 由属性同步调用、按时间这句已经说完：刚走近的玩家跳过这句，等下一句
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

    // 排进队列；不是主机时为这句建一个解码器 Decoder
    auto& Local=LocalVoices.AddDefaulted_GetRef();
    Local.State=State;
    Local.TotalBytes=State.bRecordedVoice?DeliveryNpcVoice::GetDecodedPcmBytes(State.PcmBytes,State.SampleRate):0;
    Local.LastDataAt=FPlatformTime::Seconds();
    if (State.bRecordedVoice && !GetOwner()->HasAuthority()) Local.Decoder=MakeShared<FDeliveryNpcVoiceDecoder>();
}

/// @brief 多播：边合成边压的这句已经压完，告诉客户端原始声音长度 PcmBytes 和压缩后长度 EncodedBytes
/// 数字不合理、和已经收到的数据对不上、或者 bSucceeded 为 false 时，这句改用 UseTextForLocalVoice 只显示文字
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

    // 先存下当前字幕进度 CaptionProgress，总长确定后字幕不往回退
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

/// @brief 本机这句声音出错：清掉声音数据，改成按字数计时、只显示文字；正在显示时从当前字幕进度接着显示
void UDeliveryNpcPresentationComponent::UseTextForLocalVoice(FDeliveryNpcLocalVoice& Local)
{
    const bool bDisplayed=DisplayedSpeech.bActive && DisplayedSpeech.TurnId==Local.State.TurnId;
    const float DisplayedProgress=bDisplayed?GetDisplayedSpeechProgress():0.f;

    // 只改这一句，LocalVoices 里后面的句子仍然播放自己的声音
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

// ── 停止 ──
// FinishReplyAfterSentence：ActionId 这轮说完当前句就停；StopStreamSpeech：马上停下 ActionId 这轮，bStopAll 为 true 时停下所有轮
// 停掉的轮编号记进 CancelledVoiceActions（最多 16 个），之后晚到的数据和同步都按这份记录丢掉

/// @brief 多播：ActionId 这轮说完当前句就停，LocalVoices 里这轮后面的句子删掉
void UDeliveryNpcPresentationComponent::FinishReplyAfterSentence_Implementation(FGuid ActionId)
{
    if (!ActionId.IsValid()) return;
    CancelledVoiceActions.AddUnique(ActionId);
    if (CancelledVoiceActions.Num()>16) CancelledVoiceActions.RemoveAt(0);
    if (GetOwner()->HasAuthority() && Speech.ActionId==ActionId) Speech.bActive=false;

    // 客户端可能比主机晚几帧播到这句：保留本机正在播的这句，给它 5 秒（ReceiveUntil）收完剩下的声音
    const FGuid KeepTurn=!LocalVoices.IsEmpty() && LocalVoices[0].State.ActionId==ActionId?LocalVoices[0].State.TurnId:FGuid();
    LocalVoices.RemoveAll([&](const auto& Item){ return Item.State.ActionId==ActionId && Item.State.TurnId!=KeepTurn; });
    if (KeepTurn.IsValid() && LocalVoices[0].ReceiveUntil==0)
        LocalVoices[0].ReceiveUntil=FPlatformTime::Seconds()+5;
}

/// @brief 本机这句还能收声音数据时返回 true
bool UDeliveryNpcPresentationComponent::CanReceiveVoiceData(const FDeliveryNpcLocalVoice& Local) const
{
    // 设了 ReceiveUntil 的句子（FinishReplyAfterSentence 保留下来的那句）只在期限内能收；其他句子所在的轮被取消后就不收
    if (Local.ReceiveUntil>0) return Local.ReceiveUntil>FPlatformTime::Seconds();
    return !CancelledVoiceActions.Contains(Local.State.ActionId);
}

/// @brief 多播：马上停下 ActionId 这轮的声音和字幕，删掉 LocalVoices 里这轮的句子
/// ActionId 也可能是一句完整回复的 TurnId，这时停掉电子语音 Voice
/// @param bStopAll true 时（玩家插话）本机所有轮都停下
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
        // bStopAll：本机队列里、正在显示的、Speech 上的每一轮都记进 CancelledVoiceActions
        for (const auto& Local:LocalVoices) RememberCancellation(Local.State.ActionId);
        RememberCancellation(DisplayedSpeech.ActionId);
        RememberCancellation(Speech.ActionId.IsValid()?Speech.ActionId:Speech.TurnId);

        // Speech 只在服务器上改；客户端靠 LocalVoices 和 CancelledVoiceActions 记住已经停了
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

// ── 显示查询 ──
// 字幕、口型和动画通过这几个函数读本机正在说的句子和进度

/// @brief 本机正在显示的句子：流式回复取 DisplayedSpeech，只有文字的完整回复取 Speech；已经停了的返回空
FDeliveryNpcSpeechState UDeliveryNpcPresentationComponent::GetDisplayedSpeech() const
{
    if (DisplayedSpeech.bActive || !LocalVoices.IsEmpty() || Speech.ActionId.IsValid()) return DisplayedSpeech;
    if (CancelledVoiceActions.Contains(Speech.TurnId)) return {};
    return Speech;
}

/// @brief Agent 已经有一轮回复、但本机还没开始播放（声音还在压缩或传输）时返回 true；动画用它让 NPC 保持倾听姿势
bool UDeliveryNpcPresentationComponent::IsWaitingForSpeechPlayback() const
{
    if (DisplayedSpeech.bActive) return false;
    const auto* Npc=Cast<ADeliveryNpcCharacter>(GetOwner());
    if (!Npc || !Npc->Agent) return false;
    const auto State=Npc->Agent->GetPublicState();
    if (!State.GetReplyId().IsValid() || CancelledVoiceActions.Contains(State.GetReplyId())) return false;

    // 本机还没收到任何句子：服务器上 ReplyAction 就是这轮时算在等；服务器播完或取消这轮时会清掉 ReplyAction
    if (LocalVoices.IsEmpty()) return ReplyAction==State.GetReplyId();
    const auto& Local=LocalVoices[0];

    // LocalVoices[0] 必须属于 Agent 当前这轮回复、回答的是同一句话，还没播完
    if (Local.State.ActionId!=State.GetReplyId() ||
        Local.State.ReplyToEventId!=State.ReplyToEventId || !Local.State.bActive ||
        FinishedVoiceTurns.Contains(Local.State.TurnId)) return false;
    const bool bGenerating=Local.State.AudioFormatVersion==DeliveryNpcVoice::StreamingFormatVersion && Local.State.bRecordedVoice;
    const double WaitingSince=bGenerating?FMath::Max(Local.FrontSince,Local.LastDataAt):(Local.FrontSince>0?Local.FrontSince:Local.LastDataAt);

    // 超时和 PlayRecordedVoice 一致：边合成的句子 5 秒没新数据，整句超过时长加 5 秒，就不再等
    return WaitingSince>0 && FPlatformTime::Seconds()-WaitingSince<=(bGenerating?5:Local.State.Duration+5);
}

/// @brief 本机播放器已经读走的声音字节数（已送进播放器的减去播放器里还剩的）
int32 UDeliveryNpcPresentationComponent::GetConsumedVoiceBytes() const
{
    if (!RecordedWave || LocalVoices.IsEmpty() || !DisplayedSpeech.bActive || LocalVoices[0].State.TurnId!=DisplayedSpeech.TurnId) return 0;
    const auto& Local=LocalVoices[0];
    const int32 Limit=Local.TotalBytes>0?Local.TotalBytes:Local.Pcm.Num();
    return FMath::Clamp(Local.QueuedBytes-RecordedWave->GetAvailableAudioByteCount(),0,Limit);
}

/// @brief 本机正在显示的句子进度，0 到 1；字幕按它显示到第几个字
float UDeliveryNpcPresentationComponent::GetDisplayedSpeechProgress() const
{
    if (DisplayedSpeech.bActive && !LocalVoices.IsEmpty())
    {
        const auto& Local=LocalVoices[0];
        if (!Local.State.bRecordedVoice)
            return FMath::Clamp(float((FPlatformTime::Seconds()-Local.TextStartedAt)/FMath::Max(.01f,Local.State.Duration)),0.f,1.f);
        if (Local.State.AudioFormatVersion==DeliveryNpcVoice::StreamingFormatVersion)
        {
            // 边合成的句子总长还不知道时按字数估算总长，最多显示到 95%；不比 CaptionProgress 小，字幕不往回退
            const float Estimate=FMath::Clamp(Local.State.Text.Len()*.075f+.5f,1.f,10.f)*Local.State.SampleRate*2;
            const float Progress=float(GetConsumedVoiceBytes())/(Local.TotalBytes>0?Local.TotalBytes:FMath::Max(1.f,Estimate));
            return FMath::Max(Local.CaptionProgress,FMath::Clamp(Progress,0.f,Local.State.bAudioComplete?1.f:.95f));
        }
        return float(GetConsumedVoiceBytes())/FMath::Max(1,Local.TotalBytes);
    }
    return Speech.bActive?FMath::Clamp(float((ServerTime()-Speech.StartTime)/FMath::Max(.01f,Speech.Duration)),0.f,1.f):0.f;
}

/// @brief 本机这句播完：记进 FinishedVoiceTurns（最多 16 个），停掉播放器，从 LocalVoices 删掉这句
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

/// @brief 服务器：当前句的时间到了、声音也都发出去了，而且本机播放器已经播完时返回 true
bool UDeliveryNpcPresentationComponent::HasFinishedSentence() const
{
    if (!ReplyAction.IsValid() || Speech.ActionId!=ReplyAction || !Speech.bActive ||
        (Speech.AudioFormatVersion==DeliveryNpcVoice::StreamingFormatVersion && !Speech.bAudioComplete) ||
        ServerTime()<Speech.StartTime+Speech.Duration || SendingOffset<SendingAudio.Num()) return false;

    // 主机看自己播完没有（FinishedVoiceTurns），不等客户端；专用服务器和没有音频设备的世界只看时间
    const bool bHasLocalPlayback=GetNetMode()!=NM_DedicatedServer && GetWorld()->AllowAudioPlayback() &&
        (!Speech.bRecordedVoice || GetWorld()->GetAudioDeviceRaw()!=nullptr);
    return !bHasLocalPlayback || FinishedVoiceTurns.Contains(Speech.TurnId);
}

/// @brief 服务器：当前句说完了，结束它；声音在 ServerVoices 里再留 5 秒，文字加进 SpokenReply
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

/// @brief 服务器：把这轮已经说完的句子 SpokenReply 作为一条 SelfSpeech 事件写进记忆
/// @param bCompleteReply true 时这轮完整说完：事件带上回答的是哪句话，并让 800 cm 内的 NPC 听到
void UDeliveryNpcPresentationComponent::RecordSpokenReply(bool bCompleteReply)
{
    auto* Npc=CastChecked<ADeliveryNpcCharacter>(GetOwner());
    if (SpokenReply.IsEmpty()) return;
    FDeliveryNpcObservation Said; Said.Kind=TEXT("SelfSpeech"); Said.Source=EDeliveryNpcEvidenceSource::ActionResult;
    Said.Description=SpokenReply; Said.Lifetime=60;
    const TWeakObjectPtr<AActor> Listener(Npc->Agent->GetReplyListenerForPlayback(ReplyAction,ReplyListener.Get()));
    Said.Subject=Npc; Said.Addressee=Listener.Get();

    // bCompleteReply 为 true 时才写 ReplyToEventId（这轮回答了那句话）；中途停下时只记住说过的内容
    if (bCompleteReply)
    {
        Said.EventId=Speech.TurnId;
        if (Npc->Agent->CanLinkReplyPlayback(ReplyAction)) Said.ReplyToEventId=ReplyMessage;
    }
    Npc->Agent->ObserveEvent(Said);

    // 让 800 cm 内还没收到的 NPC 听到；第三层或开麦对话时 bRequestReply 为 false，他们只记住不回话
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

// ── 本机播放 ──
// 客户端：ReceiveAudio 收到一段压缩声音就用 Decoder 解压，追加进 LocalVoices 里这句的 Pcm
// PlayRecordedVoice 每帧把还没送的 Pcm 送进播放器 RecordedWave；主机本机直接用原始声音，不解压

/// @brief 客户端 RPC：收到 TurnId 这句从 Offset 开始的一段压缩声音 Data，Total 是总长（边合成的句子没压完时为 0）
/// 解压出错或长度对不上时，这句改用 UseTextForLocalVoice 只显示文字
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

    // 补发的数据可能和已经收到的重叠：只追加 Encoded 末尾之后的部分；中间有缺口或全部重复时丢掉
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

    // 解码器在数据里读到了句末标记：直接调用 FinishStreamAudio_Implementation，不等 FinishStreamAudio 和属性同步
    if (bGenerating && Local->Decoder->IsComplete())
        FinishStreamAudio_Implementation(TurnId,Local->Decoder->GetSourcePcmBytes(),Local->Encoded.Num(),true);
    if (Offset==0 || Local->Encoded.Num()==Total)
        UE_LOG(LogTemp,Log,TEXT("NpcLatency turn=%s stage=audio_receive role=%d encoded=%d total=%d pcm=%d decode_ms=%.2f"),
            *TurnId.ToString(),int32(GetOwner()->GetLocalRole()),Local->Encoded.Num(),Total,Local->Pcm.Num(),(FPlatformTime::Seconds()-DecodeStarted)*1000);
}

/// @brief 每帧：播放 LocalVoices[0] 这句；还没开始时攒够起播缓冲再开始，播放中把新收到的声音送进播放器，播完调用 FinishLocalSentence
void UDeliveryNpcPresentationComponent::PlayRecordedVoice()
{
    if (LocalVoices.IsEmpty()) return;
    if (bPlayingRadioVoice && !IsLocalRadioListener(DisplayedSpeech))
    {
        // 这句正按开麦方式播放，但本机玩家已经不是二狗的主人：结束这句
        UE_LOG(LogTemp,Log,TEXT("NpcSpeech radio_listener_revoked turn=%s"),*DisplayedSpeech.TurnId.ToString());
        FinishLocalSentence();
        return;
    }
    auto& Local=LocalVoices[0];
    const double Now=FPlatformTime::Seconds();
    if (Local.FrontSince==0) Local.FrontSince=Now;
    const bool bGenerating=Local.State.AudioFormatVersion==DeliveryNpcVoice::StreamingFormatVersion && Local.State.bRecordedVoice;
    const bool bHasBufferedAudio=RecordedWave && RecordedWave->GetAvailableAudioByteCount()>0;

    // bHasCompleteAudio：这句的声音全部收齐，之后不再等网络数据，播放器照常播完结尾
    const bool bHasCompleteAudio=Local.State.bAudioComplete && Local.TotalBytes>0 && Local.Pcm.Num()==Local.TotalBytes;

    // 等待时间从这句排到队首（FrontSince）和最后收到数据（LastDataAt）里较晚的那个算起；收齐的句子不算缺音
    const double DataWaitingSince=FMath::Max(Local.FrontSince,Local.LastDataAt);
    const bool bTimedOut=bGenerating?
        (Now-Local.FrontSince>30 || (!bHasCompleteAudio && !bHasBufferedAudio && Now-DataWaitingSince>5)):
        Now-Local.FrontSince>Local.State.Duration+5;

    // 超时：边合成的句子在队首超过 30 秒，或者没收齐、播放器也空了、5 秒没新数据；整句超过时长加 5 秒
    // 主机调用 CancelSpeech，客户端只停本机这轮
    if (bTimedOut)
    {
        UE_LOG(LogTemp,Warning,TEXT("NpcSpeech audio_timeout turn=%s received=%d total=%d"),*Local.State.TurnId.ToString(),Local.Pcm.Num(),Local.TotalBytes);
        if (GetOwner()->HasAuthority()) CancelSpeech();
        else StopStreamSpeech_Implementation(Local.State.ActionId,false);
        return;
    }

    // 只有文字的句子：到了 StartTime 开始显示字幕，过了时长就结束
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

        // 这句已经在播：播放器空了而声音还没收完（或还没读完），记为缺音 bRebuffering
        if (bGenerating && RecordedWave && !bHasBufferedAudio)
        {
            const bool bNeedsMoreAudio=!Local.State.bAudioComplete || GetConsumedVoiceBytes()<Local.TotalBytes;

            // 刚开始缺音时打一条日志，和后台合成的时间对照，查句子中间的停顿
            if (bNeedsMoreAudio && !Local.bRebuffering)
                UE_LOG(LogTemp,Log,TEXT("NpcLatency turn=%s stage=audio_buffer_empty role=%d received=%d queued=%d complete=%d"),
                    *Local.State.TurnId.ToString(),int32(GetOwner()->GetLocalRole()),Local.Pcm.Num(),Local.QueuedBytes,int32(Local.State.bAudioComplete));
            Local.bRebuffering=bNeedsMoreAudio;
        }

        // 缺音后攒够 StreamingResumeBufferSeconds（默认 0.32 秒，按音高换算）或者已经收齐，才继续往播放器里送
        const float ConfiguredResume=VoiceSet?VoiceSet->StreamingResumeBufferSeconds:.32f;
        const float ResumeSeconds=FMath::IsFinite(ConfiguredResume)?FMath::Clamp(ConfiguredResume,.08f,1.f):.32f;
        const float Pitch=RecordedVoice?RecordedVoice->PitchMultiplier:1.f;
        const int32 ResumeBytes=FMath::CeilToInt(Local.State.SampleRate*ResumeSeconds*Pitch)*2;
        const int32 PendingBytes=Local.Pcm.Num()-Local.QueuedBytes;
        const bool bHasWholeTail=Local.State.bAudioComplete && Local.TotalBytes>0 && Local.Pcm.Num()==Local.TotalBytes;
        const bool bCanResume=!Local.bRebuffering || PendingBytes>=ResumeBytes || bHasWholeTail;
        if (RecordedWave && PendingBytes>0 && bCanResume)
        {
            // 送进同一个 RecordedWave 接着播，不重新建播放器
            RecordedWave->QueueAudio(Local.Pcm.GetData()+Local.QueuedBytes,PendingBytes);
            UE_LOG(LogTemp,Log,TEXT("NpcLatency turn=%s stage=audio_buffer_fill role=%d bytes=%d buffered_ms=%d resumed=%d"),
                *Local.State.TurnId.ToString(),int32(GetOwner()->GetLocalRole()),PendingBytes,
                int32(RecordedWave->GetAvailableAudioByteCount()*500.0/FMath::Max(1,Local.State.SampleRate)),int32(Local.bRebuffering));
            Local.QueuedBytes=Local.Pcm.Num();
            Local.bRebuffering=false;
        }
        if ((!bGenerating || Local.State.bAudioComplete) && Local.TotalBytes>0 && GetConsumedVoiceBytes()==Local.TotalBytes)
        {
            // 声音全部读完后再等 0.15 秒（混音器里还有最后一点），然后调用 FinishLocalSentence
            if (Local.DrainedAt==0) Local.DrainedAt=Now;
            if (Now-Local.DrainedAt>=.15) FinishLocalSentence();
        }
        else Local.DrainedAt=0;
        return;
    }

    // 还没开始播：边合成的句子要攒够 StreamingStartBufferSeconds，整句攒够 0.12 秒；二狗按 DialoguePitch 调音高
    const float DefaultStart=GetDefault<UDeliveryNpcVoiceSet>()->StreamingStartBufferSeconds;
    const float ConfiguredStart=VoiceSet?VoiceSet->StreamingStartBufferSeconds:DefaultStart;
    const float StartSeconds=bGenerating?(FMath::IsFinite(ConfiguredStart)?FMath::Clamp(ConfiguredStart,.16f,2.f):DefaultStart):.12f;
    const auto* Companion=Cast<ADeliveryCompanionCharacter>(GetOwner());
    const float Pitch=Companion && FMath::IsFinite(Companion->DialoguePitch)?
        FMath::Clamp(Companion->DialoguePitch,.95f,1.08f):1.f;
    int32 InitialBuffer=FMath::CeilToInt(Local.State.SampleRate*StartSeconds*Pitch)*2;
    if (Local.TotalBytes>0) InitialBuffer=FMath::Min(InitialBuffer,Local.TotalBytes);
    if ((!bGenerating && Local.TotalBytes<=0) || Local.Pcm.Num()<InitialBuffer || ServerTime()<Local.State.StartTime) return;

    // 第一次播放时建 RecordedVoice；每句新建一个 RecordedWave
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

    // PlayWhenSilent：主机离得远听不见时播放器也照样往前读，HasFinishedSentence 才能等到这句播完
    RecordedWave->VirtualizationMode=EVirtualizationMode::PlayWhenSilent;

    // 从这句开头把已收到的声音全部送进播放器；字幕进度按 GetConsumedVoiceBytes 算
    RecordedWave->QueueAudio(Local.Pcm.GetData(),Local.Pcm.Num());
    Local.QueuedBytes=Local.Pcm.Num();
    Local.bRebuffering=false;
    DisplayedSpeech=Local.State; DisplayedSpeech.bActive=true;
    StopReaction();
    RecordedVoice->SetSound(RecordedWave); RecordedVoice->Play();
    UE_LOG(LogTemp,Log,TEXT("NpcLatency speech=%s turn=%s stage=audio_play role=%d late_ms=%d"),
        *Local.State.ReplyToEventId.ToString(),*Local.State.TurnId.ToString(),int32(GetOwner()->GetLocalRole()),int32((ServerTime()-Local.State.StartTime)*1000));
}

/// @brief 设置 RecordedVoice 的音量和衰减：bRadio 为 true 时不按距离衰减；VoiceSet 没配衰减资产时用 600 cm 内不衰减、1800 cm 外听不见
void UDeliveryNpcPresentationComponent::ConfigureRecordedVoice(bool bRadio)
{
    if (!RecordedVoice) return;
    bPlayingRadioVoice=bRadio;

    // 每句都按 VoiceSet 重新设一遍，上一句开麦播放的设置不会留到下一句
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

/// @brief 本机正在播的声音在当前位置之后 640 字节（约 20 毫秒）的响度，0 到 1；口型用它张嘴
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
