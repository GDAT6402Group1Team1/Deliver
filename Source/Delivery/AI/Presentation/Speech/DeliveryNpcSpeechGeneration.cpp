#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/Speech/DeliveryNpcVoiceCodec.h"
#include "Async/Async.h"
#include "Tasks/Task.h"
#include "Templates/Atomic.h"
#include "VoiceModule.h"

// ── 回复 ──
// 后端把一句台词的声音分成几段发来：先 BeginSentenceAudio（这句开始，带文字），再多次 AppendSentenceAudio（一段段 PCM 声音），最后 EndSentenceAudio
// 服务器每收到一段就交给后台线程压缩（Opus），压好的部分存进 GeneratingSentences；TryStartGeneratedSentence 等第一包压好就开始说这句
// RefreshGeneratedSentence 把新压好的部分接到 SendingAudio（发给客户端）、ServerVoices（留着补发）和本机的播放队列
// 当前这轮还没开始时（NPC 还在说上一轮），三个函数先把数据存进 PendingReply，等上一轮说完再处理
// 一句的声音合成或压缩失败时，这句只显示文字，后面的句子照常

/// 一句话的后台压缩任务；FDeliveryNpcVoiceStreamEncoder 只在后台任务里使用，每个任务都等上一个做完才开始
/// 游戏线程只设置 bCancelled（取消）和安排下一个任务
struct FDeliveryNpcVoiceEncodingJob
{
    /// 这句的 Opus 编码器
    FDeliveryNpcVoiceStreamEncoder Encoder;

    /// 游戏线程设为 true 后，还没开始的任务不再压缩；正在执行的那次做完后结果也被丢掉
    TAtomic<bool> bCancelled{false};

    /// 最后安排的任务；下一个任务要等它做完
    UE::Tasks::FTask LastTask;

    /// 已经压好输出了多少字节，用来核对结果拼接的位置
    int32 EncodedBytes=0;

    /// 压缩失败过，之后的任务不再压
    bool bFailed=false;
};

namespace
{
/// 一个后台任务的结果，送回游戏线程处理
struct FEncodedAudioPart
{
    /// 这次压出来的数据
    TArray<uint8> Audio;

    /// 失败原因
    FString Error;

    /// 这次数据应该接在已压好数据的第几个字节后面
    int32 Offset=0;

    /// 压缩成功
    bool bSucceeded=false;

    /// 任务做完时这句已经被取消
    bool bCancelled=false;
};

/// 后台任务和游戏线程共用这份任务数据，用线程安全的共享指针
using FEncodingJob=TSharedPtr<FDeliveryNpcVoiceEncodingJob,ESPMode::ThreadSafe>;

/// 在后台对编码器做的一步操作：Start、Append 或 Finish
using FEncodeOperation=TUniqueFunction<bool(FDeliveryNpcVoiceStreamEncoder&,TArray<uint8>&,FString&)>;

/// 回到游戏线程后处理结果的函数
using FEncodeCompletion=TUniqueFunction<void(FEncodedAudioPart)>;

/// @brief 游戏线程：安排一个后台任务执行 Operation（Start、Append 或 Finish），做完后回到游戏线程调用 Completion
/// 新任务以 LastTask 为前置任务，同一句的压缩严格按收到声音的顺序进行
void QueueEncoding(const FEncodingJob& Job,FEncodeOperation Operation,FEncodeCompletion Completion)
{
    check(IsInGameThread());
    Job->LastTask=UE::Tasks::Launch(TEXT("NpcSpeech.EncodePart"),
        [Job,Operation=MoveTemp(Operation),Completion=MoveTemp(Completion)]() mutable
        {
            FEncodedAudioPart Result;
            Result.Offset=Job->EncodedBytes;
            if (!Job->bCancelled.Load() && !Job->bFailed)
            {
                Result.bSucceeded=Operation(Job->Encoder,Result.Audio,Result.Error);
                if (Result.bSucceeded) Job->EncodedBytes+=Result.Audio.Num();
                else { Job->bFailed=true; Job->Encoder.Reset(); }
            }
            Result.bCancelled=Job->bCancelled.Load();
            if (Result.bCancelled) { Result.Audio.Reset(); Job->Encoder.Reset(); }
            AsyncTask(ENamedThreads::GameThread,
                [Job,Completion=MoveTemp(Completion),Result=MoveTemp(Result)]() mutable
                { Completion(MoveTemp(Result)); });
        },UE::Tasks::Prerequisites(Job->LastTask));
}

/// @brief 游戏线程：取消这句的压缩，设置 bCancelled，再安排一个任务在前面的任务都结束后释放编码器
void CancelEncoding(const FEncodingJob& Job)
{
    check(IsInGameThread());
    if (!Job) return;
    Job->bCancelled.Store(true);

    // 释放编码器也放进任务链：等正在压的那一包做完再释放，游戏线程不用等
    Job->LastTask=UE::Tasks::Launch(TEXT("NpcSpeech.ReleaseEncoder"),
        [Job]() { Job->Encoder.Reset(); },UE::Tasks::Prerequisites(Job->LastTask));
}

/// @brief 没有声音时这句文字显示多久：每个字 0.075 秒加 0.5 秒，限制在 1 到 10 秒
float TextDuration(const FString& Text)
{
    return FMath::Clamp(Text.Len()*.075f+.5f,1.f,10.f);
}
}

/// @brief 服务器：后端开始发一句台词的声音，Begin 带着这句的序号、文字、情绪和采样率
/// 这句属于等着的下一轮（PendingReply）时先存起来；属于当前这轮时建好压缩任务，在后台调用 Encoder.Start 输出格式头
/// 序号超过 2、采样率不是 24000、文字为空或超过 120 字、序号重复或比已经说过的小时返回 false
bool UDeliveryNpcPresentationComponent::BeginSentenceAudio(FGuid ActionId,FGuid MessageId,const FDeliveryNpcSpeechBegin& Begin)
{
    check(IsInGameThread());
    if (GetOwner() && GetOwner()->HasAuthority() && ActionId.IsValid() && ActionId==PendingReply.ReplyId && MessageId==PendingReply.MessageId)
    {
        if (Begin.Sequence<0 || Begin.Sequence>2 || Begin.SampleRate!=24000 || Begin.AudioStreamVersion!=1 ||
            Begin.Text.IsEmpty() || Begin.Text.Len()>120 || PendingReply.Begins.Contains(Begin.Sequence) ||
            PendingReply.Whole.Contains(Begin.Sequence) ||
            (PendingReply.SentenceCount!=INDEX_NONE && Begin.Sequence>=PendingReply.SentenceCount)) return false;
        PendingReply.Begins.Add(Begin.Sequence,Begin);
        return true;
    }
    if (!GetOwner() || !GetOwner()->HasAuthority() || !ActionId.IsValid() || ActionId!=ReplyAction || MessageId!=ReplyMessage ||
        Begin.Sequence<NextSentence || Begin.Sequence>2 || Begin.SampleRate!=24000 || Begin.AudioStreamVersion!=1 ||
        Begin.Text.IsEmpty() || Begin.Text.Len()>120 || GeneratingSentences.Contains(Begin.Sequence) || Sentences.Contains(Begin.Sequence) ||
        (ExpectedSentences!=INDEX_NONE && Begin.Sequence>=ExpectedSentences)) return false;

    // UE 要求语音模块在游戏线程加载，后台任务里才能用它的编码器
    FVoiceModule::Get();
    auto& Sentence=GeneratingSentences.Add(Begin.Sequence);
    Sentence.Sentence.Sequence=Begin.Sequence;
    Sentence.Sentence.Text=Begin.Text;
    Sentence.Sentence.Emotion=Begin.Emotion;
    Sentence.Sentence.SampleRate=Begin.SampleRate;
    Sentence.TurnId=FGuid::NewGuid();
    Sentence.Encoding=MakeShared<FDeliveryNpcVoiceEncodingJob,ESPMode::ThreadSafe>();
    const auto Job=Sentence.Encoding;
    const FGuid TurnId=Sentence.TurnId;
    const int32 Sequence=Begin.Sequence,SampleRate=Begin.SampleRate;
    const TWeakObjectPtr<UDeliveryNpcPresentationComponent> WeakThis(this);
    ++PendingEncodeTasks;
    QueueEncoding(Job,
        [SampleRate](auto& Encoder,auto& Audio,auto& Error) { return Encoder.Start(SampleRate,Audio,Error); },

        // 回到游戏线程：组件没了，或者回复、句子、任务已经换了（被打断后又开始了新的一句），这次结果直接丢掉
        // 压缩失败或拼接位置对不上时，这句标记失败，只显示文字
        [WeakThis,ActionId,MessageId,Sequence,TurnId,Job](FEncodedAudioPart Result)
        {
            auto* Component=WeakThis.Get();
            if (!Component) return;
            --Component->PendingEncodeTasks;
            auto* Current=Component->GeneratingSentences.Find(Sequence);
            if (Component->ReplyAction!=ActionId || Component->ReplyMessage!=MessageId || !Current ||
                Current->TurnId!=TurnId || Current->Encoding!=Job || Job->bCancelled.Load() || Result.bCancelled) return;
            if (!Result.bSucceeded || Result.Offset!=Current->Encoded.Num())
            {
                Current->bFailed=Current->bEncodingEnded=true;
                Current->Encoded.Reset(); Current->Sentence.Pcm.Reset(); CancelEncoding(Job);
                UE_LOG(LogTemp,Warning,TEXT("NpcSpeech stream_encode_failed turn=%s detail=%s"),*TurnId.ToString(),*Result.Error);
            }
            else Current->Encoded.Append(Result.Audio);
            Component->RefreshGeneratedSentence(Sequence);
        });
    return true;
}

/// @brief 服务器：收到一句台词的一段 PCM 声音
/// 段号 ChunkIndex 和起始字节 OffsetBytes 必须紧接上一段，每段最多 30720 字节，整句最多 12 秒，否则返回 false
/// 属于当前这轮时，这段接进 Sentence.Pcm 并交给后台调用 Encoder.Append
bool UDeliveryNpcPresentationComponent::AppendSentenceAudio(FGuid ActionId,FGuid MessageId,const FDeliveryNpcAudioChunk& Chunk)
{
    check(IsInGameThread());
    if (GetOwner() && GetOwner()->HasAuthority() && ActionId.IsValid() && ActionId==PendingReply.ReplyId && MessageId==PendingReply.MessageId)
    {
        if (!PendingReply.Begins.Contains(Chunk.Sequence) || PendingReply.Ends.Contains(Chunk.Sequence)) return false;
        auto& Parts=PendingReply.Chunks.FindOrAdd(Chunk.Sequence);
        const int32 Offset=Parts.IsEmpty()?0:Parts.Last().OffsetBytes+Parts.Last().Pcm.Num();
        if (Parts.Num()>=DeliveryNpcVoice::MaxAudioChunks || Chunk.ChunkIndex!=Parts.Num() || Chunk.OffsetBytes!=Offset || Chunk.Pcm.IsEmpty() ||
            Chunk.Pcm.Num()%2 || Chunk.Pcm.Num()>30720 || Chunk.Pcm.Num()>DeliveryNpcVoice::MaxPcmBytes-Offset) return false;
        Parts.Add(Chunk);
        return true;
    }
    if (!GetOwner() || !GetOwner()->HasAuthority() || ActionId!=ReplyAction || MessageId!=ReplyMessage) return false;
    auto* Current=GeneratingSentences.Find(Chunk.Sequence);
    if (!Current || Current->bInputEnded || Chunk.ChunkIndex!=Current->NextChunk || Current->NextChunk>=DeliveryNpcVoice::MaxAudioChunks ||
        Chunk.OffsetBytes!=Current->ReceivedPcmBytes || Chunk.Pcm.IsEmpty() || Chunk.Pcm.Num()>30720 || Chunk.Pcm.Num()%2 ||
        Chunk.Pcm.Num()>DeliveryNpcVoice::MaxPcmBytes-Current->ReceivedPcmBytes) return false;
    ++Current->NextChunk;
    Current->ReceivedPcmBytes+=Chunk.Pcm.Num();

    // 这句已经压缩失败：仍然按上面的规则检查段号，但不再压缩；这句只显示文字，后面的句子不受影响
    if (Current->bFailed) return true;
    Current->Sentence.Pcm.Append(Chunk.Pcm);
    const auto Job=Current->Encoding;
    const FGuid TurnId=Current->TurnId;
    const int32 Sequence=Chunk.Sequence;
    const TWeakObjectPtr<UDeliveryNpcPresentationComponent> WeakThis(this);
    ++PendingEncodeTasks;
    QueueEncoding(Job,
        [Pcm=Chunk.Pcm](auto& Encoder,auto& Audio,auto& Error) { return Encoder.Append(Pcm,Audio,Error); },

        // 回到游戏线程：结果过期就丢掉；失败、拼接位置不对或压缩后超过 MaxEncodedBytes 时这句标记失败
        [WeakThis,ActionId,MessageId,Sequence,TurnId,Job](FEncodedAudioPart Result)
        {
            auto* Component=WeakThis.Get();
            if (!Component) return;
            --Component->PendingEncodeTasks;
            auto* Sentence=Component->GeneratingSentences.Find(Sequence);
            if (Component->ReplyAction!=ActionId || Component->ReplyMessage!=MessageId || !Sentence ||
                Sentence->TurnId!=TurnId || Sentence->Encoding!=Job || Job->bCancelled.Load() || Result.bCancelled) return;
            if (!Result.bSucceeded || Result.Offset!=Sentence->Encoded.Num() ||
                Result.Audio.Num()>DeliveryNpcVoice::MaxEncodedBytes-Sentence->Encoded.Num())
            {
                Sentence->bFailed=Sentence->bEncodingEnded=true;
                Sentence->Encoded.Reset(); Sentence->Sentence.Pcm.Reset(); CancelEncoding(Job);
                UE_LOG(LogTemp,Warning,TEXT("NpcSpeech stream_encode_failed turn=%s detail=%s"),*TurnId.ToString(),*Result.Error);
            }
            else Sentence->Encoded.Append(Result.Audio);
            Component->RefreshGeneratedSentence(Sequence);
        });
    RefreshGeneratedSentence(Sequence);
    return true;
}

/// @brief 服务器：后端说这句的声音发完了，End 带着总段数、总字节数和合成是否成功
/// 总段数和总字节数要和收到的一致；成功时交给后台调用 Encoder.Finish 补上句尾、写结束包；失败时这句只显示文字
bool UDeliveryNpcPresentationComponent::EndSentenceAudio(FGuid ActionId,FGuid MessageId,const FDeliveryNpcAudioEnd& End)
{
    check(IsInGameThread());
    if (GetOwner() && GetOwner()->HasAuthority() && ActionId.IsValid() && ActionId==PendingReply.ReplyId && MessageId==PendingReply.MessageId)
    {
        if (!PendingReply.Begins.Contains(End.Sequence) || PendingReply.Ends.Contains(End.Sequence)) return false;
        const auto* Parts=PendingReply.Chunks.Find(End.Sequence);
        const int32 Count=Parts?Parts->Num():0;
        const int32 Bytes=Count>0?Parts->Last().OffsetBytes+Parts->Last().Pcm.Num():0;
        if (End.ChunkCount!=Count || End.TotalPcmBytes!=Bytes || (End.bSucceeded && (Count==0 || Bytes==0))) return false;
        PendingReply.Ends.Add(End.Sequence,End);
        return true;
    }
    if (!GetOwner() || !GetOwner()->HasAuthority() || ActionId!=ReplyAction || MessageId!=ReplyMessage) return false;
    auto* Current=GeneratingSentences.Find(End.Sequence);
    if (!Current || Current->bInputEnded || End.ChunkCount!=Current->NextChunk || End.TotalPcmBytes!=Current->ReceivedPcmBytes ||
        (End.bSucceeded && (End.ChunkCount<=0 || End.TotalPcmBytes<=0))) return false;
    Current->bInputEnded=true;
    const auto Job=Current->Encoding;
    const FGuid TurnId=Current->TurnId;
    const int32 Sequence=End.Sequence;
    if (!End.bSucceeded || Current->bFailed)
    {
        Current->bFailed=Current->bEncodingEnded=true;
        Current->Encoded.Reset(); Current->Sentence.Pcm.Reset(); CancelEncoding(Job);

        // 这句已经开始说过（序号小于 NextSentence）而且现在没在说它：文字可能已经显示完了，直接移除这句
        if (Sequence<NextSentence && (!Speech.bActive || Speech.TurnId!=TurnId))
        {
            GeneratingSentences.Remove(Sequence);
            return true;
        }
        RefreshGeneratedSentence(Sequence);
        return true;
    }
    const TWeakObjectPtr<UDeliveryNpcPresentationComponent> WeakThis(this);
    ++PendingEncodeTasks;
    QueueEncoding(Job,
        [](auto& Encoder,auto& Audio,auto& Error) { return Encoder.Finish(Audio,Error); },

        // 回到游戏线程：结果过期就丢掉；设置 bEncodingEnded（这句压完了），失败时标记失败
        [WeakThis,ActionId,MessageId,Sequence,TurnId,Job](FEncodedAudioPart Result)
        {
            auto* Component=WeakThis.Get();
            if (!Component) return;
            --Component->PendingEncodeTasks;
            auto* Sentence=Component->GeneratingSentences.Find(Sequence);
            if (Component->ReplyAction!=ActionId || Component->ReplyMessage!=MessageId || !Sentence ||
                Sentence->TurnId!=TurnId || Sentence->Encoding!=Job || Job->bCancelled.Load() || Result.bCancelled) return;
            Sentence->bEncodingEnded=true;
            if (!Result.bSucceeded || Result.Offset!=Sentence->Encoded.Num() ||
                Result.Audio.Num()>DeliveryNpcVoice::MaxEncodedBytes-Sentence->Encoded.Num())
            {
                Sentence->bFailed=true;
                Sentence->Encoded.Reset(); Sentence->Sentence.Pcm.Reset(); CancelEncoding(Job);
                UE_LOG(LogTemp,Warning,TEXT("NpcSpeech stream_encode_failed turn=%s detail=%s"),*TurnId.ToString(),*Result.Error);
            }
            else Sentence->Encoded.Append(Result.Audio);
            Component->RefreshGeneratedSentence(Sequence);
        });
    return true;
}

/// @brief 取消所有句子的后台压缩，清空 GeneratingSentences
void UDeliveryNpcPresentationComponent::CancelSentenceGeneration()
{
    check(IsInGameThread());
    for (auto& Entry:GeneratingSentences) CancelEncoding(Entry.Value.Encoding);
    GeneratingSentences.Reset();
}

/// @brief 服务器：下一句（NextSentence）已经有压好的数据或已经失败时，开始说这句
/// 写好 Speech（这句的文字、情绪、编号、0.1 秒后开始），同步给客户端，并调用 StartStreamSpeech 开始分段发送
/// 正在说别的句子、或者下一句还没压出任何数据时返回 false
bool UDeliveryNpcPresentationComponent::TryStartGeneratedSentence()
{
    if (!GetOwner()->HasAuthority() || !ReplyAction.IsValid() || Speech.bActive) return false;
    auto* Current=GeneratingSentences.Find(NextSentence);
    if (!Current || (!Current->bFailed && Current->Encoded.IsEmpty())) return false;
    Speech={}; Speech.TurnId=Current->TurnId; Speech.ActionId=ReplyAction; Speech.ReplyToEventId=ReplyMessage;
    Speech.RadioListener=ResolveRadioListener(ReplyMessage);
    Speech.Sequence=NextSentence++; Speech.Text=Current->Sentence.Text; Speech.Emotion=Current->Sentence.Emotion;
    Speech.SampleRate=Current->Sentence.SampleRate; Speech.AudioFormatVersion=DeliveryNpcVoice::StreamingFormatVersion;
    Speech.bRecordedVoice=!Current->bFailed; Speech.bActive=true;
    Speech.Duration=TextDuration(Speech.Text); Speech.StartTime=ServerTime()+.1;
    SendingAudio.Reset(); SendingOffset=0;
    GetOwner()->ForceNetUpdate(); OnRep_Speech(); StartStreamSpeech(Speech);
    RefreshGeneratedSentence(Speech.Sequence);
    UE_LOG(LogTemp,Log,TEXT("NpcLatency speech=%s turn=%s stage=stream_scheduled sequence=%d"),
        *ReplyMessage.ToString(),*Speech.TurnId.ToString(),Speech.Sequence);
    return true;
}

/// @brief 服务器：把这句新压好的数据接到三处：SendingAudio（正在发给客户端）、ServerVoices（留着给后来的玩家补发）、本机播放队列
/// 只处理正在说的那句；失败时清掉已发的数据，改成只显示文字
/// 压完时写上整句的长度，按真实声音长度更新 Speech.Duration，并调用 FinishStreamAudio 通知客户端这句的总长
void UDeliveryNpcPresentationComponent::RefreshGeneratedSentence(int32 Sequence)
{
    auto* Current=GeneratingSentences.Find(Sequence);
    if (!Current || !Speech.bActive || Speech.TurnId!=Current->TurnId || Speech.ActionId!=ReplyAction) return;

    // 这句刚压完、还没通知过客户端，函数末尾调用 FinishStreamAudio
    const bool bNotifyComplete=Current->bEncodingEnded && !Speech.bAudioComplete;
    auto* Local=LocalVoices.FindByPredicate([&](const auto& Value) { return Value.State.TurnId==Current->TurnId; });
    if (Current->bFailed)
    {
        SendingAudio.Reset(); SendingOffset=0; ServerVoices.Remove(Current->TurnId);
        Speech.bRecordedVoice=false; Speech.bAudioComplete=true;
        Speech.PcmBytes=Speech.EncodedBytes=0; Speech.Duration=TextDuration(Speech.Text);
        if (Local) UseTextForLocalVoice(*Local);
    }
    else
    {
        if (Current->Encoded.Num()>SendingAudio.Num())
            SendingAudio.Append(Current->Encoded.GetData()+SendingAudio.Num(),Current->Encoded.Num()-SendingAudio.Num());
        auto& Saved=ServerVoices.FindOrAdd(Current->TurnId);
        Saved.RadioListener=Speech.RadioListener;
        Saved.bComplete=Current->bEncodingEnded;
        if (Current->Sentence.Pcm.Num()>Saved.Pcm.Num())
            Saved.Pcm.Append(Current->Sentence.Pcm.GetData()+Saved.Pcm.Num(),Current->Sentence.Pcm.Num()-Saved.Pcm.Num());
        if (Current->Encoded.Num()>Saved.Encoded.Num())
            Saved.Encoded.Append(Current->Encoded.GetData()+Saved.Encoded.Num(),Current->Encoded.Num()-Saved.Encoded.Num());
        if (Local && Current->Sentence.Pcm.Num()>Local->Pcm.Num())
        {
            Local->Pcm.Append(Current->Sentence.Pcm.GetData()+Local->Pcm.Num(),Current->Sentence.Pcm.Num()-Local->Pcm.Num());
            Local->LastDataAt=FPlatformTime::Seconds();
        }
        if (Current->bEncodingEnded)
        {
            Speech.bAudioComplete=true; Speech.PcmBytes=Current->ReceivedPcmBytes; Speech.EncodedBytes=Current->Encoded.Num();
            const int32 PlaybackBytes=DeliveryNpcVoice::GetDecodedPcmBytes(Speech.PcmBytes,Speech.SampleRate);
            Speech.Duration=float(PlaybackBytes)/(2*Speech.SampleRate);
            if (Local)
            {
                // 整句长度变了，字幕的进度算法也变；先记下已经显示的进度，免得字幕往回跳
                if (DisplayedSpeech.bActive && DisplayedSpeech.TurnId==Current->TurnId)
                    Local->CaptionProgress=GetDisplayedSpeechProgress();

                // 服务器本机直接播放原始声音，不经过压缩；句尾补上和压缩时同样的 0.04 秒无声数据，两边长度一致
                if (Local->Pcm.Num()<PlaybackBytes) Local->Pcm.AddZeroed(PlaybackBytes-Local->Pcm.Num());
                Local->TotalBytes=PlaybackBytes; Local->State=Speech;
                Local->LastDataAt=FPlatformTime::Seconds();
            }
        }
    }
    if (bNotifyComplete)
    {
        GetOwner()->ForceNetUpdate();
        FinishStreamAudio(Speech.TurnId,Speech.PcmBytes,Speech.EncodedBytes,!Current->bFailed);
    }
}
