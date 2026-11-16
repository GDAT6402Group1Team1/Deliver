#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Presentation/Speech/DeliveryNpcVoiceCodec.h"
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
// 三句 12 秒 PCM 经 base64 后约 2.31 MB，仍为整轮和单行保留明确上限
constexpr int64 MaxStreamResponseBytes = 3 * 1024 * 1024;
constexpr int32 MaxStreamLineBytes = DeliveryNpcVoice::MaxPcmBase64Chars + 16 * 1024;

// 从后端诊断信息中读取一个短标签，含路径、地址或控制字符时丢弃
FString ReadDiagnosticLabel(const FJsonObject& Json, const TCHAR* Key, int32 Limit)
{
    FString Value;
    if (!Json.TryGetStringField(Key, Value) || Value.Len() > Limit)
        return {};
    // 这里只接短标签，地址、文件路径、凭据格式与控制字符不能进入面板
    for (const TCHAR Character : Value)
        if (!FChar::IsAlnum(Character) && Character != TEXT(' ') && Character != TEXT('_') && Character != TEXT('-') &&
            Character != TEXT('.'))
            return {};
    return Value.TrimStartAndEnd();
}

// 读取一个非负数值，读不到时返回 -1
double ReadDiagnosticTime(const FJsonObject& Json, const TCHAR* Key)
{
    double Value = -1;
    return Json.TryGetNumberField(Key, Value) && FMath::IsFinite(Value) && Value >= 0 ? Value : -1;
}

// 读取一个非负整数，读不到时返回 -1
int32 ReadDiagnosticCount(const FJsonObject& Json, const TCHAR* Key)
{
    const double Value = ReadDiagnosticTime(Json, Key);
    return Value >= 0 && Value <= MAX_int32 && FMath::FloorToDouble(Value) == Value ? static_cast<int32>(Value) : -1;
}

// 读取后端随建议附带的诊断：模型名、路由、各段耗时和 token 数，供 AI 检查器显示
bool ReadDiagnostics(const FJsonObject& Event, FDeliveryNpcDecisionTrace& Trace)
{
    Trace.InputSummary = TEXT("实际模型输入：未提供");
    const TSharedPtr<FJsonObject>* Object = nullptr;
    if (!Event.TryGetObjectField(TEXT("diagnostics"), Object) || !Object || !Object->IsValid())
        return false;
    const auto& Json = **Object;
    Trace.ModelProvider = ReadDiagnosticLabel(Json, TEXT("model_provider"), 64);
    Trace.ActualModelName = ReadDiagnosticLabel(Json, TEXT("model_name"), 96);
    Trace.FallbackReason = ReadDiagnosticLabel(Json, TEXT("fallback_reason"), 64);
    const FString Source = ReadDiagnosticLabel(Json, TEXT("request_source"), 16);
    if (Source == TEXT("player") || Source == TEXT("ambient"))
        Trace.RequestSource = Source;
    const FString Route = ReadDiagnosticLabel(Json, TEXT("decision_route"), 32);
    if (Route == TEXT("local_combat") || Route == TEXT("player_dialogue") || Route == TEXT("ambient_dialogue") ||
        Route == TEXT("provider_dispatch"))
        Trace.Route = Route;
    Trace.BackendQueueMs = ReadDiagnosticTime(Json, TEXT("queue_ms"));
    Trace.BackendDecisionMs = ReadDiagnosticTime(Json, TEXT("decision_ms"));
    Trace.BackendElapsedMs = ReadDiagnosticTime(Json, TEXT("backend_elapsed_ms"));
    Trace.FirstSentenceMs = ReadDiagnosticTime(Json, TEXT("first_sentence_ms"));
    Trace.InputTokens = ReadDiagnosticCount(Json, TEXT("input_tokens"));
    Trace.CachedInputTokens = ReadDiagnosticCount(Json, TEXT("cached_input_tokens"));
    Trace.OutputTokens = ReadDiagnosticCount(Json, TEXT("output_tokens"));
    const auto CountText = [&Json](const TCHAR* Key)
    {
        const int32 Count = ReadDiagnosticCount(Json, Key);
        return Count >= 0 ? FString::FromInt(Count) : FString(TEXT("未提供"));
    };
    Trace.InputSummary =
        FString::Printf(TEXT("实际模型输入：事件 %s、对象 %s、动作候选 %s"), *CountText(TEXT("input_event_count")),
                        *CountText(TEXT("input_subject_count")), *CountText(TEXT("input_candidate_count")));
    return true;
}

// 读取一个在给定范围内的整数字段
bool ReadInteger(const FJsonObject& Json, const TCHAR* Key, int32 Minimum, int32 Maximum, int32& Out)
{
    double Value = 0;
    if (!Json.TryGetNumberField(Key, Value) || !FMath::IsFinite(Value) || Value < Minimum || Value > Maximum ||
        FMath::FloorToDouble(Value) != Value)
        return false;
    Out = static_cast<int32>(Value);
    return true;
}

// 读取一句台词和情绪，台词不超过 120 字
bool ReadSpeechText(const FJsonObject& Json, FString& Text, FName& Emotion)
{
    FString Value;
    if (!Json.TryGetStringField(TEXT("text"), Text) || Text.Len() > 120 || Text.TrimStartAndEnd().IsEmpty() ||
        !Json.TryGetStringField(TEXT("emotion"), Value) ||
        (Value != TEXT("Calm") && Value != TEXT("Angry") && Value != TEXT("Surprised")))
        return false;
    Emotion = FName(Value);
    return true;
}
} // namespace

// 只在游戏和 PIE 世界创建
bool UDeliveryAiGatewaySubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

// 生成世界编号，服务器加载共享行为表 DT_NpcBehaviorCatalog
void UDeliveryAiGatewaySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    WorldEpoch = FGuid::NewGuid();
    bShuttingDown = false;
    const auto* Settings = GetDefault<UDeliveryAiSettings>();
    if (Settings->bEnabled && GetWorld()->GetNetMode() != NM_Client && !Settings->NpcBehaviorCatalog.IsNull())
    {
        const auto* Table = Settings->NpcBehaviorCatalog.LoadSynchronous();
        if (!Table || !BehaviorCatalog.Load(*Table))
        {
            UE_LOG(LogTemp, Warning, TEXT("NPC 行为表未加载或存在无效行，继续使用现有角色候选"));
        }
        else
        {
            UE_LOG(LogTemp, Log, TEXT("NpcBehaviorCatalog loaded table=%s count=%d"), *Table->GetPathName(),
                   BehaviorCatalog.Num());
        }
    }
}

// 交谈流程第 11 步，服务器：请求入队，玩家请求排在 NPC 闲聊前面
bool UDeliveryAiGatewaySubsystem::Enqueue(UDeliveryNpcAgentComponent* Agent, const FDeliveryNpcDecisionRequest& Request)
{
    const auto* Settings = GetDefault<UDeliveryAiSettings>();
    if (bShuttingDown || !Settings->bEnabled || GetWorld()->GetNetMode() == NM_Client || !IsValid(Agent) ||
        Agent->GetWorld() != GetWorld() || !Agent->GetOwner()->HasAuthority() || Request.WorldEpoch != WorldEpoch ||
        Request.NpcId != Agent->GetNpcId() || !Request.RequestId.IsValid() || Pending.Contains(Request.RequestId) ||
        Pending.Num() >= 3 * FMath::Clamp(Settings->MaxPendingRequests, 1, 64))
        return false;

    // 统计这个 NPC 和全部等待中的请求数量，超出上限时拒绝
    int32 WaitingModels = 0, NpcRequests = 0;
    for (const auto& Pair : Pending)
    {
        if (!Pair.Value.bModelFinished)
            ++WaitingModels;
        if (Pair.Value.Agent != Agent)
            continue;
        ++NpcRequests;
        if (!Pair.Value.bModelFinished)
            return false;
    }
    // 同一个 NPC 最多接收正在播放、等待播放和当前模型请求这三份数据
    if (NpcRequests >= 3 || WaitingModels >= FMath::Clamp(Settings->MaxPendingRequests, 1, 64))
        return false;

    // 记下截止时间和后端地址
    FPending Entry;
    Entry.Agent = Agent;
    Entry.Request = Request;
    Entry.Deadline = FPlatformTime::Seconds() + FMath::Clamp(Settings->RequestTimeout, .5f, 30.f);
    Entry.Endpoint = Settings->DecisionEndpoint;
    Pending.Add(Request.RequestId, MoveTemp(Entry));

    // 玩家请求插到队列前部
    if (Request.bPlayerRequest)
    {
        int32 Index = 0;
        while (Index < Queue.Num() && Pending[Queue[Index]].Request.bPlayerRequest)
            ++Index;
        Queue.Insert(Request.RequestId, Index);
    }
    else
        Queue.Add(Request.RequestId);
    Agent->BeginDecisionTrace(Request);
    return true;
}

// 移除一个请求：向后端发 /cancel，再取消本地 HTTP 连接
void UDeliveryAiGatewaySubsystem::Remove(FGuid RequestId)
{
    FPending Entry;
    if (!Pending.RemoveAndCopyValue(RequestId, Entry))
        return;
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

// 取消某个 NPC 的全部请求
void UDeliveryAiGatewaySubsystem::Cancel(UDeliveryNpcAgentComponent* Agent)
{
    TArray<FGuid> Ids;
    for (const auto& Pair : Pending)
        if (Pair.Value.Agent == Agent)
            Ids.Add(Pair.Key);
    for (const auto& Id : Ids)
    {
        if (const auto* Entry = Pending.Find(Id))
            Agent->TraceRequestEvent(Entry->Request.WorldEpoch, Id, TEXT("Cancelled"), TEXT("RequestCancelled"));
        Remove(Id);
    }
}

// 取消某个 NPC 的一个请求，并在诊断记录中标记为已取消
void UDeliveryAiGatewaySubsystem::CancelRequest(UDeliveryNpcAgentComponent* Agent, FGuid RequestId)
{
    const auto* Entry = Pending.Find(RequestId);
    if (Entry && Entry->Agent == Agent)
    {
        const auto& Trace = Agent->GetDecisionTrace();
        if (!Entry->bReplyFinished && Trace.Stage != TEXT("Failed") && Trace.Stage != TEXT("Cancelled"))
            Agent->TraceRequestEvent(Entry->Request.WorldEpoch, RequestId, TEXT("Cancelled"), TEXT("RequestCancelled"));
        Remove(RequestId);
    }
}

// 请求结束：超时、失败或非流式回复，先移除请求，再通知 Agent
void UDeliveryAiGatewaySubsystem::Complete(FGuid RequestId, const FString& Body, FName Error)
{
    check(IsInGameThread());
    const FPending* Entry = Pending.Find(RequestId);
    if (bShuttingDown || !Entry)
        return;
    auto Agent = Entry->Agent;
    const FGuid RequestWorld = Entry->Request.WorldEpoch;
    const bool bStream = Entry->Stream.IsValid();
    if (FPlatformTime::Seconds() >= Entry->Deadline)
        Error = TEXT("RequestTimeout");
    FDeliveryNpcDecisionProposal Proposal;
    const bool bDecoded = Error.IsNone() && DeliveryNpc::DecodeProposal(Body, Proposal, Error);

    // 先移除，给下一个请求留出名额
    // 先减少等待中的请求数，再通知 Agent，给下一次请求留出位置
    Remove(RequestId);
    if (bStream)
    {
        if (Agent.IsValid() && Agent->GetWorld() == GetWorld())
        {
            Agent->TraceRequestEvent(RequestWorld, RequestId, TEXT("Failed"), Error);
            Agent->FinishReply(RequestId, false);
        }
        return;
    }
    if (Agent.IsValid() && Agent->GetWorld() == GetWorld())
    {
        // 先记录真实响应耗时，ReceiveDecision 可能立即发起下一轮请求
        Agent->TraceRequestEvent(RequestWorld, RequestId, bDecoded ? FName(TEXT("ReplyDone")) : FName(TEXT("Failed")), Error);
        Agent->ReceiveDecision(RequestId, bDecoded ? &Proposal : nullptr, Error);
    }
}

// 交谈流程第 11 步，服务器：创建 HTTP 请求，POST 到后端 /stream，日志 http_start
void UDeliveryAiGatewaySubsystem::Dispatch(FGuid RequestId)
{
    FPending* Entry = Pending.Find(RequestId);
    if (!Entry || Entry->Http)
        return;

    // 请求体由 DeliveryNpc::EncodeRequest 编码成 JSON
    auto Http = FHttpModule::Get().CreateRequest();
    Entry->Http = Http;
    const bool bStream = GetDefault<UDeliveryAiSettings>()->bStreamReplies;
    Http->SetURL(Entry->Endpoint + (bStream ? TEXT("/stream") : TEXT("")));
    Http->SetVerb(TEXT("POST"));
    Http->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Http->SetContentAsString(DeliveryNpc::EncodeRequest(Entry->Request));
    // 游戏线程分别检查模型和声音的截止时间，HTTP 连接为后续声音保留有限时间
    Http->SetTimeout(static_cast<float>(FMath::Max(.1, Entry->Deadline - FPlatformTime::Seconds()) +
                                        (bStream && Entry->Request.StreamProtocolVersion == 2 ? 30 : 0)));
    Http->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
    const TWeakObjectPtr<UDeliveryAiGatewaySubsystem> WeakThis(this);

    // 流式：后台线程收到的数据按行放进缓冲区，游戏线程在 Tick 中读取
    if (bStream)
    {
        Entry->Stream = MakeShared<FDeliveryNpcStreamBuffer, ESPMode::ThreadSafe>();
        auto Buffer = Entry->Stream;
        Http->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda(
            [Buffer](void* Data, int64& Size)
            {
                if (Size < 0 || Size > MaxStreamResponseBytes - Buffer->Total)
                {
                    Size = 0;
                    return;
                }
                Buffer->Total += Size;
                Buffer->Bytes.Append(static_cast<uint8*>(Data), static_cast<int32>(Size));
                int32 Start = 0;
                for (int32 Index = 0; Index < Buffer->Bytes.Num(); ++Index)
                    if (Buffer->Bytes[Index] == '\n')
                    {
                        if (Index - Start > MaxStreamLineBytes)
                        {
                            Size = 0;
                            return;
                        }
                        FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Buffer->Bytes.GetData() + Start), Index - Start);
                        Buffer->Lines.Enqueue(FString(Text.Length(), Text.Get()));
                        Start = Index + 1;
                    }
                if (Start)
                    Buffer->Bytes.RemoveAt(0, Start, EAllowShrinking::No);
                if (Buffer->Bytes.Num() > MaxStreamLineBytes)
                    Size = 0;
            }));
    }
    UE_LOG(LogTemp, Log, TEXT("NpcLatency request=%s stage=http_start player=%d"), *RequestId.ToString(),
           Entry->Request.bPlayerRequest);

    // HTTP 结束回调，在游戏线程执行
    Http->OnProcessRequestComplete().BindLambda(
        [WeakThis, RequestId, bStream](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
        {
            auto* Self = WeakThis.Get();
            if (!Self)
                return;
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
            else
                Self->Complete(RequestId, Response->GetContentAsString(), NAME_None);
        });
    if (Entry->Agent.IsValid() && Entry->Agent->GetWorld() == GetWorld())
        Entry->Agent->TraceRequestEvent(Entry->Request.WorldEpoch, RequestId, TEXT("Requesting"));
    // 诊断订阅只读状态，仍防止同步退出后继续启动已经移除的请求
    if (!Pending.Contains(RequestId))
        return;
    if (!Http->ProcessRequest())
        Complete(RequestId, {}, TEXT("TransportFailure"));
}

// 服务器每帧：读取流式数据，处理超时，按并发上限发出排队中的请求
void UDeliveryAiGatewaySubsystem::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    if (bShuttingDown || GetWorld()->GetNetMode() == NM_Client)
        return;

    // 把缓冲区里的每一行交给 ReadStream
    TArray<FGuid> StreamingIds;
    for (const auto& Pair : Pending)
        if (Pair.Value.Stream)
            StreamingIds.Add(Pair.Key);
    for (const FGuid& Id : StreamingIds)
    {
        auto* Entry = Pending.Find(Id);
        if (!Entry)
            continue;
        auto Buffer = Entry->Stream;
        FString Line;
        while (Pending.Contains(Id) && Buffer->Lines.Dequeue(Line))
            ReadStream(Id, Line);
        Entry = Pending.Find(Id);
        if (Entry && Entry->bHttpFinished)
            FinishStreamRequest(Id);
    }

    // 超时或 NPC 已销毁的请求按超时结束
    TArray<FGuid> Expired;
    const double Now = FPlatformTime::Seconds();
    for (const auto& Pair : Pending)
        if (!Pair.Value.Agent.IsValid() || Now >= Pair.Value.Deadline)
            Expired.Add(Pair.Key);
    for (const auto& Id : Expired)
        Complete(Id, {}, TEXT("RequestTimeout"));

    // 统计进行中的请求，玩家请求等待时不发新的闲聊请求，闲聊同时最多一个
    int32 InFlight = 0;
    int32 AmbientInFlight = 0;
    for (const auto& Pair : Pending)
        if (Pair.Value.Http && !Pair.Value.bModelFinished)
        {
            ++InFlight;
            if (!Pair.Value.Request.bPlayerRequest)
                ++AmbientInFlight;
        }
    const int32 Limit = FMath::Clamp(GetDefault<UDeliveryAiSettings>()->MaxConcurrentRequests, 1, 8);
    // 使用请求列表的副本逐个发送，发送失败时可以安全移除原列表中的请求
    const TArray<FGuid> Waiting = Queue;
    bool bPlayerWaiting = false;
    for (const auto& Pair : Pending)
        bPlayerWaiting |= Pair.Value.Request.bPlayerRequest && !Pair.Value.bModelFinished;
    for (const auto& Id : Waiting)
    {
        if (InFlight >= Limit)
            break;
        const FPending* Entry = Pending.Find(Id);
        if (!Entry || Entry->Http)
            continue;
        // 优先处理玩家发言，新的 NPC 闲聊稍后再发送
        const bool bAmbient = !Entry->Request.bPlayerRequest;
        if (bAmbient && (bPlayerWaiting || AmbientInFlight >= 1))
            continue;
        Dispatch(Id);
        if (Pending.Contains(Id))
        {
            ++InFlight;
            if (bAmbient)
                ++AmbientInFlight;
        }
    }
}

// 流式请求的 HTTP 结束后，按是否完整收到回复通知 Agent
void UDeliveryAiGatewaySubsystem::FinishStreamRequest(FGuid RequestId)
{
    const auto* Entry = Pending.Find(RequestId);
    if (!Entry || !Entry->bHttpFinished)
        return;
    const bool bSucceeded = Entry->bHttpSucceeded && Entry->bReplyFinished && FPlatformTime::Seconds() < Entry->Deadline;
    const int32 SentenceCount = Entry->SentenceCount;
    const auto Agent = Entry->Agent;
    const FGuid RequestWorld = Entry->Request.WorldEpoch;
    Remove(RequestId);
    if (Agent.IsValid() && Agent->GetWorld() == GetWorld())
    {
        if (!bSucceeded)
            Agent->TraceRequestEvent(RequestWorld, RequestId, TEXT("Failed"), TEXT("TransportFailure"));
        Agent->FinishReply(RequestId, bSucceeded, SentenceCount);
    }
}

// 流式数据格式错误：记录原因，按失败结束本次请求
void UDeliveryAiGatewaySubsystem::RejectStream(FGuid RequestId, const TCHAR* Reason)
{
    const auto* Entry = Pending.Find(RequestId);
    if (!Entry)
        return;
    const auto Agent = Entry->Agent;
    const FGuid RequestWorld = Entry->Request.WorldEpoch;
    UE_LOG(LogTemp, Log, TEXT("NpcSpeech stream_rejected request=%s reason=%s"), *RequestId.ToString(), Reason);
    Remove(RequestId);
    if (Agent.IsValid() && Agent->GetWorld() == GetWorld())
    {
        Agent->TraceRequestEvent(RequestWorld, RequestId, TEXT("Failed"), FName(Reason));
        Agent->FinishReply(RequestId, false);
    }
}

// 交谈流程第 14 步，服务器：读取后端发来的一行 JSON，按类型交给 Agent
void UDeliveryAiGatewaySubsystem::ReadStream(FGuid RequestId, const FString& Line)
{
    auto* Entry = Pending.Find(RequestId);
    if (!Entry || !Entry->Agent.IsValid() || Entry->Agent->GetWorld() != GetWorld() || Entry->Request.WorldEpoch != WorldEpoch)
        return;
    if (FPlatformTime::Seconds() >= Entry->Deadline)
    {
        RejectStream(RequestId, TEXT("RequestTimeout"));
        return;
    }

    // 解析 JSON，忽略心跳和不属于本请求的行
    auto* Agent = Entry->Agent.Get();
    TSharedPtr<FJsonObject> Json;
    FString Type;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Json) || !Json.IsValid() ||
        !Json->TryGetStringField(TEXT("type"), Type))
    {
        RejectStream(RequestId, TEXT("InvalidJson"));
        return;
    }
    if (Type == TEXT("keepalive"))
        return;
    FString Id;
    if (!Json->TryGetStringField(TEXT("request_id"), Id) || Id != RequestId.ToString(EGuidFormats::DigitsWithHyphensLower))
        return;
    if (Type == TEXT("error"))
    {
        RejectStream(RequestId, TEXT("BackendError"));
        return;
    }
    if (Entry->bReplyFinished)
    {
        RejectStream(RequestId, TEXT("EventAfterDone"));
        return;
    }

    // proposal：模型选择的动作和第一句台词，交给 Agent 校验
    if (Type == TEXT("proposal"))
    {
        if (Entry->bHasProposal)
        {
            RejectStream(RequestId, TEXT("DuplicateProposal"));
            return;
        }
        const TSharedPtr<FJsonObject>* Object;
        if (!Json->TryGetObjectField(TEXT("proposal"), Object))
        {
            RejectStream(RequestId, TEXT("InvalidProposal"));
            return;
        }
        FString Body;
        FJsonSerializer::Serialize(Object->ToSharedRef(), TJsonWriterFactory<>::Create(&Body));
        FDeliveryNpcDecisionProposal Proposal;
        FName Reason;
        if (!DeliveryNpc::DecodeProposal(Body, Proposal, Reason) || !Agent->ReceiveStreamDecision(RequestId, Proposal))
        {
            RejectStream(RequestId, TEXT("RejectedProposal"));
            return;
        }
        Entry = Pending.Find(RequestId);
        if (Entry)
            Entry->bHasProposal = true;
        return;
    }
    if (!Entry->bHasProposal)
    {
        RejectStream(RequestId, TEXT("MissingProposal"));
        return;
    }

    // model_done：模型输出结束，记下共有几句
    if (Type == TEXT("model_done"))
    {
        int32 Count = 0;
        if (Entry->Request.StreamProtocolVersion != 2 || Entry->bModelFinished ||
            !ReadInteger(*Json, TEXT("sentences"), 0, 3, Count))
        {
            RejectStream(RequestId, TEXT("InvalidModelDone"));
            return;
        }
        TOptional<FString> ApprovedText;
        if (Json->HasField(TEXT("reply_text")))
        {
            FString Text;
            if (!Json->TryGetStringField(TEXT("reply_text"), Text) || !DeliveryNpcMessages::IsValidReplyText(Text) ||
                ((Count == 0) != Text.IsEmpty()))
            {
                RejectStream(RequestId, TEXT("InvalidReplyText"));
                return;
            }
            ApprovedText = MoveTemp(Text);
        }
        for (int32 Index = Count; Index < 3; ++Index)
            if (Entry->Sentences[Index].State != ESentenceReception::Empty)
            {
                RejectStream(RequestId, TEXT("UnexpectedSentence"));
                return;
            }
        Entry->bModelFinished = true;
        Entry->SentenceCount = Count;
        Entry->Deadline = FPlatformTime::Seconds() + 30;
        FDeliveryNpcDecisionTrace Diagnostics;
        ReadDiagnostics(*Json, Diagnostics);
        Agent->TraceDiagnostics(Entry->Request.WorldEpoch, RequestId, Diagnostics);
        Agent->TraceRequestEvent(Entry->Request.WorldEpoch, RequestId, TEXT("ModelDone"));
        // 先更新请求记录，再通知 NPC，回调中可以开始下一轮模型请求
        if (!Agent->FinishModelReply(RequestId, Count, MoveTemp(ApprovedText)))
        {
            RejectStream(RequestId, TEXT("ModelDoneRejected"));
            return;
        }
        UE_LOG(LogTemp, Log, TEXT("NpcLatency request=%s stage=model_done sentences=%d"), *RequestId.ToString(), Count);
        return;
    }

    // done：整轮回复结束，包括语音
    if (Type == TEXT("done"))
    {
        int32 Count = 0;
        if (!ReadInteger(*Json, TEXT("sentences"), 0, 3, Count))
        {
            RejectStream(RequestId, TEXT("InvalidSentenceCount"));
            return;
        }
        if (Entry->bModelFinished && Count != Entry->SentenceCount)
        {
            RejectStream(RequestId, TEXT("SentenceCountChanged"));
            return;
        }
        for (int32 Index = 0; Index < 3; ++Index)
        {
            const auto& Sentence = Entry->Sentences[Index];
            if ((Index < Count && !Sentence.IsFinished()) || (Index >= Count && Sentence.State != ESentenceReception::Empty))
            {
                RejectStream(RequestId, TEXT("IncompleteSentences"));
                return;
            }
        }
        Entry->SentenceCount = Count;
        Entry->bReplyFinished = true;
        Agent->TraceRequestEvent(Entry->Request.WorldEpoch, RequestId, TEXT("ReplyDone"));
        return;
    }

    // 以下事件都属于某一句，先读句子序号
    int32 Sequence = 0;
    if (!ReadInteger(*Json, TEXT("sequence"), 0, 2, Sequence))
    {
        RejectStream(RequestId, TEXT("InvalidSequence"));
        return;
    }
    if (Entry->bModelFinished && Sequence >= Entry->SentenceCount)
    {
        RejectStream(RequestId, TEXT("UnexpectedSentence"));
        return;
    }
    auto& Receipt = Entry->Sentences[Sequence];

    // sentence：整句文字和声音一次到达
    if (Type == TEXT("sentence"))
    {
        FDeliveryNpcSentence Sentence;
        FString Pcm;
        Sentence.Sequence = Sequence;
        if (Receipt.State != ESentenceReception::Empty || !ReadSpeechText(*Json, Sentence.Text, Sentence.Emotion) ||
            !Json->TryGetStringField(TEXT("pcm"), Pcm) ||
            !ReadInteger(*Json, TEXT("sample_rate"), 16000, 24000, Sentence.SampleRate) ||
            (Sentence.SampleRate != 16000 && Sentence.SampleRate != 24000) || Pcm.Len() > DeliveryNpcVoice::MaxPcmBase64Chars ||
            (!Pcm.IsEmpty() && !FBase64::Decode(Pcm, Sentence.Pcm)) || Sentence.Pcm.Num() % 2 ||
            Sentence.Pcm.Num() > DeliveryNpcVoice::GetMaxPcmBytes(Sentence.SampleRate))
        {
            RejectStream(RequestId, TEXT("InvalidWholeSentence"));
            return;
        }
        Receipt.State = ESentenceReception::Whole;
        Receipt.PcmBytes = Sentence.Pcm.Num();
        Agent->ReceiveSentence(RequestId, Sentence);
        return;
    }
    // 新传输只接收本次请求明确协商过的分段，旧后端返回完整句子仍可使用
    if (!Entry->Request.bStreamSentenceAudio)
    {
        RejectStream(RequestId, TEXT("AudioStreamNotRequested"));
        return;
    }

    // speech_begin：分段声音的开头，带文字和情绪
    if (Type == TEXT("speech_begin"))
    {
        FDeliveryNpcSpeechBegin Begin;
        Begin.Sequence = Sequence;
        if (Receipt.State != ESentenceReception::Empty || !ReadSpeechText(*Json, Begin.Text, Begin.Emotion) ||
            !ReadInteger(*Json, TEXT("sample_rate"), 24000, 24000, Begin.SampleRate) ||
            !ReadInteger(*Json, TEXT("audio_stream_version"), 1, 1, Begin.AudioStreamVersion))
        {
            RejectStream(RequestId, TEXT("InvalidSpeechBegin"));
            return;
        }
        if (!Agent->ReceiveSpeechBegin(RequestId, Begin))
        {
            RejectStream(RequestId, TEXT("SpeechBeginRejected"));
            return;
        }
        if (auto* Current = Pending.Find(RequestId))
            Current->Sentences[Sequence].State = ESentenceReception::Streaming;
        return;
    }

    // audio_chunk：一段 PCM 声音
    if (Type == TEXT("audio_chunk"))
    {
        FDeliveryNpcAudioChunk Chunk;
        FString Pcm;
        Chunk.Sequence = Sequence;
        if (Receipt.State != ESentenceReception::Streaming ||
            !ReadInteger(*Json, TEXT("chunk_index"), 0, DeliveryNpcVoice::MaxAudioChunks - 1, Chunk.ChunkIndex) ||
            Chunk.ChunkIndex != Receipt.NextChunkIndex ||
            !ReadInteger(*Json, TEXT("offset_bytes"), 0, DeliveryNpcVoice::MaxPcmBytes, Chunk.OffsetBytes) ||
            Chunk.OffsetBytes != Receipt.PcmBytes || !Json->TryGetStringField(TEXT("pcm"), Pcm) || Pcm.IsEmpty() ||
            Pcm.Len() > 40960 || !FBase64::Decode(Pcm, Chunk.Pcm) || Chunk.Pcm.IsEmpty() || Chunk.Pcm.Num() % 2 ||
            Chunk.Pcm.Num() > 30720 || Chunk.Pcm.Num() > DeliveryNpcVoice::MaxPcmBytes - Receipt.PcmBytes)
        {
            RejectStream(RequestId, TEXT("InvalidAudioChunk"));
            return;
        }
        const int32 Bytes = Chunk.Pcm.Num();
        if (!Agent->ReceiveAudioChunk(RequestId, Chunk))
        {
            RejectStream(RequestId, TEXT("AudioChunkRejected"));
            return;
        }
        if (auto* Current = Pending.Find(RequestId))
        {
            ++Current->Sentences[Sequence].NextChunkIndex;
            Current->Sentences[Sequence].PcmBytes += Bytes;
        }
        return;
    }

    // audio_end：这一句的声音结束
    if (Type == TEXT("audio_end"))
    {
        FDeliveryNpcAudioEnd End;
        FString Status;
        End.Sequence = Sequence;
        if (Receipt.State != ESentenceReception::Streaming || !Json->TryGetStringField(TEXT("status"), Status) ||
            (Status != TEXT("completed") && Status != TEXT("failed")) ||
            !ReadInteger(*Json, TEXT("chunk_count"), 0, DeliveryNpcVoice::MaxAudioChunks, End.ChunkCount) ||
            End.ChunkCount != Receipt.NextChunkIndex ||
            !ReadInteger(*Json, TEXT("total_pcm_bytes"), 0, DeliveryNpcVoice::MaxPcmBytes, End.TotalPcmBytes) ||
            End.TotalPcmBytes != Receipt.PcmBytes ||
            (Status == TEXT("completed") && (End.ChunkCount == 0 || End.TotalPcmBytes == 0)) ||
            (Json->HasField(TEXT("audio_error")) &&
             (!Json->TryGetStringField(TEXT("audio_error"), End.Error) || End.Error.Len() > 128)))
        {
            RejectStream(RequestId, TEXT("InvalidAudioEnd"));
            return;
        }
        End.bSucceeded = Status == TEXT("completed");
        if (!Agent->ReceiveAudioEnd(RequestId, End))
        {
            RejectStream(RequestId, TEXT("AudioEndRejected"));
            return;
        }
        if (auto* Current = Pending.Find(RequestId))
            Current->Sentences[Sequence].State = End.bSucceeded ? ESentenceReception::Complete : ESentenceReception::Failed;
        return;
    }
    RejectStream(RequestId, TEXT("UnknownEvent"));
}

// 取消全部请求并清空队列，之后不再接收回复
void UDeliveryAiGatewaySubsystem::Shutdown()
{
    if (bShuttingDown)
        return;
    bShuttingDown = true;
    WorldEpoch.Invalidate();
    TArray<FGuid> Ids;
    Pending.GetKeys(Ids);
    for (const auto& Id : Ids)
    {
        // 取消一条回复时可能同时移除同 NPC 的待播回复
        const auto* Entry = Pending.Find(Id);
        if (!Entry)
            continue;
        const auto Agent = Entry->Agent;
        const bool bStream = Entry->Stream.IsValid();
        Remove(Id);
        if (Agent.IsValid())
        {
            if (bStream)
                Agent->FinishReply(Id, false);
            else
                Agent->ReceiveDecision(Id, nullptr, TEXT("WorldEnded"));
        }
    }
}

// 世界结束时取消全部请求
void UDeliveryAiGatewaySubsystem::OnWorldEndPlay(UWorld& InWorld)
{
    Shutdown();
    Super::OnWorldEndPlay(InWorld);
}

// 子系统销毁时取消全部请求
void UDeliveryAiGatewaySubsystem::Deinitialize()
{
    Shutdown();
    Super::Deinitialize();
}

// 性能统计用
TStatId UDeliveryAiGatewaySubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UDeliveryAiGatewaySubsystem, STATGROUP_Tickables);
}
