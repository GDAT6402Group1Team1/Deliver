#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "AI/Decision/DeliveryNpcTypes.h"
#include "DeliveryAiGatewaySubsystem.generated.h"

class UDeliveryNpcAgentComponent;
class IHttpRequest;
struct FDeliveryNpcStreamBuffer;

// 为当前关卡发送模型请求，处理排队、超时和取消
// 每个游戏世界分别保存请求，退出关卡后不再处理它的回复
UCLASS()
class DELIVERY_API UDeliveryAiGatewaySubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    // 返回 true 表示请求已加入队列，每个 NPC 最多保留一个等待中的请求
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
        TSharedPtr<FDeliveryNpcStreamBuffer, ESPMode::ThreadSafe> Stream;
        bool bHttpFinished = false;
        bool bHttpSucceeded = false;
        bool bReplyFinished = false;
        bool bHasProposal = false;
        int32 SentenceCount = 0;
    };
    void Dispatch(FGuid RequestId);
    void Complete(FGuid RequestId, const FString& Body, FName Error);
    void Remove(FGuid RequestId);
    void Shutdown();
    void ReadStream(FGuid RequestId, const FString& Line);
    TMap<FGuid, FPending> Pending;
    TArray<FGuid> Queue;
    FGuid WorldEpoch;
    bool bShuttingDown = false;
    friend class FDeliveryAiInfrastructureTest;
    friend class FDeliveryAiBehaviorTreeTest;
};
