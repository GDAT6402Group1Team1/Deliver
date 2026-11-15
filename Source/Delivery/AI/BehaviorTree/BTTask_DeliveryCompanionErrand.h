#pragma once
#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_DeliveryCompanionErrand.generated.h"
class UDeliveryCompanionComponent;

// 持续推进主人已确认的委托，树中断时只停止本次导航，事务仍由同伴组件维护
UCLASS()
class DELIVERY_API UBTTask_DeliveryCompanionErrand : public UBTTaskNode
{
    GENERATED_BODY()
public:
    UBTTask_DeliveryCompanionErrand();
    // 搜索子树只推进寻找阶段，找到后由普通委托分支完成拾取和交还
    UPROPERTY(EditAnywhere,Category="委托") bool bSearchStageOnly=false;
    virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory) override;
    virtual void TickTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory,float DeltaSeconds) override;
    virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory) override;
    virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp,uint8* NodeMemory,EBTNodeResult::Type Result) override;
private:
    bool CanRunStage() const;
    void StopUncontinuedMovement();
    TWeakObjectPtr<UDeliveryCompanionComponent> Companion;
    int32 CommandRevision=INDEX_NONE;
};
