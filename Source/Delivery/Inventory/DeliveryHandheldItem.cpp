// Copyright Epic Games, Inc. All Rights Reserved.

#include "Inventory/DeliveryHandheldItem.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"

ADeliveryHandheldItem::ADeliveryHandheldItem()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
    PrimaryActorTick.TickGroup = TG_PostPhysics;
    bReplicates = true;
    SetReplicateMovement(true);
    SetNetUpdateFrequency(20.0f);

    ItemMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ItemMesh"));
    ItemMesh->SetCollisionProfileName(TEXT("PhysicsActor"));
    ItemMesh->SetSimulatePhysics(true);
    ItemMesh->SetIsReplicated(true);
    RootComponent = ItemMesh;

    ItemComponent = CreateDefaultSubobject<UDeliveryInventoryItemComponent>(TEXT("Item"));
    InteractableComponent = CreateDefaultSubobject<UDeliveryInteractableComponent>(TEXT("Interactable"));
    InteractableComponent->PromptText = NSLOCTEXT("DeliveryItems", "PickupPrompt", "[E] Pick Up");
    InteractableComponent->InteractRadius = 100.0f;
    InteractableComponent->PromptOffset = FVector(0.0f, 0.0f, 45.0f);
    InteractableComponent->InteractionKey = EDeliveryInteractionKey::PickupE;
}

// 将物品的拾取条件和执行方法绑定到交互组件
void ADeliveryHandheldItem::BeginPlay()
{
    Super::BeginPlay();
    // 将目标行为接入通用长按流程
    InteractableComponent->CanUse.BindUObject(this, &ADeliveryHandheldItem::CanPickup);
    InteractableComponent->Use.BindUObject(this, &ADeliveryHandheldItem::Pickup);
}

void ADeliveryHandheldItem::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    UpdateHeldTransform(DeltaSeconds, false);
}

// 根据手部位置和身体朝向更新手持物，平滑掉细小抖动
void ADeliveryHandheldItem::UpdateHeldTransform(float DeltaSeconds, bool bSnap)
{
    if (!bHeldPresentationActive || !HeldAnchor.IsValid())
        return;

    // 手持物品的位置跟随物理手掌，旋转不直接继承手掌
    // 手掌可能在物理模拟中连续翻转，直接继承会让工具不停旋转
    // 使用同步后的身体水平朝向建立稳定的握持方向
    FVector StableForward = FVector::ForwardVector;
    if (const ADeliveryCharacter *Holder = Cast<ADeliveryCharacter>(HeldAnchor->GetOwner()))
    {
        // 角色根对象的水平旋转可能跟随镜头
        // 工具应跟随身体朝向，避免转动镜头时工具也跟着转动
        StableForward = Holder->GetActiveRagdoll() ? Holder->GetActiveRagdoll()->GetBodyForward()
                                                   : Holder->GetActorForwardVector().GetSafeNormal2D();
    }
    if (StableForward.IsNearlyZero())
        StableForward = FVector::ForwardVector;
    FQuat StableRotation = FRotationMatrix::MakeFromXZ(StableForward, FVector::UpVector).ToQuat();
    if (const ADeliveryCharacter *Holder = Cast<ADeliveryCharacter>(HeldAnchor->GetOwner()))
    {
        const float SwingWeight =
            Holder->GetActiveRagdoll() ? Holder->GetActiveRagdoll()->GetAxeSwingWeight() : 0.f;
        if (SwingWeight > 0 && Holder->GetMesh())
        {
            // 跟随前臂的前后摆动，忽略沿前臂轴线的细小扭转
            const FVector Forearm = (Holder->GetMesh()->GetBoneLocation(TEXT("RightHand")) -
                                     Holder->GetMesh()->GetBoneLocation(TEXT("RightForeArm")))
                                        .GetSafeNormal();
            const float Angle = FMath::Atan2(Forearm.Dot(StableForward), -Forearm.Z);
            const FVector Right = FVector::CrossProduct(FVector::UpVector, StableForward);
            const FQuat SwingRotation(Right, -Angle);
            StableRotation =
                FQuat::Slerp(StableRotation, SwingRotation * StableRotation, SwingWeight).GetNormalized();
        }
    }
    const FTransform StableGripFrame(StableRotation, HeldAnchor->GetComponentLocation(), FVector::OneVector);
    const FTransform Target = HeldRelativeTransform * StableGripFrame;
    if (bSnap)
    {
        SetActorLocationAndRotation(Target.GetLocation(), Target.GetRotation(), false, nullptr,
                                    ETeleportType::TeleportPhysics);
        return;
    }

    const float PositionAlpha = 1.0f - FMath::Exp(-HeldPositionFollowSpeed * DeltaSeconds);
    const float RotationAlpha = 1.0f - FMath::Exp(-HeldRotationFollowSpeed * DeltaSeconds);
    const FVector Location = FMath::Lerp(GetActorLocation(), Target.GetLocation(), PositionAlpha);
    const FQuat Rotation = FQuat::Slerp(GetActorQuat(), Target.GetRotation(), RotationAlpha).GetNormalized();
    SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
}

void ADeliveryHandheldItem::Use_Implementation(APawn *User) {}

// 斧头使用物理挥砍，其余物品调用自身使用行为
void ADeliveryHandheldItem::UseFromInventory(APawn *User)
{
    if (bUsesAxeSwing)
    {
        if (ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(User))
        {
            if (Character->GetActiveRagdoll())
                Character->GetActiveRagdoll()->RequestAxeSwing();
        }
        return;
    }
    Use(User);
}

// 将通用交互请求交给角色背包处理
void ADeliveryHandheldItem::HandleInteract(APawn *Interactor)
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(Interactor);
    if (Character && Character->GetInventoryComponent())
    {
        Character->GetInventoryComponent()->TryPickup(this);
    }
}

// 根据手持或收纳状态调整物品附着、碰撞和可见性
void ADeliveryHandheldItem::SetInventoryPresentation(bool bInHand, bool bInInventory,
                                                     USceneComponent *HandParent)
{
    if (HasAuthority())
        SetReplicateMovement(false);
    // 改变附着关系前先停止物理模拟
    // 物品可能刚从隐藏的背包槽取出，因此需要恢复可见状态

    ItemMesh->SetSimulatePhysics(false);
    ItemMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SetActorEnableCollision(false);
    SetActorHiddenInGame(!bInHand);
    ItemMesh->SetVisibility(bInHand, true);
    InteractableComponent->bInteractEnabled = false;

    if (bInHand && HandParent)
    {
        DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        HeldAnchor = HandParent;
        bHeldPresentationActive = true;
        SetActorTickEnabled(true);
        // 拾取时立即对齐，此后在物理更新完成后平滑跟随，减少手部抖动
        UpdateHeldTransform(0.0f, true);
    }
    else if (bInInventory)
    {
        DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        bHeldPresentationActive = false;
        HeldAnchor.Reset();
        SetActorTickEnabled(false);
    }
}

// 把物品固定到车辆货架，并停用自由物理
void ADeliveryHandheldItem::SetVehicleCargoPresentation(USceneComponent *CargoParent)
{
    if (!CargoParent)
    {
        return;
    }

    // 位置由附着关系决定，不该再被移动复制覆盖一遍——两边各写一次会打架，
    // 表现是客户端上的快递在车上微微抖动
    if (HasAuthority())
        SetReplicateMovement(false);

    // 模拟中的根组件附着不可靠，必须先停物理再改附着（和手上那套同一个理由）
    ItemMesh->SetSimulatePhysics(false);
    ItemMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SetActorEnableCollision(false);

    // 绑在车上是看得见的，和塞进背包不一样
    SetActorHiddenInGame(false);
    ItemMesh->SetVisibility(true, true);

    // 车上的货不给按 E："拿走别人车上的快递"不是现在要做的玩法，
    // 而留着可交互会让提示浮窗在骑车时一直糊在屏幕上
    InteractableComponent->bInteractEnabled = false;

    // 不要每帧跟随：这两个状态清掉，Tick 里那套就不会再动它
    bHeldPresentationActive = false;
    HeldAnchor.Reset();
    SetActorTickEnabled(false);

    DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    AttachToComponent(CargoParent, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
}

// 解除收纳状态，恢复碰撞和物理并设置丢出速度
void ADeliveryHandheldItem::DropFromInventory(const FVector &WorldLocation, const FVector &InitialVelocity)
{
    DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    bHeldPresentationActive = false;
    HeldAnchor.Reset();
    SetActorTickEnabled(false);
    SetOwner(nullptr);
    if (HasAuthority())
        SetReplicateMovement(true);
    SetActorHiddenInGame(false);
    ItemMesh->SetVisibility(true, true);
    SetActorEnableCollision(true);
    SetActorLocation(WorldLocation, false, nullptr, ETeleportType::TeleportPhysics);
    ItemMesh->SetCollisionProfileName(TEXT("PhysicsActor"));
    ItemMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    ItemMesh->SetSimulatePhysics(true);
    ItemMesh->SetPhysicsLinearVelocity(InitialVelocity);
    InteractableComponent->bInteractEnabled = true;
}

// 检查物品归属，任务物品还需通过任务条件
bool ADeliveryHandheldItem::CanPickup(APawn *Interactor, bool bCheckTaskAvailability) const
{
    const ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(Interactor);
    if (!Character || GetOwner())
        return false;
    if (const UDeliveryItemComponent *TaskItem = FindComponentByClass<UDeliveryItemComponent>())
    {
        return !bCheckTaskAvailability || TaskItem->CanBeAcquired();
    }
    return !Character->IsRidingVehicle();
}

// 在服务器将物品放入背包，成功后通知操作玩家
bool ADeliveryHandheldItem::Pickup(APawn *Interactor)
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(Interactor);
    if (!Character || !Character->HasAuthority() || !Character->GetInventoryComponent() ||
        !Character->GetInventoryComponent()->TryPickup(this))
    {
        return false;
    }
    Character->ClientPickupSucceeded();
    return true;
}
