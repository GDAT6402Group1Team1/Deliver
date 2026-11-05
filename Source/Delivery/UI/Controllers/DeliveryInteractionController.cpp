#include "UI/Controllers/DeliveryInteractionController.h"
#include "Character/DeliveryCharacter.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Task/World/DeliveryPickupPointComponent.h"
#include "Task/World/DeliveryItemComponent.h"

// 优先显示目标无法操作的原因，否则使用目标配置的提示
FText UDeliveryInteractionController::GetPrompt(UDeliveryInteractableComponent *Target, bool bPickup,
                                                const APawn *Interactor) const
{
    if (!IsValid(Target))
        return FText::GetEmpty();
    if (bPickup && Target->GetOwner())
    {
        FText Reason;
        if (const UDeliveryPickupPointComponent *Point =
                UDeliveryPickupPointComponent::FindOn(Target->GetOwner()))
            Reason = Point->GetBlockedReason();
        else if (const UDeliveryItemComponent *Item =
                     Target->GetOwner()->FindComponentByClass<UDeliveryItemComponent>())
            Reason = Item->GetPickupBlockedReason();
        if (!Reason.IsEmpty())
            return Reason;
    }
    return Target->PromptText;
}

// 查询角色当前对指定目标的长按进度
float UDeliveryInteractionController::GetProgress(const ADeliveryCharacter *Character, AActor *Target) const
{
    return IsValid(Character) ? Character->GetPickupHoldProgress(Target) : -1.f;
}
