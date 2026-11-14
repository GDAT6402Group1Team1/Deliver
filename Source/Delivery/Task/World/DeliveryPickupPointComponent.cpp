// Copyright Epic Games, Inc. All Rights Reserved.

#include "Task/World/DeliveryPickupPointComponent.h"

#include "Delivery.h"
#include "Carry/DeliveryPackageCarrierInterface.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Task/World/DeliveryItemComponent.h"
#include "World/DeliveryLocationComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Misc/ScopeExit.h"

UDeliveryPickupPointComponent::UDeliveryPickupPointComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

// 将取货点接入通用交互流程
void UDeliveryPickupPointComponent::BeginPlay()
{
    Super::BeginPlay();

    EnsureInteractableConfigured();
}

// 准备交互组件，并配置长按、范围和目标行为
void UDeliveryPickupPointComponent::EnsureInteractableConfigured()
{
    AActor *Owner = GetOwner();
    UDeliveryInteractableComponent *Interactable =
        Owner ? Owner->FindComponentByClass<UDeliveryInteractableComponent>() : nullptr;

    if (!Interactable)
    {
        /**
         * 缺少交互组件时自动创建，确保取货点能被探测和操作
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
               TEXT("[取货点] %s 上没有 DeliveryInteractableComponent，已自动建一个"
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

    // 只纠正还停在默认值的那套；GeneralF + HoldDuration 0 不可能是能用的取货点，
    // 所以改它不会覆盖任何有意的配置
    // 半径总是以本组件为准，自动建的和手加的都盖掉
    // 这样"怎么调半径"只有一个答案，不用先分清那个可交互组件是谁建的
    Interactable->CanUse.BindUObject(this, &UDeliveryPickupPointComponent::CanPickup);
    Interactable->Use.BindUObject(this, &UDeliveryPickupPointComponent::Pickup);
    Interactable->InteractRadius = InteractRadius;

    const bool bDefaultKey = Interactable->InteractionKey == EDeliveryInteractionKey::GeneralF;
    const bool bNoHold = Interactable->HoldDuration <= KINDA_SMALL_NUMBER;

    if (bDefaultKey)
    {
        Interactable->InteractionKey = EDeliveryInteractionKey::PickupE;
    }
    if (bNoHold)
    {
        // 和快递箱子、收件点三处保持一致；玩家的肌肉记忆是"按住 E"
        Interactable->HoldDuration = 0.5f;
    }
    if (Interactable->PromptText.IsEmpty())
    {
        Interactable->PromptText = NSLOCTEXT("DeliveryPickup", "HoldTakePackage", "[Hold E] Collect Package");
    }

    if (bDefaultKey || bNoHold)
    {
        UE_LOG(LogDelivery, Log, TEXT("[取货点] %s 的可交互组件还是默认值，已自动改成 按住 E / %.2f 秒。"),
               *GetNameSafe(Owner), Interactable->HoldDuration);
    }
}

UDeliveryPickupPointComponent *UDeliveryPickupPointComponent::FindOn(const AActor *Actor)
{
    return Actor ? Actor->FindComponentByClass<UDeliveryPickupPointComponent>() : nullptr;
}

// 从地点组件取得这个取货点的编号
FName UDeliveryPickupPointComponent::GetLocationId() const
{
    const AActor *Owner = GetOwner();
    const UDeliveryLocationComponent *Location =
        Owner ? Owner->FindComponentByClass<UDeliveryLocationComponent>() : nullptr;

    return Location ? Location->LocationId : NAME_None;
}

// 寻找属于当前取货点且可领取的任务
UDeliveryTaskDefinition *UDeliveryPickupPointComponent::ResolvePendingTask() const
{
    const UDeliveryTaskManagerComponent *Manager = UDeliveryTaskManagerComponent::Get(this);
    const FName MyId = GetLocationId();
    if (!Manager || MyId.IsNone())
    {
        return nullptr;
    }

    TArray<UDeliveryTaskDefinition *> Awaiting;
    Manager->GetTasksByStatus(EDeliveryTaskStatus::AwaitingPickup, Awaiting);

    for (UDeliveryTaskDefinition *Task : Awaiting)
    {
        if (Task && Task->PickupLocationId == MyId)
        {
            return Task;
        }
    }

    return nullptr;
}

// 检查是否有可发放的任务快递
bool UDeliveryPickupPointComponent::CanDispense() const
{
    const UDeliveryTaskDefinition *Task = ResolvePendingTask();
    const UDeliveryTaskManagerComponent *Manager = UDeliveryTaskManagerComponent::Get(this);

    // 还要过 CanAcquireItem：全世界同时只有一个已接单计时的任务，
    // 别的任务的快递要在这里被拦住（和旧流程捡箱子时是同一道门槛）
    return Task && Manager && Manager->CanAcquireItem(Task);
}

// 提供取货点当前无法发货的原因
FText UDeliveryPickupPointComponent::GetBlockedReason(const APawn *Interactor) const
{
    /**
     * 骑着车停在取货点时，要告诉玩家该做什么，而不是报任务配置问题
     *
     * 判据取"传进来的这个交互者"，不去抓第一个 PlayerController——
     * 同机多人时那样会按别人的骑车状态显示我的提示。
     */
    if (const ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(Interactor))
    {
        if (Character->IsRidingVehicle())
        {
            return NSLOCTEXT("DeliveryPickup", "DismountToTake", "[F] Dismount to Collect Package");
        }
    }

    if (GetLocationId().IsNone())
    {
        return NSLOCTEXT("DeliveryPickup", "PointNoId", "Pickup Location Not Configured");
    }

    const UDeliveryTaskManagerComponent *Manager = UDeliveryTaskManagerComponent::Get(this);
    if (!Manager)
    {
        return NSLOCTEXT("DeliveryPickup", "NoManager", "Delivery Service Unavailable");
    }

    if (const UDeliveryTaskDefinition *Task = ResolvePendingTask())
    {
        if (Manager->CanAcquireItem(Task))
        {
            return FText::GetEmpty();
        }

        // 先分"没接电话"再分"有别的单"：这两种都会让 CanAcquireItem 返回 假，
        // 但玩家该做的事完全不同（去接电话 vs 先送完手上这单）
        if (!Manager->IsTaskAccepted(Task))
        {
            return NSLOCTEXT("DeliveryPickup", "NotAnswered", "Answer the Call to Accept This Delivery");
        }

        return NSLOCTEXT("DeliveryPickup", "OtherTask", "Complete Your Current Delivery First");
    }

    // 没有待取件任务指向这里；分两种情况说，因为玩家的下一步动作完全不同：
    // 手上已经有货 → 该去送；什么都没有 → 该等电话
    if (const UDeliveryTaskDefinition *Active = Manager->GetActiveTask())
    {
        if (Manager->GetTaskStatus(Active) == EDeliveryTaskStatus::InProgress)
        {
            return NSLOCTEXT("DeliveryPickup", "AlreadyCarrying", "Package Collected. Head to the Drop-Off");
        }
    }

    return NSLOCTEXT("DeliveryPickup", "NoPendingHere", "No Packages to Collect Here");
}

// 生成任务快递并交给接收者，失败时清理新建快递
bool UDeliveryPickupPointComponent::TryDispense(ADeliveryCharacter *Receiver)
{
    AActor *Owner = GetOwner();
    UWorld *World = GetWorld();
    if (!Owner || !Owner->HasAuthority() || !IsValid(Receiver) || !World)
    {
        return false;
    }

    UDeliveryTaskDefinition *Task = ResolvePendingTask();
    // 蓝图直接调用发货也必须遵守下车取货的规则
    if (!CanPickup(Receiver, true) || !Task)
    {
        return false;
    }

    UClass *SpawnClass = PackageClass ? PackageClass.Get() : ADeliveryTestPackage::StaticClass();

    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    // 放进根组件的空间算偏移：取货点可能挂在整栋楼上、原点在地下，
    // 用世界 Z 加偏移会把快递生成在半空或地底
    const FTransform Base = Owner->GetActorTransform();
    const FVector SpawnLocation = Base.TransformPosition(SpawnOffset);

    ADeliveryPackageItem *Package =
        World->SpawnActor<ADeliveryPackageItem>(SpawnClass, SpawnLocation, Base.Rotator(), Params);
    if (!Package)
    {
        UE_LOG(LogDelivery, Warning, TEXT("[取货点] %s 生成快递失败（类 %s）"), *Owner->GetName(),
               *GetNameSafe(SpawnClass));

        return false;
    }

    // 先把任务绑上，再交给玩家；顺序不能反：ReceiveTaskPackage 里会调
    // NotifyAcquired 让任务进入进行中，那时候 OwningTask 必须已经是对的
    if (UDeliveryItemComponent *Item = Package->GetDeliveryItemComponent())
    {
        Item->OwningTask = Task;
    }

    if (!Receiver->ReceiveTaskPackage(Package))
    {
        // 没人收得下就别把它留在世界里当野快递：玩家会以为捡得起来，
        // 而它身上的 OwningTask 已经指向当前任务，捡起来会走到另一条路上去
        Package->Destroy();

        return false;
    }

    UE_LOG(LogDelivery, Log, TEXT("[取货点] %s 发出任务 %s 的快递给 %s"), *Owner->GetName(),
           *Task->TaskId.ToString(), *Receiver->GetName());

    return true;
}

bool UDeliveryPickupPointComponent::TryDispenseToCarrier(APawn* Receiver, UDeliveryTaskDefinition* ExpectedTask)
{
    AActor* Point = GetOwner();
    UWorld* World = GetWorld();
    auto* Carrier = IsValid(Receiver) ? Cast<IDeliveryPackageCarrierInterface>(Receiver) : nullptr;
    APlayerState* Player = Carrier ? Carrier->GetTaskPlayerState() : nullptr;
    auto* Interaction = UDeliveryInteractableComponent::FindOn(Point, EDeliveryInteractionKey::PickupE);
    const auto* Manager = UDeliveryTaskManagerComponent::Get(this);
    if (!IsValid(Point) || !Point->HasAuthority() || !World || !IsValid(Receiver) ||
        !Receiver->HasAuthority() || Receiver->GetWorld() != World || !Carrier ||
        !Carrier->CanHandleTaskPackage() || Carrier->GetCarriedTaskPackage() ||
        !IsValid(Player) || Player->GetWorld() != World || Player->IsInactive() ||
        !IsValid(ExpectedTask) || !Manager || !Manager->IsTaskAccepted(ExpectedTask) ||
        ResolvePendingTask() != ExpectedTask || !Manager->CanAcquireItem(ExpectedTask) ||
        !Interaction || !Interaction->CanInteract(Receiver))
    {
        return false;
    }

    ADeliveryPackageItem* Package = nullptr;
    // 距离和射线都指向原取货锚点，整栋建筑的其他墙面仍会挡住取件
    const auto IsAtPickupPoint = [&]()
    {
        if (!IsValid(Point) || !IsValid(Receiver) || !IsValid(Interaction) ||
            Point->GetWorld() != World || Receiver->GetWorld() != World || !Interaction->CanInteract(Receiver)) return false;
        const FVector PickupPoint = Interaction->GetInteractionPoint(Receiver->GetActorLocation());
        FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionPackagePickup), false, Receiver);
        if (IsValid(Package)) Query.AddIgnoredActor(Package);
        FCollisionObjectQueryParams Objects;
        for (const auto Channel : {ECC_WorldStatic, ECC_WorldDynamic, ECC_Pawn, ECC_Vehicle, ECC_PhysicsBody})
            Objects.AddObjectTypesToQuery(Channel);
        FHitResult Hit;
        // 锚点恰好位于取件表面时只容许末端两厘米，不能忽略整栋楼的遮挡
        return !World->LineTraceSingleByObjectType(Hit, Receiver->GetActorLocation(), PickupPoint, Objects, Query) ||
            FVector::DistSquared(Hit.ImpactPoint, PickupPoint) <= FMath::Square(2.f);
    };
    if (!IsAtPickupPoint()) return false;

    UClass* SpawnClass = PackageClass ? PackageClass.Get() : ADeliveryTestPackage::StaticClass();
    const FTransform Base = Point->GetActorTransform();
    const FTransform SpawnTransform(Base.GetRotation(), Base.TransformPosition(SpawnOffset));
    Package = World->SpawnActorDeferred<ADeliveryPackageItem>(
        SpawnClass, SpawnTransform, nullptr, Receiver, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if (!Package) return false;
    auto* Item = Package->GetDeliveryItemComponent();
    if (!Item)
    {
        Package->Destroy();
        return false;
    }

    // 未交出的新物品不是丢件，失败销毁不能把其他人的进行中任务退回待取件
    const bool bReportLost = Item->bReportLostOnDestroy;
    Item->bReportLostOnDestroy = false;
    Item->OwningTask = ExpectedTask;
    if (!Item->BeginTransfer(Receiver))
    {
        Package->Destroy();
        return false;
    }
    const TWeakObjectPtr<UDeliveryItemComponent> ReservedItem = Item;
    ON_SCOPE_EXIT
    {
        if (ReservedItem.IsValid()) ReservedItem->EndTransfer();
    };
    Package->FinishSpawning(SpawnTransform);

    const auto RejectPackage = [&]()
    {
        if (!IsValid(Package)) return false;
        auto* CurrentItem = Package->GetDeliveryItemComponent();
        if (Package->GetOwner())
        {
            // 预约期间不应换手，若蓝图回调仍改了归属则保留实物供排查
            if (IsValid(CurrentItem)) CurrentItem->bReportLostOnDestroy = bReportLost;
            UE_LOG(LogDelivery, Warning, TEXT("[PickupPoint] %s 拒绝接收确认，保留已有归属的包裹 %s owner=%s"),
                *GetNameSafe(Point), *GetNameSafe(Package), *GetNameSafe(Package->GetOwner()));
            return false;
        }
        // 构造脚本可能覆盖标志，真正销毁临时物品前再次关闭丢件上报
        if (IsValid(CurrentItem)) CurrentItem->bReportLostOnDestroy = false;
        Package->Destroy();
        return false;
    };

    // 生成期间的蓝图回调也可能改变人物或任务，真正交出前再次核对
    if (!IsValid(Package) || !IsValid(Item) || !IsValid(Receiver) || !IsValid(Player) || Player->IsInactive() ||
        Package->GetDeliveryItemComponent() != Item || Item->OwningTask != ExpectedTask ||
        Player->GetWorld() != World || !IsAtPickupPoint() ||
        Carrier->GetTaskPlayerState() != Player || !Carrier->CanHandleTaskPackage() ||
        Carrier->GetCarriedTaskPackage() || ResolvePendingTask() != ExpectedTask ||
        !Carrier->CanReceiveTaskPackage(Package))
    {
        return RejectPackage();
    }
    if (!Carrier->TryReceiveTaskPackage(Package) || !IsValid(Package) || !IsValid(Item) || !IsValid(Receiver) ||
        Carrier->GetCarriedTaskPackage() != Package || Package->GetOwner() != Receiver || Item->OwningTask != ExpectedTask)
        return RejectPackage();
    Item->bReportLostOnDestroy = bReportLost;
    return true;
}

bool UDeliveryPickupPointComponent::CanPickup(APawn *Interactor, bool bCheckTaskAvailability) const
{
    if (!Interactor)
    {
        return false;
    }
    // 探测阶段保留骑车时不可领取的目标，才能显示下车提示
    // 真正执行时仍要复核骑车状态和任务是否可以领取
    if (!bCheckTaskAvailability)
    {
        return true;
    }

    // 取货点只向下车玩家发货，后备箱装卸沿用车尾交互
    if (const ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(Interactor))
    {
        if (Character->IsRidingVehicle())
        {
            return false;
        }
    }

    return CanDispense();
}

bool UDeliveryPickupPointComponent::Pickup(APawn *Interactor)
{
    return TryDispense(Cast<ADeliveryCharacter>(Interactor));
}
