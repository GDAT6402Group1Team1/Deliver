#include "Interaction/DeliveryInteractionRegistry.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "GameFramework/Pawn.h"

// 拒绝其他世界的组件，避免误把客户端对象登记到服务器世界
void UDeliveryInteractionRegistry::Register(UDeliveryInteractableComponent* Component)
{
    if (IsValid(Component) && Component->GetWorld() == GetWorld())
        Entries.AddUnique(Component);
}

void UDeliveryInteractionRegistry::Unregister(UDeliveryInteractableComponent* Component)
{
    Entries.Remove(Component);
}

void UDeliveryInteractionRegistry::Deinitialize()
{
    Entries.Reset();
    Super::Deinitialize();
}

void UDeliveryInteractionRegistry::GetCandidates(const APawn* Seeker, EDeliveryInteractionKey Key,
    TArray<UDeliveryInteractableComponent*>& OutCandidates)
{
    OutCandidates.Reset();
    if (!IsValid(Seeker) || Seeker->GetWorld() != GetWorld())
        return;

    for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
    {
        UDeliveryInteractableComponent* Candidate = Entries[Index].Get();
        if (!Candidate)
        {
            // 正常结束时会注销，这里清除意外失效的弱引用
            Entries.RemoveAtSwap(Index);
            continue;
        }
        if (Candidate->InteractionKey == Key && Candidate->CanInteract(Seeker))
            OutCandidates.Add(Candidate);
    }
}

// 最近目标按实际交互位置判断，与距离限制保持一致
UDeliveryInteractableComponent* UDeliveryInteractionRegistry::FindNearest(
    const APawn* Seeker, EDeliveryInteractionKey Key)
{
    TArray<UDeliveryInteractableComponent*> Candidates;
    GetCandidates(Seeker, Key, Candidates);
    UDeliveryInteractableComponent* Best = nullptr;
    float BestDistance = TNumericLimits<float>::Max();
    for (UDeliveryInteractableComponent* Candidate : Candidates)
    {
        const float Distance = Candidate->GetInteractionDistance(Seeker);
        if (Distance >= 0.f && Distance < BestDistance)
        {
            Best = Candidate;
            BestDistance = Distance;
        }
    }
    return Best;
}
