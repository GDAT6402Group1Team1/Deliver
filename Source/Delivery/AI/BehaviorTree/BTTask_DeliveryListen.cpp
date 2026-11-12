#include "AI/BehaviorTree/BTTask_DeliveryListen.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
namespace { constexpr EAIFocusPriority::Type ListeningFocus=EAIFocusPriority::LastFocusPriority+2; }

UBTTask_DeliveryListen::UBTTask_DeliveryListen()
{
    NodeName=TEXT("倾听玩家"); bCreateNodeInstance=true; bNotifyTick=true; bNotifyTaskFinished=true;
}
EBTNodeResult::Type UBTTask_DeliveryListen::ExecuteTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory)
{
    Controller=OwnerComp.GetAIOwner();
    auto* Npc=Controller.IsValid()?Cast<ADeliveryNpcCharacter>(Controller->GetPawn()):nullptr;
    if (!Npc || !Npc->Agent->IsListening()) return EBTNodeResult::Failed;
    Controller->StopMovement();
    Controller->SetFocus(Npc->Agent->GetListeningSpeaker(),ListeningFocus);
    return EBTNodeResult::InProgress;
}
void UBTTask_DeliveryListen::TickTask(UBehaviorTreeComponent& OwnerComp,uint8* Memory,float DeltaSeconds)
{
    auto* Npc=Controller.IsValid()?Cast<ADeliveryNpcCharacter>(Controller->GetPawn()):nullptr;
    if (!Npc || Npc->IsKnockedDown() || !Npc->Agent->IsListening()) FinishLatentTask(OwnerComp,EBTNodeResult::Succeeded);
    else Controller->SetFocus(Npc->Agent->GetListeningSpeaker(),ListeningFocus);
}
void UBTTask_DeliveryListen::OnTaskFinished(UBehaviorTreeComponent& OwnerComp,uint8* Memory,EBTNodeResult::Type Result)
{
    if (Controller.IsValid()) Controller->ClearFocus(ListeningFocus);
    Controller.Reset(); Super::OnTaskFinished(OwnerComp,Memory,Result);
}
