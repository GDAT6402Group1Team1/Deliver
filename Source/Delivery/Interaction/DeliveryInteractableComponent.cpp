// Copyright Epic Games, Inc. All Rights Reserved.

#include "Interaction/DeliveryInteractableComponent.h"
#include "GameFramework/Pawn.h"
#include "Interaction/DeliveryInteractionRegistry.h"
#include "Engine/World.h"
#include "Components/PrimitiveComponent.h"


UDeliveryInteractableComponent::UDeliveryInteractableComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    SetIsReplicatedByDefault(false);
}

void UDeliveryInteractableComponent::BeginPlay()
{
    Super::BeginPlay();
    if (UDeliveryInteractionRegistry* Registry = GetWorld()->GetSubsystem<UDeliveryInteractionRegistry>())
        Registry->Register(this);
}

void UDeliveryInteractableComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UDeliveryInteractionRegistry* Registry = GetWorld()->GetSubsystem<UDeliveryInteractionRegistry>())
        Registry->Unregister(this);
    Super::EndPlay(EndPlayReason);
}

// 检查交互开关、操作对象和距离
bool UDeliveryInteractableComponent::CanInteract(const APawn *Interactor) const
{
    if (!bInteractEnabled || !Interactor || !GetOwner())
    {
        return false;
    }
    if (Interactor == GetOwner())
    {
        return false;
    }
    const float Distance = GetInteractionDistance(Interactor);

    return Distance >= 0.0f && Distance <= InteractRadius;
}

// 优先使用配置的交互位置，物品可使用碰撞表面作为距离起点
FVector UDeliveryInteractableComponent::GetInteractionPoint(const FVector &FromLocation) const
{
    const AActor *Owner = GetOwner();
    if (!Owner)
    {
        return FromLocation;
    }

    // 锚点优先；挂在整栋楼上时 Actor 原点可能在楼心/地下，而大型网格用
    // "Use Complex Collision As Simple" 时连简单碰撞体都没有、下面那个查询也会失败，
    // 于是"箭头指门口、判定从楼心算"；锚点是唯一能让两者统一的办法
    if (const USceneComponent *Anchor = InteractAnchor.Get())
    {
        return Anchor->GetComponentLocation();
    }

    // 地面物品按碰撞表面计算拾取距离
    // 角色原点位于髋部，直接从原点测量会把高度差计入距离
    // 这会导致脚边的物品也被误判为距离过远
    if (InteractionKey == EDeliveryInteractionKey::PickupE)
    {
        if (const UPrimitiveComponent *Primitive = Cast<UPrimitiveComponent>(Owner->GetRootComponent()))
        {
            FVector Closest;
            if (Primitive->GetClosestPointOnCollision(FromLocation, Closest) > 0.0f)
            {
                return Closest;
            }
        }
    }

    return Owner->GetActorLocation();
}

// 计算角色到交互位置的距离，避免模型原点造成偏差
float UDeliveryInteractableComponent::GetInteractionDistance(const APawn *Interactor) const
{
    if (!Interactor || !GetOwner())
    {
        return -1.0f;
    }

    const FVector InteractionPoint = GetInteractionPoint(Interactor->GetActorLocation());
    FVector InteractorPoint = Interactor->GetActorLocation();

    // 玩家这一侧也取碰撞表面：从胶囊原点（骨盆）算的话，脚边的东西也会显得一米开外
    if (InteractionKey == EDeliveryInteractionKey::PickupE)
    {
        if (const UPrimitiveComponent *InteractorPrimitive =
                Cast<UPrimitiveComponent>(Interactor->GetRootComponent()))
        {
            FVector Closest;
            if (InteractorPrimitive->GetClosestPointOnCollision(InteractionPoint, Closest) > 0.0f)
            {
                InteractorPoint = Closest;
            }
        }
    }

    return FVector::Dist(InteractorPoint, InteractionPoint);
}

void UDeliveryInteractableComponent::Execute(APawn *Interactor)
{
    OnInteract.Broadcast(Interactor);
}

// 根据交互位置和偏移计算提示显示位置
FVector UDeliveryInteractableComponent::GetPromptLocation() const
{
    const AActor *Owner = GetOwner();
    if (!Owner)
    {
        return PromptOffset;
    }

    // 浮窗也吊在锚点上方，不是 Actor 原点上方
    // 否则会出现"提示飘在楼中间、能按的地方在门口"——玩家会走向提示那边
    const FVector Base =
        InteractAnchor.Get() ? InteractAnchor->GetComponentLocation() : Owner->GetActorLocation();

    return Base + PromptOffset;
}

UDeliveryInteractableComponent *UDeliveryInteractableComponent::FindOn(const AActor *Actor)
{
    return Actor ? Actor->FindComponentByClass<UDeliveryInteractableComponent>() : nullptr;
}
