#include "AI/DeliveryAiGatewaySubsystem.h"
#include "AI/DeliveryAiSettings.h"
#include "AI/DeliveryNpcAgentComponent.h"
#include "Engine/World.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

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
    Queue.Add(Request.RequestId);
    return true;
}

void UDeliveryAiGatewaySubsystem::Remove(FGuid RequestId)
{
    FPending Entry;
    if (!Pending.RemoveAndCopyValue(RequestId, Entry)) return;
    Queue.Remove(RequestId);
    if (Entry.Http)
    {
        // 先移除记录并解绑，再取消传输，取消回调也不能重新进入 Agent
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
    if (FPlatformTime::Seconds() >= Entry->Deadline) Error = TEXT("RequestTimeout");
    FDeliveryNpcDecisionProposal Proposal;
    const bool bDecoded = Error.IsNone() && DeliveryNpc::DecodeProposal(Body, Proposal, Error);
    // 释放预算后才通知 Agent，合并的下一次请求可以安全地排队
    Remove(RequestId);
    if (Agent.IsValid() && Agent->GetWorld() == GetWorld())
        Agent->ReceiveDecision(RequestId, bDecoded ? &Proposal : nullptr, Error);
}

void UDeliveryAiGatewaySubsystem::Dispatch(FGuid RequestId)
{
    FPending* Entry = Pending.Find(RequestId);
    if (!Entry || Entry->Http) return;
    auto Http = FHttpModule::Get().CreateRequest();
    Entry->Http = Http;
    Http->SetURL(Entry->Endpoint);
    Http->SetVerb(TEXT("POST"));
    Http->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Http->SetContentAsString(DeliveryNpc::EncodeRequest(Entry->Request));
    Http->SetTimeout(static_cast<float>(FMath::Max(.1, Entry->Deadline - FPlatformTime::Seconds())));
    Http->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
    const TWeakObjectPtr<UDeliveryAiGatewaySubsystem> WeakThis(this);
    Http->OnProcessRequestComplete().BindLambda([WeakThis, RequestId](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
    {
        auto* Self = WeakThis.Get();
        if (!Self) return;
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
    TArray<FGuid> Expired;
    const double Now = FPlatformTime::Seconds();
    for (const auto& Pair : Pending)
        if (!Pair.Value.Agent.IsValid() || Now >= Pair.Value.Deadline) Expired.Add(Pair.Key);
    for (const auto& Id : Expired) Complete(Id, {}, TEXT("RequestTimeout"));
    int32 InFlight = 0;
    for (const auto& Pair : Pending) if (Pair.Value.Http) ++InFlight;
    const int32 Limit = FMath::Clamp(GetDefault<UDeliveryAiSettings>()->MaxConcurrentRequests, 1, 8);
    // 拷贝队列避免 ProcessRequest 同步失败时的移除操作使迭代器失效
    const TArray<FGuid> Waiting = Queue;
    for (const auto& Id : Waiting)
    {
        if (InFlight >= Limit) break;
        const FPending* Entry = Pending.Find(Id);
        if (!Entry || Entry->Http) continue;
        Dispatch(Id);
        if (Pending.Contains(Id)) ++InFlight;
    }
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
