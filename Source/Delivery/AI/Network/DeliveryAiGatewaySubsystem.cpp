#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "Containers/Queue.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Base64.h"

// 网络线程接收回复内容，游戏线程再将内容交给对应 NPC
struct FDeliveryNpcStreamBuffer
{
    TArray<uint8> Bytes;
    TQueue<FString, EQueueMode::Mpsc> Lines;
    int64 Total = 0;
};
#include "AI/Network/DeliveryAiSettings.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Engine/World.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

namespace
{
    bool ReadInteger(const FJsonObject& Json, const TCHAR* Key, int32 Minimum, int32 Maximum, int32& Out)
    {
        double Value = 0;
        if (!Json.TryGetNumberField(Key, Value) || !FMath::IsFinite(Value) ||
            Value < Minimum || Value > Maximum || FMath::FloorToDouble(Value) != Value) return false;
        Out = static_cast<int32>(Value);
        return true;
    }

    bool ReadSpeechText(const FJsonObject& Json, FString& Text, FName& Emotion)
    {
        FString Value;
        if (!Json.TryGetStringField(TEXT("text"), Text) || Text.Len() > 120 || Text.TrimStartAndEnd().IsEmpty() ||
            !Json.TryGetStringField(TEXT("emotion"), Value) ||
            (Value != TEXT("Calm") && Value != TEXT("Angry") && Value != TEXT("Surprised"))) return false;
        Emotion = FName(Value);
        return true;
    }
}

bool UDeliveryAiGatewaySubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UDeliveryAiGatewaySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    WorldEpoch = FGuid::NewGuid();
    bShuttingDown = false;
}

bool UDeliveryAiGatewaySubsystem::Enqueue(UDeliveryNpcAgentComponent* Agent, const FDeliveryNpcDecisionRequest& Request)
{
    const auto* Settings = GetDefault<UDeliveryAiSettings>();
    if (bShuttingDown || !Settings->bEnabled || GetWorld()->GetNetMode() == NM_Client || !IsValid(Agent) ||
        Agent->GetWorld() != GetWorld() || !Agent->GetOwner()->HasAuthority() || Request.WorldEpoch != WorldEpoch ||
        Request.NpcId != Agent->GetNpcId() || !Request.RequestId.IsValid() || Pending.Contains(Request.RequestId) ||
        Pending.Num() >= FMath::Clamp(Settings->MaxPendingRequests, 1, 64)) return false;
    for (const auto& Pair : Pending) if (Pair.Value.Agent == Agent) return false;
    FPending Entry;
    Entry.Agent = Agent;
    Entry.Request = Request;
    Entry.Deadline = FPlatformTime::Seconds() + FMath::Clamp(Settings->RequestTimeout, .5f, 30.f);
    Entry.Endpoint = Settings->DecisionEndpoint;
    Pending.Add(Request.RequestId, MoveTemp(Entry));
    if (Request.bPlayerRequest)
    {
        int32 Index = 0;
        while (Index < Queue.Num() && Pending[Queue[Index]].Request.bPlayerRequest) ++Index;
        Queue.Insert(Request.RequestId, Index);
    }
    else Queue.Add(Request.RequestId);
    return true;
}

void UDeliveryAiGatewaySubsystem::Remove(FGuid RequestId)
{
    FPending Entry;
    if (!Pending.RemoveAndCopyValue(RequestId, Entry)) return;
    Queue.Remove(RequestId);
    if (Entry.Http)
    {
        if (Entry.Stream && !Entry.bReplyFinished)
        {
            auto CancelHttp = FHttpModule::Get().CreateRequest();
            CancelHttp->SetURL(Entry.Endpoint + TEXT("/cancel"));
            CancelHttp->SetVerb(TEXT("POST"));
            CancelHttp->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
            CancelHttp->SetContentAsString(DeliveryNpc::EncodeRequest(Entry.Request));
            CancelHttp->SetTimeout(2);
            CancelHttp->ProcessRequest();
        }
        // 先删除请求记录并解除完成通知，再取消 HTTP，避免取消时又处理一次回复
        Entry.Http->OnProcessRequestComplete().Unbind();
        Entry.Http->CancelRequest();
    }
}

void UDeliveryAiGatewaySubsystem::Cancel(UDeliveryNpcAgentComponent* Agent)
{
    TArray<FGuid> Ids;
    for (const auto& Pair : Pending) if (Pair.Value.Agent == Agent) Ids.Add(Pair.Key);
    for (const auto& Id : Ids) Remove(Id);
}

void UDeliveryAiGatewaySubsystem::Complete(FGuid RequestId, const FString& Body, FName Error)
{
    check(IsInGameThread());
    const FPending* Entry = Pending.Find(RequestId);
    if (bShuttingDown || !Entry) return;
    auto Agent = Entry->Agent;
    const bool bStream = Entry->Stream.IsValid();
    if (FPlatformTime::Seconds() >= Entry->Deadline) Error = TEXT("RequestTimeout");
    FDeliveryNpcDecisionProposal Proposal;
    const bool bDecoded = Error.IsNone() && DeliveryNpc::DecodeProposal(Body, Proposal, Error);
    // 先减少等待中的请求数，再通知 Agent，给下一次请求留出位置
    Remove(RequestId);
    if (bStream)
    {
        if (Agent.IsValid()) Agent->FinishReply(RequestId, false);
        return;
    }
    if (Agent.IsValid() && Agent->GetWorld() == GetWorld())
        Agent->ReceiveDecision(RequestId, bDecoded ? &Proposal : nullptr, Error);
}

void UDeliveryAiGatewaySubsystem::Dispatch(FGuid RequestId)
{
    FPending* Entry = Pending.Find(RequestId);
    if (!Entry || Entry->Http) return;
    auto Http = FHttpModule::Get().CreateRequest();
    Entry->Http = Http;
    const bool bStream = GetDefault<UDeliveryAiSettings>()->bStreamReplies;
    Http->SetURL(Entry->Endpoint + (bStream ? TEXT("/stream") : TEXT("")));
    Http->SetVerb(TEXT("POST"));
    Http->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Http->SetContentAsString(DeliveryNpc::EncodeRequest(Entry->Request));
    Http->SetTimeout(static_cast<float>(FMath::Max(.1, Entry->Deadline - FPlatformTime::Seconds())));
    Http->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
    const TWeakObjectPtr<UDeliveryAiGatewaySubsystem> WeakThis(this);
    if (bStream)
    {
        Entry->Stream = MakeShared<FDeliveryNpcStreamBuffer, ESPMode::ThreadSafe>();
        auto Buffer = Entry->Stream;
        Http->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda([Buffer](void* Data, int64& Size)
        {
            if (Size < 0 || Buffer->Total + Size > 2 * 1024 * 1024) { Size = 0; return; }
            Buffer->Total += Size;
            Buffer->Bytes.Append(static_cast<uint8*>(Data), static_cast<int32>(Size));
            int32 Start = 0;
            for (int32 Index = 0; Index < Buffer->Bytes.Num(); ++Index)
                if (Buffer->Bytes[Index] == '\n')
                {
                    FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Buffer->Bytes.GetData() + Start), Index - Start);
                    Buffer->Lines.Enqueue(FString(Text.Length(), Text.Get()));
                    Start = Index + 1;
                }
            if (Start) Buffer->Bytes.RemoveAt(0, Start, EAllowShrinking::No);
            if (Buffer->Bytes.Num() > 600000) Size = 0;
        }));
    }
    UE_LOG(LogTemp, Log, TEXT("NpcLatency request=%s stage=http_start player=%d"), *RequestId.ToString(), Entry->Request.bPlayerRequest);
    Http->OnProcessRequestComplete().BindLambda([WeakThis, RequestId, bStream](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
    {
        auto* Self = WeakThis.Get();
        if (!Self) return;
        if (bStream)
        {
            if (auto* PendingReply = Self->Pending.Find(RequestId))
            {
                PendingReply->bHttpFinished = true;
                PendingReply->bHttpSucceeded = bSucceeded && Response && Response->GetResponseCode() == 200;
            }
            return;
        }
        if (!bSucceeded || !Response || Response->GetResponseCode() != 200)
            Self->Complete(RequestId, {}, TEXT("TransportFailure"));
        else if (Response->GetContent().Num() > 65536)
            Self->Complete(RequestId, {}, TEXT("ResponseTooLarge"));
        else Self->Complete(RequestId, Response->GetContentAsString(), NAME_None);
    });
    if (!Http->ProcessRequest()) Complete(RequestId, {}, TEXT("TransportFailure"));
}

void UDeliveryAiGatewaySubsystem::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    if (bShuttingDown || GetWorld()->GetNetMode() == NM_Client) return;
    TArray<FGuid> StreamingIds;
    for (const auto& Pair : Pending) if (Pair.Value.Stream) StreamingIds.Add(Pair.Key);
    for (const FGuid& Id : StreamingIds)
    {
        auto* Entry = Pending.Find(Id);
        if (!Entry) continue;
        auto Buffer = Entry->Stream;
        FString Line;
        while (Pending.Contains(Id) && Buffer->Lines.Dequeue(Line)) ReadStream(Id, Line);
        Entry = Pending.Find(Id);
        if (Entry && Entry->bHttpFinished) FinishStreamRequest(Id);
    }
    TArray<FGuid> Expired;
    const double Now = FPlatformTime::Seconds();
    for (const auto& Pair : Pending)
        if (!Pair.Value.Agent.IsValid() || Now >= Pair.Value.Deadline) Expired.Add(Pair.Key);
    for (const auto& Id : Expired) Complete(Id, {}, TEXT("RequestTimeout"));
    int32 InFlight = 0;
    int32 AmbientInFlight = 0;
    for (const auto& Pair : Pending)
        if (Pair.Value.Http)
        {
            ++InFlight;
            if (!Pair.Value.Request.bPlayerRequest) ++AmbientInFlight;
        }
    const int32 Limit = FMath::Clamp(GetDefault<UDeliveryAiSettings>()->MaxConcurrentRequests, 1, 8);
    // 使用请求列表的副本逐个发送，发送失败时可以安全移除原列表中的请求
    const TArray<FGuid> Waiting = Queue;
    bool bPlayerWaiting=false;
    for (const auto& Pair:Pending) bPlayerWaiting|=Pair.Value.Request.bPlayerRequest;
    for (const auto& Id : Waiting)
    {
        if (InFlight >= Limit) break;
        const FPending* Entry = Pending.Find(Id);
        if (!Entry || Entry->Http) continue;
        // 优先处理玩家发言，新的 NPC 闲聊稍后再发送
        const bool bAmbient=!Entry->Request.bPlayerRequest;
        if (bAmbient && (bPlayerWaiting || AmbientInFlight>=1)) continue;
        Dispatch(Id);
        if (Pending.Contains(Id)) { ++InFlight; if (bAmbient) ++AmbientInFlight; }
    }
}

void UDeliveryAiGatewaySubsystem::FinishStreamRequest(FGuid RequestId)
{
    const auto* Entry = Pending.Find(RequestId);
    if (!Entry || !Entry->bHttpFinished) return;
    const bool bSucceeded = Entry->bHttpSucceeded && Entry->bReplyFinished && FPlatformTime::Seconds() < Entry->Deadline;
    const int32 SentenceCount = Entry->SentenceCount;
    const auto Agent = Entry->Agent;
    Remove(RequestId);
    if (Agent.IsValid()) Agent->FinishReply(RequestId, bSucceeded, SentenceCount);
}

void UDeliveryAiGatewaySubsystem::RejectStream(FGuid RequestId, const TCHAR* Reason)
{
    const auto* Entry = Pending.Find(RequestId);
    if (!Entry) return;
    const auto Agent = Entry->Agent;
    UE_LOG(LogTemp, Log, TEXT("NpcSpeech stream_rejected request=%s reason=%s"), *RequestId.ToString(), Reason);
    Remove(RequestId);
    if (Agent.IsValid()) Agent->FinishReply(RequestId, false);
}

void UDeliveryAiGatewaySubsystem::ReadStream(FGuid RequestId, const FString& Line)
{
    auto* Entry = Pending.Find(RequestId);
    if (!Entry || !Entry->Agent.IsValid()) return;
    if (FPlatformTime::Seconds() >= Entry->Deadline) { RejectStream(RequestId, TEXT("RequestTimeout")); return; }
    auto* Agent = Entry->Agent.Get();
    TSharedPtr<FJsonObject> Json;
    FString Type;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Json) || !Json.IsValid() || !Json->TryGetStringField(TEXT("type"), Type))
    { RejectStream(RequestId, TEXT("InvalidJson")); return; }
    if (Type == TEXT("keepalive")) return;
    FString Id;
    if (!Json->TryGetStringField(TEXT("request_id"), Id) || Id != RequestId.ToString(EGuidFormats::DigitsWithHyphensLower)) return;
    if (Type == TEXT("error")) { RejectStream(RequestId, TEXT("BackendError")); return; }
    if (Entry->bReplyFinished) { RejectStream(RequestId, TEXT("EventAfterDone")); return; }
    if (Type == TEXT("proposal"))
    {
        if (Entry->bHasProposal) { RejectStream(RequestId, TEXT("DuplicateProposal")); return; }
        const TSharedPtr<FJsonObject>* Object;
        if (!Json->TryGetObjectField(TEXT("proposal"), Object)) { RejectStream(RequestId, TEXT("InvalidProposal")); return; }
        FString Body; FJsonSerializer::Serialize(Object->ToSharedRef(), TJsonWriterFactory<>::Create(&Body));
        FDeliveryNpcDecisionProposal Proposal; FName Reason;
        if (!DeliveryNpc::DecodeProposal(Body, Proposal, Reason) || !Agent->ReceiveStreamDecision(RequestId, Proposal))
        { RejectStream(RequestId, TEXT("RejectedProposal")); return; }
        Entry = Pending.Find(RequestId);
        if (Entry) Entry->bHasProposal = true;
        return;
    }
    if (!Entry->bHasProposal) { RejectStream(RequestId, TEXT("MissingProposal")); return; }
    if (Type == TEXT("done"))
    {
        int32 Count = 0;
        if (!ReadInteger(*Json, TEXT("sentences"), 0, 3, Count))
        { RejectStream(RequestId, TEXT("InvalidSentenceCount")); return; }
        for (int32 Index = 0; Index < 3; ++Index)
        {
            const auto& Sentence = Entry->Sentences[Index];
            if ((Index < Count && !Sentence.IsFinished()) ||
                (Index >= Count && Sentence.State != ESentenceReception::Empty))
            { RejectStream(RequestId, TEXT("IncompleteSentences")); return; }
        }
        Entry->SentenceCount = Count;
        Entry->bReplyFinished = true;
        return;
    }
    int32 Sequence = 0;
    if (!ReadInteger(*Json, TEXT("sequence"), 0, 2, Sequence))
    { RejectStream(RequestId, TEXT("InvalidSequence")); return; }
    auto& Receipt = Entry->Sentences[Sequence];
    if (Type == TEXT("sentence"))
    {
        FDeliveryNpcSentence Sentence; FString Pcm;
        Sentence.Sequence = Sequence;
        if (Receipt.State != ESentenceReception::Empty ||
            !ReadSpeechText(*Json, Sentence.Text, Sentence.Emotion) || !Json->TryGetStringField(TEXT("pcm"), Pcm) ||
            !ReadInteger(*Json, TEXT("sample_rate"), 16000, 24000, Sentence.SampleRate) ||
            (Sentence.SampleRate != 16000 && Sentence.SampleRate != 24000) || Pcm.Len() > 512000 ||
            (!Pcm.IsEmpty() && !FBase64::Decode(Pcm, Sentence.Pcm)) || Sentence.Pcm.Num() % 2 ||
            Sentence.Pcm.Num() > 384000)
        { RejectStream(RequestId, TEXT("InvalidWholeSentence")); return; }
        Receipt.State = ESentenceReception::Whole;
        Receipt.PcmBytes = Sentence.Pcm.Num();
        Agent->ReceiveSentence(RequestId, Sentence);
        return;
    }
    // 新传输只接收本次请求明确协商过的分段，旧后端返回完整句子仍可使用
    if (!Entry->Request.bStreamSentenceAudio) { RejectStream(RequestId, TEXT("AudioStreamNotRequested")); return; }
    if (Type == TEXT("speech_begin"))
    {
        FDeliveryNpcSpeechBegin Begin;
        Begin.Sequence = Sequence;
        if (Receipt.State != ESentenceReception::Empty || !ReadSpeechText(*Json, Begin.Text, Begin.Emotion) ||
            !ReadInteger(*Json, TEXT("sample_rate"), 24000, 24000, Begin.SampleRate) ||
            !ReadInteger(*Json, TEXT("audio_stream_version"), 1, 1, Begin.AudioStreamVersion))
        { RejectStream(RequestId, TEXT("InvalidSpeechBegin")); return; }
        if (!Agent->ReceiveSpeechBegin(RequestId, Begin)) { RejectStream(RequestId, TEXT("SpeechBeginRejected")); return; }
        if (auto* Current = Pending.Find(RequestId)) Current->Sentences[Sequence].State = ESentenceReception::Streaming;
        return;
    }
    if (Type == TEXT("audio_chunk"))
    {
        FDeliveryNpcAudioChunk Chunk;
        FString Pcm;
        Chunk.Sequence = Sequence;
        if (Receipt.State != ESentenceReception::Streaming ||
            !ReadInteger(*Json, TEXT("chunk_index"), 0, 31, Chunk.ChunkIndex) || Chunk.ChunkIndex != Receipt.NextChunkIndex ||
            !ReadInteger(*Json, TEXT("offset_bytes"), 0, 384000, Chunk.OffsetBytes) || Chunk.OffsetBytes != Receipt.PcmBytes ||
            !Json->TryGetStringField(TEXT("pcm"), Pcm) || Pcm.IsEmpty() || Pcm.Len() > 40960 ||
            !FBase64::Decode(Pcm, Chunk.Pcm) || Chunk.Pcm.IsEmpty() || Chunk.Pcm.Num() % 2 ||
            Chunk.Pcm.Num() > 30720 || Chunk.Pcm.Num() > 384000 - Receipt.PcmBytes)
        { RejectStream(RequestId, TEXT("InvalidAudioChunk")); return; }
        const int32 Bytes = Chunk.Pcm.Num();
        if (!Agent->ReceiveAudioChunk(RequestId, Chunk)) { RejectStream(RequestId, TEXT("AudioChunkRejected")); return; }
        if (auto* Current = Pending.Find(RequestId))
        {
            ++Current->Sentences[Sequence].NextChunkIndex;
            Current->Sentences[Sequence].PcmBytes += Bytes;
        }
        return;
    }
    if (Type == TEXT("audio_end"))
    {
        FDeliveryNpcAudioEnd End;
        FString Status;
        End.Sequence = Sequence;
        if (Receipt.State != ESentenceReception::Streaming || !Json->TryGetStringField(TEXT("status"), Status) ||
            (Status != TEXT("completed") && Status != TEXT("failed")) ||
            !ReadInteger(*Json, TEXT("chunk_count"), 0, 32, End.ChunkCount) || End.ChunkCount != Receipt.NextChunkIndex ||
            !ReadInteger(*Json, TEXT("total_pcm_bytes"), 0, 384000, End.TotalPcmBytes) || End.TotalPcmBytes != Receipt.PcmBytes ||
            (Status == TEXT("completed") && (End.ChunkCount == 0 || End.TotalPcmBytes == 0)) ||
            (Json->HasField(TEXT("audio_error")) && (!Json->TryGetStringField(TEXT("audio_error"), End.Error) || End.Error.Len() > 128)))
        { RejectStream(RequestId, TEXT("InvalidAudioEnd")); return; }
        End.bSucceeded = Status == TEXT("completed");
        if (!Agent->ReceiveAudioEnd(RequestId, End)) { RejectStream(RequestId, TEXT("AudioEndRejected")); return; }
        if (auto* Current = Pending.Find(RequestId))
            Current->Sentences[Sequence].State = End.bSucceeded ? ESentenceReception::Complete : ESentenceReception::Failed;
        return;
    }
    RejectStream(RequestId, TEXT("UnknownEvent"));
}

void UDeliveryAiGatewaySubsystem::Shutdown()
{
    if (bShuttingDown) return;
    bShuttingDown = true;
    WorldEpoch.Invalidate();
    TArray<FGuid> Ids;
    Pending.GetKeys(Ids);
    for (const auto& Id : Ids)
    {
        const auto Agent = Pending[Id].Agent;
        Remove(Id);
        if (Agent.IsValid()) Agent->ReceiveDecision(Id, nullptr, TEXT("WorldEnded"));
    }
}

void UDeliveryAiGatewaySubsystem::OnWorldEndPlay(UWorld& InWorld)
{
    Shutdown();
    Super::OnWorldEndPlay(InWorld);
}

void UDeliveryAiGatewaySubsystem::Deinitialize()
{
    Shutdown();
    Super::Deinitialize();
}

TStatId UDeliveryAiGatewaySubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UDeliveryAiGatewaySubsystem, STATGROUP_Tickables);
}
