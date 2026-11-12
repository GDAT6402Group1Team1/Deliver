#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "AI/DeliveryNpcTypes.h"
#include "DeliveryAiGatewaySubsystem.generated.h"

class UDeliveryNpcAgentComponent;
class IHttpRequest;

// 每个 World 独立的 HTTP 请求所有者，结束世界时取消传输并使所有旧回复失效
UCLASS()
class DELIVERY_API UDeliveryAiGatewaySubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    // 排队成功才占用预算，每个 Agent 最多保留一个请求
    bool Enqueue(UDeliveryNpcAgentComponent* Agent, const FDeliveryNpcDecisionRequest& Request);
    void Cancel(UDeliveryNpcAgentComponent* Agent);
    FGuid GetWorldEpoch() const { return WorldEpoch; }
    int32 GetPendingCount() const { return Pending.Num(); }
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void OnWorldEndPlay(UWorld& InWorld) override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

protected:
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
    struct FPending
    {
        TWeakObjectPtr<UDeliveryNpcAgentComponent> Agent;
        FDeliveryNpcDecisionRequest Request;
        TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Http;
        double Deadline = 0;
        FString Endpoint;
    };
    void Dispatch(FGuid RequestId);
    void Complete(FGuid RequestId, const FString& Body, FName Error);
    void Remove(FGuid RequestId);
    void Shutdown();
    TMap<FGuid, FPending> Pending;
    TArray<FGuid> Queue;
    FGuid WorldEpoch;
    bool bShuttingDown = false;
    friend class FDeliveryAiInfrastructureTest;
    friend class FDeliveryAiBehaviorTreeTest;
};
