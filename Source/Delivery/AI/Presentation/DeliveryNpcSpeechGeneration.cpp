#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/DeliveryNpcVoiceCodec.h"
#include "Async/Async.h"
#include "Tasks/Task.h"
#include "Templates/Atomic.h"
#include "VoiceModule.h"

// 编码器只由同一条后台任务链访问，游戏线程只设置取消标记和下一任务的依赖
struct FDeliveryNpcVoiceEncodingJob
{
    FDeliveryNpcVoiceStreamEncoder Encoder;
    TAtomic<bool> bCancelled{false};
    UE::Tasks::FTask LastTask;
    int32 EncodedBytes=0;
    bool bFailed=false;
};

namespace
{
struct FEncodedAudioPart
{
    TArray<uint8> Audio;
    FString Error;
    int32 Offset=0;
    bool bSucceeded=false;
    bool bCancelled=false;
};

using FEncodingJob=TSharedPtr<FDeliveryNpcVoiceEncodingJob,ESPMode::ThreadSafe>;
using FEncodeOperation=TUniqueFunction<bool(FDeliveryNpcVoiceStreamEncoder&,TArray<uint8>&,FString&)>;
using FEncodeCompletion=TUniqueFunction<void(FEncodedAudioPart)>;

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

void CancelEncoding(const FEncodingJob& Job)
{
    check(IsInGameThread());
    if (!Job) return;
    Job->bCancelled.Store(true);
    // 先让正在执行的 Append 退出，再释放编码器，取消不会阻塞游戏线程
    Job->LastTask=UE::Tasks::Launch(TEXT("NpcSpeech.ReleaseEncoder"),
        [Job]() { Job->Encoder.Reset(); },UE::Tasks::Prerequisites(Job->LastTask));
}

float TextDuration(const FString& Text)
{
    return FMath::Clamp(Text.Len()*.075f+.5f,1.f,10.f);
}
}

bool UDeliveryNpcPresentationComponent::BeginSentenceAudio(FGuid ActionId,FGuid MessageId,const FDeliveryNpcSpeechBegin& Begin)
{
    check(IsInGameThread());
    if (!GetOwner() || !GetOwner()->HasAuthority() || !ActionId.IsValid() || ActionId!=ReplyAction || MessageId!=ReplyMessage ||
        Begin.Sequence<NextSentence || Begin.Sequence>2 || Begin.SampleRate!=24000 || Begin.AudioStreamVersion!=1 ||
        Begin.Text.IsEmpty() || Begin.Text.Len()>120 || GeneratingSentences.Contains(Begin.Sequence) || Sentences.Contains(Begin.Sequence) ||
        (ExpectedSentences!=INDEX_NONE && Begin.Sequence>=ExpectedSentences)) return false;

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

bool UDeliveryNpcPresentationComponent::AppendSentenceAudio(FGuid ActionId,FGuid MessageId,const FDeliveryNpcAudioChunk& Chunk)
{
    check(IsInGameThread());
    if (!GetOwner() || !GetOwner()->HasAuthority() || ActionId!=ReplyAction || MessageId!=ReplyMessage) return false;
    auto* Current=GeneratingSentences.Find(Chunk.Sequence);
    if (!Current || Current->bInputEnded || Chunk.ChunkIndex!=Current->NextChunk || Current->NextChunk>=32 ||
        Chunk.OffsetBytes!=Current->ReceivedPcmBytes || Chunk.Pcm.IsEmpty() || Chunk.Pcm.Num()>30720 || Chunk.Pcm.Num()%2 ||
        Chunk.Pcm.Num()>DeliveryNpcVoice::MaxPcmBytes-Current->ReceivedPcmBytes) return false;
    ++Current->NextChunk;
    Current->ReceivedPcmBytes+=Chunk.Pcm.Num();
    // 编码失败后仍检查后台剩余数据的顺序，这一句继续使用文字，后一句仍可播放
    if (Current->bFailed) return true;
    Current->Sentence.Pcm.Append(Chunk.Pcm);
    const auto Job=Current->Encoding;
    const FGuid TurnId=Current->TurnId;
    const int32 Sequence=Chunk.Sequence;
    const TWeakObjectPtr<UDeliveryNpcPresentationComponent> WeakThis(this);
    ++PendingEncodeTasks;
    QueueEncoding(Job,
        [Pcm=Chunk.Pcm](auto& Encoder,auto& Audio,auto& Error) { return Encoder.Append(Pcm,Audio,Error); },
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

bool UDeliveryNpcPresentationComponent::EndSentenceAudio(FGuid ActionId,FGuid MessageId,const FDeliveryNpcAudioEnd& End)
{
    check(IsInGameThread());
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
        // 文字可能已经播完，收齐这句剩余通知后再移除，避免影响后面的台词
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

void UDeliveryNpcPresentationComponent::CancelSentenceGeneration()
{
    check(IsInGameThread());
    for (auto& Entry:GeneratingSentences) CancelEncoding(Entry.Value.Encoding);
    GeneratingSentences.Reset();
}

bool UDeliveryNpcPresentationComponent::TryStartGeneratedSentence()
{
    if (!GetOwner()->HasAuthority() || !ReplyAction.IsValid() || Speech.bActive) return false;
    auto* Current=GeneratingSentences.Find(NextSentence);
    if (!Current || (!Current->bFailed && Current->Encoded.IsEmpty())) return false;
    Speech={}; Speech.TurnId=Current->TurnId; Speech.ActionId=ReplyAction; Speech.ReplyToEventId=ReplyMessage;
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

void UDeliveryNpcPresentationComponent::RefreshGeneratedSentence(int32 Sequence)
{
    auto* Current=GeneratingSentences.Find(Sequence);
    if (!Current || !Speech.bActive || Speech.TurnId!=Current->TurnId || Speech.ActionId!=ReplyAction) return;
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
                // 先记住已显示的字数，再用整句总长计算后续进度
                if (DisplayedSpeech.bActive && DisplayedSpeech.TurnId==Current->TurnId)
                    Local->CaptionProgress=GetDisplayedSpeechProgress();
                // 主机直接播放原声音，句末只补一次编码器同样使用的 40 ms
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
