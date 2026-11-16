#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_DeliveryExecuteNpcAction.generated.h"

class UDeliveryNpcAgentComponent;
class AAIController;
class ADeliveryNpcCharacter;
struct FDeliveryNpcPublicState;

/**
 * @brief 行为树里执行 Agent 已采纳的动作，做完把结果交回 Agent
 *
 * 开始：ExecuteTask 调 Agent 的 StartAction 领动作，再调角色的 ExecuteAction 开始做
 * 持续：TickTask 每帧检查动作做完没有；交战转到 TickFight 判断停手、走近还是出拳，逃跑转到 TickFlee
 * 结束：OnTaskFinished 调 Agent 的 TryFinishAction 交回成功、失败或取消
 * 等模型回答不在这里，这个节点只管做
 */
UCLASS()
class DELIVERY_API UBTTask_DeliveryExecuteNpcAction : public UBTTaskNode
{
    GENERATED_BODY()
public:
    UBTTask_DeliveryExecuteNpcAction();

    // ── 执行 ──
    // 行为树依次调用：ExecuteTask 开始 → TickTask 每帧 → AbortTask 被打断 → OnTaskFinished 交回结果

    /// @brief 领取当前动作并开始做
    /// @return 动作很快能做完时直接返回结果，需要时间时返回 InProgress
    virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;

    /// @brief 行为树要切到别的分支，停下当前动作
    virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;

    /// @brief 每帧检查动作进度，交战和逃跑在这里逐帧推进
    virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;

    /// @brief 节点结束时把结果交回 Agent，撤掉朝向
    virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) override;

private:
    // ── 执行 ──
    // 交战、逃跑、踱步每帧要做的事

    /// 撤掉看向；bReplacingOwnAction 时是同一 NPC 换动作，不清其他朝向
    void ReleaseFocus(bool bReplacingOwnAction=false /* true 表示同一 NPC 马上换下一个动作 */);

    /// 走到附近的点，二狗跟随时按 CompanionPace 调速度
    bool StartNearbyMove(const FVector& Destination,const FDeliveryNpcPublicState* CompanionPace=nullptr);

    /// 交战每帧：该停就停，够不着就走近，冷却过了就出拳
    void TickFight(UBehaviorTreeComponent& OwnerComp,ADeliveryNpcCharacter* Npc,const FDeliveryNpcPublicState& State);

    /// 逃跑每帧：离威胁够远就停，否则更新逃跑路线
    void TickFlee(UBehaviorTreeComponent& OwnerComp,ADeliveryNpcCharacter* Npc,const FDeliveryNpcPublicState& State);

    /// 结束交战，写下原因
    void FinishFight(UBehaviorTreeComponent& OwnerComp,FName Reason,bool bFailed /* true 按失败结束 */);

    // ── 执行 ──
    // 每个 NPC 有自己的节点实例，动作编号和计时不会和别人混

    /// 这位 NPC 的 Agent
    TWeakObjectPtr<UDeliveryNpcAgentComponent> Agent;

    /// 这位 NPC 的 AI 控制器
    TWeakObjectPtr<AAIController> Controller;

    /// 当前看向的对象
    TWeakObjectPtr<AActor> FocusTarget;

    /// 正在执行的动作编号
    FGuid ActionId;

    /// 动作最晚结束时间
    double FinishAt = 0;

    /// 这一拳最晚结束时间
    double StrikeFinishAt = 0;

    /// 这一拳已经挥出去
    bool bStrikeStarted = false;

    /// 二狗边说话边继续原来的跟随或跑腿
    bool bKeepsCompanionMovement=false;

    /// 踱步的起点
    FVector WalkOrigin=FVector::ZeroVector;

    /// 踱步的终点
    FVector WalkDestination=FVector::ZeroVector;

    /// 踱步正在往回走
    bool bReturningToOrigin=false;

    // ── 交战 ──

    /// 当前这一拳的编号，和战斗组件、日志里的 punch= 是同一个
    FGuid FightPunchId;

    /// 已经发出走近对手的移动
    bool bFightMoveRequested=false;

    // ── 逃跑 ──

    /// 下次更新逃跑路线的时间
    double NextFleePathUpdate=0;

    /// 已经跑了多远
    float FleeTravelled=0;

    /// 上一帧自己的位置
    FVector PreviousFleePosition=FVector::ZeroVector;

    /// 上一帧威胁的位置
    FVector PreviousThreatPosition=FVector::ZeroVector;

    /// 已经把逃跑结果报过
    bool bReportedFlee=false;
};
