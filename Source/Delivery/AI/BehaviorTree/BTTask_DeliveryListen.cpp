#include "AI/BehaviorTree/BTTask_DeliveryListen.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"

// ── 倾听 ──
// 玩家对 NPC 说话时，Agent 的 ShouldPauseForConversation 返回 true，行为树进入这个节点
// ExecuteTask 让 NPC 停下并调用 SetFocus 看向说话的人；TickTask 每帧更新看向的目标，不再需要倾听或被打倒时结束节点
// OnTaskFinished 调用 ClearFocus，NPC 不再盯着说话的人

namespace
{
/// 倾听用的 SetFocus 优先级，比引擎内置的几级都高；OnTaskFinished 只清掉这一级
constexpr EAIFocusPriority::Type ListeningFocus = EAIFocusPriority::LastFocusPriority + 2;
}

/// @brief 每个 NPC 的行为树各有一份节点实例（bCreateNodeInstance），这样 Controller 成员不会被别的 NPC 改掉；打开 TickTask 和 OnTaskFinished
UBTTask_DeliveryListen::UBTTask_DeliveryListen()
{
    NodeName = TEXT("倾听玩家");
    bCreateNodeInstance = true;
    bNotifyTick = true;
    bNotifyTaskFinished = true;
}

/// @brief 行为树进入节点时调用：ShouldPauseForConversation 为 false 就返回 Failed
/// 否则调用 StopMovement 停下，SetFocus 看向 GetListeningSpeaker，返回 InProgress，之后每帧调用 TickTask
EBTNodeResult::Type UBTTask_DeliveryListen::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* Memory)
{
    Controller = OwnerComp.GetAIOwner();
    auto* Npc = Controller.IsValid() ? Cast<ADeliveryNpcCharacter>(Controller->GetPawn()) : nullptr;
    if (!Npc || !Npc->Agent->ShouldPauseForConversation())
        return EBTNodeResult::Failed;
    Controller->StopMovement();
    Controller->SetFocus(Npc->Agent->GetListeningSpeaker(), ListeningFocus);
    return EBTNodeResult::InProgress;
}

/// @brief 每帧调用：NPC 被打倒或 ShouldPauseForConversation 变成 false 时调用 FinishLatentTask 结束节点；否则继续看向说话的人
void UBTTask_DeliveryListen::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* Memory, float DeltaSeconds)
{
    auto* Npc = Controller.IsValid() ? Cast<ADeliveryNpcCharacter>(Controller->GetPawn()) : nullptr;
    if (!Npc || Npc->IsKnockedDown() || !Npc->Agent->ShouldPauseForConversation())
        FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
    else
        Controller->SetFocus(Npc->Agent->GetListeningSpeaker(), ListeningFocus);
}

/// @brief 节点结束（正常结束或被行为树打断）时调用 ClearFocus 清掉倾听用的看向目标，再清掉 Controller
void UBTTask_DeliveryListen::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* Memory, EBTNodeResult::Type Result)
{
    if (Controller.IsValid())
        Controller->ClearFocus(ListeningFocus);
    Controller.Reset();
    Super::OnTaskFinished(OwnerComp, Memory, Result);
}
