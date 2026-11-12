#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AI/DeliveryNpcTypes.h"
#include "DeliveryNpcAgentComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryNpcStateChanged, const FDeliveryNpcPublicState&, State);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryNpcDecisionFinished, FName, Reason);

// 服务器认知和动作状态的唯一修改者，网关只回交建议，行为树只领取和回报动作
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryNpcAgentComponent : public UActorComponent
{
    GENERATED_BODY()
    friend class FDeliveryResidentIntegrationTest;
    friend class FDeliveryResidentPIECommand;
public:
    UDeliveryNpcAgentComponent();

    // 感知接口：只有服务器接收，同一 EventId 在最近 128 条记录内只计一次
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI")
    bool ObserveEvent(const FDeliveryNpcObservation& Observation);
    // true 仅表示请求已排队或已合并，不表示模型已经批准动作
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI") bool RequestDecision();
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI") void CancelDecision();
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI") void CancelPlan(FName Reason);

    // 行为树接口：领取时再次检查目标，结束必须带同一 ActionId
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI")
    bool StartApprovedAction(FDeliveryNpcPublicState& OutAction);
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Delivery|AI")
    bool TryFinishAction(FGuid ActionId, EDeliveryNpcActionStatus Result, FName Reason);
    UFUNCTION(BlueprintPure, Category="Delivery|AI") FDeliveryNpcPublicState GetPublicState() const { return PublicState; }
    UFUNCTION(BlueprintPure, Category="Delivery|AI") bool HasActiveAction() const;
    UFUNCTION(BlueprintPure, Category="Delivery|AI") bool IsDecisionPending() const { return bDecisionPending; }

    // 配置：人格只参与建议生成，不授予额外权限
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|AI") FString Persona = TEXT("普通路人，注意安全并遵守当前动作能力");
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|AI", meta=(ClampMin="100", ClampMax="3000", Units="cm"))
    float MaxLookAtDistance = 1200.f;

    // 事件：服务器和接收复制的客户端均通知本机表现，迟加入者可直接读取当前状态
    UPROPERTY(BlueprintAssignable, Category="Delivery|AI") FDeliveryNpcStateChanged OnPublicStateChanged;
    UPROPERTY(BlueprintAssignable, Category="Delivery|AI") FDeliveryNpcDecisionFinished OnDecisionFinished;

    // 网关内部回交接口，不暴露给客户端 RPC
    bool BuildDecisionRequest(FGuid WorldEpoch, FDeliveryNpcDecisionRequest& OutRequest);
    void ReceiveDecision(FGuid RequestId, const FDeliveryNpcDecisionProposal* Proposal, FName Error);
    FGuid GetNpcId() const { return NpcId; }
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
    UFUNCTION() void OnRep_PublicState();
    bool IsAuthority() const;
    void PruneEvidence();
    bool CanLookAt(AActor* Target) const;
    bool TryAcceptProposal(const FDeliveryNpcDecisionProposal& Proposal, FName& Reason);
    void PublishState();
    double ServerTime() const;

    UPROPERTY(Replicated) bool bDecisionPending=false;
    UPROPERTY(ReplicatedUsing=OnRep_PublicState) FDeliveryNpcPublicState PublicState;
    FGuid NpcId;
    int32 ContextRevision = 0;
    int32 NextPlanRevision = 0;
    FDeliveryNpcDecisionRequest ActiveRequest;
    TArray<FDeliveryNpcEvidence> Evidence;
    TArray<FGuid> RecentEventIds;
    TMap<FGuid, TWeakObjectPtr<AActor>> KnownSubjects;
    TArray<FGuid> ActionEvidenceIds;
    FGuid ActionTargetId;
    bool bDecisionQueued = false;
    bool bEndingPlay = false;
    double ActionDeadline = 0;
};
