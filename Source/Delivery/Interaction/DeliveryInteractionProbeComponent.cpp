// Copyright Epic Games, Inc. All Rights Reserved.

#include "Interaction/DeliveryInteractionProbeComponent.h"
#include "UI/Controllers/DeliveryInteractionController.h"
#include "Components/PrimitiveComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Camera/CameraComponent.h"
#include "Interaction/DeliveryInteractionGeometry.h"
#include "Interaction/DeliveryInteractionRegistry.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/World/DeliveryPickupPointComponent.h"
#include "Vehicle/DeliveryMotorbike.h"

UDeliveryInteractionProbeComponent::UDeliveryInteractionProbeComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    // 20Hz 更新范围内的 E 目标
    PrimaryComponentTick.TickInterval = 0.05f;
    SetIsReplicatedByDefault(false);
}

// 仅为本机玩家更新交互目标、提示和高亮
void UDeliveryInteractionProbeComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                                       FActorComponentTickFunction *ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    const APawn *Owner = Cast<APawn>(GetOwner());
    const ADeliveryCharacter *OwnerCharacter = Cast<ADeliveryCharacter>(Owner);

    /**
     * 本机控制的角色或本机车辆的驾驶员才执行探测
     * 骑车时控制器控制车辆，角色自身的本机控制标记会变为假
     */
    const ADeliveryMotorbike *RiddenBike = OwnerCharacter ? OwnerCharacter->GetCurrentVehicle() : nullptr;
    const bool bLocallyDriving = RiddenBike && RiddenBike->IsLocallyControlled();

    if (!Owner || (!Owner->IsLocallyControlled() && !bLocallyDriving))
    {
        FocusedGeneral = nullptr;
        FocusedPickup = nullptr;
        RestorePickupHighlight();
        return;
    }

    // 晕倒后不能交互，同时清除选中目标和旧的高亮
    const ADeliveryCharacter *Character = OwnerCharacter;
    if (Character && Character->IsIncapacitated())
    {
        FocusedGeneral = nullptr;
        FocusedPickup = nullptr;
        RestorePickupHighlight();
        return;
    }

    // 分别查找拾取键和交互键的目标，两个按键可以对应不同物件
    UDeliveryInteractableComponent *Pickup = FindPreferredPickup(Owner);
    UDeliveryInteractableComponent *General =
        GetWorld()->GetSubsystem<UDeliveryInteractionRegistry>()->FindNearest(Owner, EDeliveryInteractionKey::GeneralF);
    FocusedPickup = Pickup;
    FocusedGeneral = General;
    SetPickupHighlight(Pickup ? Pickup->GetOwner() : nullptr);

    // 同时存在两个目标时，屏幕提示优先显示拾取操作
    UDeliveryInteractableComponent *PromptTarget = Pickup ? Pickup : General;
    if (bShowPrompt && PromptTarget)
    {
        if (UDeliveryPromptSubsystem *Prompt = UDeliveryPromptSubsystem::Get(this))
        {
            if (!WidgetController)
                WidgetController = NewObject<UDeliveryInteractionController>(this);
            const float Progress =
                Pickup ? WidgetController->GetProgress(Character, Pickup->GetOwner()) : -1.f;
            const FText Text = WidgetController->GetPrompt(PromptTarget, Pickup != nullptr);
            Prompt->PushPrompt(Text, PromptTarget->GetPromptLocation(), Progress);
        }
    }
}

AActor *UDeliveryInteractionProbeComponent::GetFocusedActor() const
{
    const UDeliveryInteractableComponent *Target = FocusedGeneral.Get();
    return Target ? Target->GetOwner() : nullptr;
}

AActor *UDeliveryInteractionProbeComponent::GetFocusedPickupActor() const
{
    const UDeliveryInteractableComponent *Target = FocusedPickup.Get();
    return Target ? Target->GetOwner() : nullptr;
}

// 优先选择准星附近的可见目标，再考虑距离
UDeliveryInteractableComponent *UDeliveryInteractionProbeComponent::FindPreferredPickup(
    const APawn *Owner) const
{
    const ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(Owner);
    if (!Character)
        return nullptr;

    TArray<UDeliveryInteractableComponent *> Candidates;
    GetWorld()->GetSubsystem<UDeliveryInteractionRegistry>()->GetCandidates(Owner, EDeliveryInteractionKey::PickupE, Candidates);
    UDeliveryInteractableComponent *Best = nullptr;
    float BestDistance = TNumericLimits<float>::Max();
    bool bBestUsable = false;
    bool bBestAimed = false;
    float BestMiss = TNumericLimits<float>::Max();
    const UCameraComponent *Camera = Character->GetFollowCamera();
    for (UDeliveryInteractableComponent *Candidate : Candidates)
    {
        AActor *Target = Candidate->GetOwner();
        if (!Character->CanUsePickupTarget(Target, false))
            continue;
        const bool bUsable = Character->CanUsePickupTarget(Target);
        // 长按期间，只要目标仍有效且在范围内，就保持当前目标
        if (bUsable && Character->GetPickupHoldProgress(Target) >= 0.0f)
            return Candidate;
        // 排序与范围检查使用相同的交互距离，门口锚点不会被模型原点影响
        const float Distance = Candidate->GetInteractionDistance(Owner);
        // 不可领取的快递仍可显示原因提示
        // 但不能挡住其他可以操作的取货点或收件点
        FVector Center, Extent;
        Target->GetActorBounds(true, Center, Extent);
        const float Miss =
            Camera
                ? DeliveryInteractionGeometry::AimMissDistance(Camera->GetComponentLocation(),
                                                               Camera->GetForwardVector().GetSafeNormal(),
                                                               Center + FVector::UpVector * (Extent.Z * 0.5f))
                : -1.0f;
        const float Allowance =
            35.0f + FMath::Min(Extent.Size(), 25.0f) + (FocusedPickup.Get() == Candidate ? 15.0f : 0.0f);
        const bool bAimed = Miss >= 0.0f && Miss <= Allowance;
        // 准星只用于候选排序，不因目标偏离准星而禁止操作
        const bool bBetterRank =
            bAimed != bBestAimed ? bAimed
                                 : (bAimed && Miss != BestMiss ? Miss < BestMiss : Distance < BestDistance);
        if (!Best || (bUsable && !bBestUsable) || (bUsable == bBestUsable && bBetterRank))
        {
            Best = Candidate;
            BestDistance = Distance;
            bBestUsable = bUsable;
            bBestAimed = bAimed;
            BestMiss = Miss;
        }
    }
    return Best;
}

// 保存目标原显示设置，再开启交互高亮
void UDeliveryInteractionProbeComponent::SetPickupHighlight(AActor *Actor)
{
    if (HighlightedActor.Get() == Actor)
        return;
    RestorePickupHighlight();
    HighlightedActor = Actor;
    if (!Actor)
        return;

    TArray<UPrimitiveComponent *> Primitives;
    Actor->GetComponents(Primitives);
    for (UPrimitiveComponent *Primitive : Primitives)
    {
        if (!Primitive || !Primitive->IsVisible())
            continue;
        FPrimitiveHighlightState &State = HighlightStates.AddDefaulted_GetRef();
        State.Component = Primitive;
        State.bRenderCustomDepth = Primitive->bRenderCustomDepth;
        State.StencilValue = Primitive->CustomDepthStencilValue;
        Primitive->SetRenderCustomDepth(true);
        Primitive->SetCustomDepthStencilValue(1);
    }
}

// 恢复旧目标的描边和深度设置
void UDeliveryInteractionProbeComponent::RestorePickupHighlight()
{
    for (const FPrimitiveHighlightState &State : HighlightStates)
    {
        if (UPrimitiveComponent *Primitive = State.Component.Get())
        {
            Primitive->SetRenderCustomDepth(State.bRenderCustomDepth);
            Primitive->SetCustomDepthStencilValue(State.StencilValue);
        }
    }
    HighlightStates.Reset();
    HighlightedActor.Reset();
}

void UDeliveryInteractionProbeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    RestorePickupHighlight();
    Super::EndPlay(EndPlayReason);
}
