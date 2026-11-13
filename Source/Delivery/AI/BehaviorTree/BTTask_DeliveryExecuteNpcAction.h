#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_DeliveryExecuteNpcAction.generated.h"

class UDeliveryNpcAgentComponent;
class AAIController;
class ADeliveryNpcCharacter;
struct FDeliveryNpcPublicState;

// 在行为树中执行 Agent 已检查的动作，并报告成功、失败或取消
// 等待模型回复由 Agent 处理，这个节点只负责动作执行
UCLASS()
class DELIVERY_API UBTTask_DeliveryExecuteNpcAction : public UBTTaskNode
{
    GENERATED_BODY()
public:
    UBTTask_DeliveryExecuteNpcAction();
    virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
    virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
    virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
    virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) override;

private:
    void ReleaseFocus();
    bool StartNearbyMove(const FVector& Destination);
    void TickFight(UBehaviorTreeComponent& OwnerComp,ADeliveryNpcCharacter* Npc,const FDeliveryNpcPublicState& State);
    void TickFlee(UBehaviorTreeComponent& OwnerComp,ADeliveryNpcCharacter* Npc,const FDeliveryNpcPublicState& State);
    void FinishFight(UBehaviorTreeComponent& OwnerComp,FName Reason,bool bFailed);
    // 每个 NPC 使用自己的任务实例，动作编号和计时不会与其他 NPC 共用
    TWeakObjectPtr<UDeliveryNpcAgentComponent> Agent;
    TWeakObjectPtr<AAIController> Controller;
    TWeakObjectPtr<AActor> FocusTarget;
    FGuid ActionId;
    double FinishAt = 0;
    double StrikeFinishAt = 0;
    bool bStrikeStarted = false;
    FVector WalkOrigin=FVector::ZeroVector;
    FVector WalkDestination=FVector::ZeroVector;
    bool bReturningToOrigin=false;
    FGuid FightPunchId;
    bool bFightMoveRequested=false;
    double NextFleePathUpdate=0;
    float FleeTravelled=0;
    FVector PreviousFleePosition=FVector::ZeroVector;
    FVector PreviousThreatPosition=FVector::ZeroVector;
    bool bReportedFlee=false;
};
