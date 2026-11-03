// Copyright Epic Games, Inc. All Rights Reserved.

#include "Task/World/DeliveryTargetComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Carry/DeliveryCarryComponent.h"

#include "Character/DeliveryCharacter.h"
#include "Delivery.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "World/DeliveryLocationComponent.h"
#include "GameFramework/Actor.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"

UDeliveryTargetComponent::UDeliveryTargetComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

// 将交付点接入通用交互流程
void UDeliveryTargetComponent::BeginPlay()
{
    Super::BeginPlay();

    EnsureInteractableConfigured();
}

// 准备交互组件并统一提示位置、范围和交付行为
void UDeliveryTargetComponent::EnsureInteractableConfigured()
{
    AActor *Owner = GetOwner();
    UDeliveryInteractableComponent *Interactable =
        Owner ? Owner->FindComponentByClass<UDeliveryInteractableComponent>() : nullptr;

    if (!Interactable)
    {
        /**
         * 缺少交互组件时自动创建，确保收件点能被探测和操作
         * 运行时创建的组件不写回关卡，每次进入游戏重新配置
         */
        Interactable = NewObject<UDeliveryInteractableComponent>(
            Owner, UDeliveryInteractableComponent::StaticClass(), TEXT("AutoInteractable"));
        if (!Interactable)
        {
            return;
        }
        // RegisterComponent 在世界已经 BeginPlay 之后会顺带调组件的 BeginPlay，
        // 而注册进交互注册表正是在那里做的——少这一句等于建了个不工作的组件
        Interactable->RegisterComponent();

        UE_LOG(LogDelivery, Log,
               TEXT("[收件点] %s 上没有 DeliveryInteractableComponent，已自动建一个"
                    "（按住 E / 半径 %.0fcm）。想调参数就在那个 Actor 上手加一个，"
                    "手加的优先、这里不会再建。"),
               *GetNameSafe(Owner), Interactable->InteractRadius);
    }

    /**
     * 未手动指定交互起点时，使用地点组件的位置
     * 导航和交互因此都指向同一位置，避免从建筑中心计算距离
     */
    if (!Interactable->InteractAnchor)
    {
        if (UDeliveryLocationComponent *Location = Owner->FindComponentByClass<UDeliveryLocationComponent>())
        {
            Interactable->InteractAnchor = Location;
        }
    }

    // 半径总是以本组件为准，自动建的和手加的都盖掉
    // 这样"怎么调半径"只有一个答案，不用先分清那个可交互组件是谁建的
    Interactable->CanUse.BindUObject(this, &UDeliveryTargetComponent::CanDeliver);
    Interactable->Use.BindUObject(this, &UDeliveryTargetComponent::Deliver);
    Interactable->InteractRadius = InteractRadius;

    const bool bDefaultKey = Interactable->InteractionKey == EDeliveryInteractionKey::GeneralF;
    const bool bNoHold = Interactable->HoldDuration <= KINDA_SMALL_NUMBER;

    if (bDefaultKey)
    {
        Interactable->InteractionKey = EDeliveryInteractionKey::PickupE;
    }
    if (bNoHold)
    {
        Interactable->HoldDuration = 0.5f;
    }
    if (Interactable->PromptText.IsEmpty())
    {
        Interactable->PromptText = NSLOCTEXT("DeliveryPickup", "HoldDeliverPackage", "[按住 E] 交付快递");
    }

    // 提示半径和交付半径必须一致，否则"提示出来了但按不动"
    if (!FMath::IsNearlyEqual(DeliveryRadius, Interactable->InteractRadius))
    {
        UE_LOG(LogDelivery, Warning,
               TEXT("[收件点] %s 的交付半径 %.0f 和提示半径 %.0f 不一致，已对齐到 %.0f。"
                    "不一致的表现是「提示出来了但按不动」。"),
               *GetNameSafe(Owner), DeliveryRadius, Interactable->InteractRadius,
               Interactable->InteractRadius);
        DeliveryRadius = Interactable->InteractRadius;
    }

    if (bDefaultKey || bNoHold)
    {
        UE_LOG(LogDelivery, Log, TEXT("[收件点] %s 的可交互组件还是默认值，已自动改成 按住 E / %.2f 秒。"),
               *GetNameSafe(Owner), Interactable->HoldDuration);
    }
}

// 检查快递任务身份、配送状态和交付距离
bool UDeliveryTargetComponent::CanAcceptDelivery(AActor *ItemActor, APlayerState * /* 未使用 */) const
{
    if (!ExpectedTask || !ItemActor || !GetOwner())
    {
        return false;
    }

    // 必须是这个任务的快递，不能拿别的包裹来交差
    const UDeliveryItemComponent *Item = ItemActor->FindComponentByClass<UDeliveryItemComponent>();
    if (!Item || Item->OwningTask != ExpectedTask)
    {
        return false;
    }

    const UDeliveryTaskManagerComponent *Manager = UDeliveryTaskManagerComponent::Get(this);
    if (!Manager || Manager->GetTaskStatus(ExpectedTask) != EDeliveryTaskStatus::InProgress)
    {
        return false;
    }

    // 判距离用快递本身的位置：它此刻在谁手上、在不在车里都由交互系统决定
    // 收件点那一侧取 GetDeliveryPoint——和提示/按键判定同一个点，不是 Actor 原点
    const FVector ItemLocation = ItemActor->GetActorLocation();

    return FVector::Dist(ItemLocation, GetDeliveryPoint(ItemLocation)) <= DeliveryRadius;
}

// 取得交付判定使用的位置，与交互提示保持一致
FVector UDeliveryTargetComponent::GetDeliveryPoint(FVector FromLocation) const
{
    const AActor *Owner = GetOwner();
    if (!Owner)
    {
        return FromLocation;
    }

    // 交给可交互组件算，而不是在这里再实现一遍"锚点 > 碰撞表面 > 原点"
    // 两处各写一份迟早分叉，而分叉的表现正是这个函数要防的那个 bug
    if (const UDeliveryInteractableComponent *Interactable =
            Owner->FindComponentByClass<UDeliveryInteractableComponent>())
    {
        return Interactable->GetInteractionPoint(FromLocation);
    }

    return Owner->GetActorLocation();
}

// 完成任务后解除快递携带关系，再销毁已交付物品
bool UDeliveryTargetComponent::TryDeliver(AActor *ItemActor, APlayerState *Player)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !CanAcceptDelivery(ItemActor, Player))
    {
        return false;
    }

    UDeliveryTaskManagerComponent *Manager = UDeliveryTaskManagerComponent::Get(this);
    if (!Manager || !Manager->TryCompleteDelivery(ExpectedTask, Player))
    {
        return false;
    }

    if (ADeliveryPackageItem *Package = Cast<ADeliveryPackageItem>(ItemActor))
    {
        ADeliveryCharacter *Holder = Cast<ADeliveryCharacter>(Package->GetOwner());
        if (const ADeliveryMotorbike *Vehicle = Cast<ADeliveryMotorbike>(Package->GetOwner()))
        {
            Holder = Vehicle->GetDriver();
        }
        if (Holder)
            Holder->GetCarryComponent()->ReleasePackage(Package);
        else if (ADeliveryMotorbike *Vehicle = Cast<ADeliveryMotorbike>(Package->GetOwner()))
            Vehicle->ReleaseCargo();
    }
    else if (ADeliveryHandheldItem *Handheld = Cast<ADeliveryHandheldItem>(ItemActor))
    {
        // 快递此刻挂在谁名下，就从那边摘干净再销毁
        // 必须摘：直接 Destroy 的话持有方还留着一个指向已销毁 Actor 的指针，
        // `HasCargo()` / 背包槽位会继续报"有货"，下一件快递就领不到了
        if (ADeliveryCharacter *Holder = Cast<ADeliveryCharacter>(Handheld->GetOwner()))
        {
            if (UDeliveryInventoryComponent *Inventory = Holder->GetInventoryComponent())
            {
                Inventory->ConsumeHeldItem(Handheld);
            }
        }
        else if (ADeliveryMotorbike *Bike = Cast<ADeliveryMotorbike>(Handheld->GetOwner()))
        {
            Bike->ReleaseCargo();
        }
    }

    ItemActor->Destroy();

    return true;
}

// 向交互流程提供当前玩家是否能够交付
bool UDeliveryTargetComponent::CanDeliver(APawn *Interactor, bool bCheckTaskAvailability) const
{
    const ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(Interactor);
    return Character &&
           CanAcceptDelivery(Character->GetCarriedTaskPackage(), Character->GetTaskPlayerState());
}

// 执行交付，成功后通知玩家播放反馈
bool UDeliveryTargetComponent::Deliver(APawn *Interactor)
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(Interactor);
    if (!Character || !TryDeliver(Character->GetCarriedTaskPackage(), Character->GetTaskPlayerState()))
    {
        return false;
    }
    Character->ClientDeliverySucceeded();
    return true;
}
