// Copyright Epic Games, Inc. All Rights Reserved.

#include "Inventory/DeliveryHandheldItem.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
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
    InteractableComponent->PromptText = NSLOCTEXT("DeliveryItems", "PickupPrompt", "[E] 拾取");
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
    RefreshCarrierPresentation();
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
    const FQuat UprightGrip = StableRotation;
    if (const ADeliveryCharacter *Holder = Cast<ADeliveryCharacter>(HeldAnchor->GetOwner()))
    {
        const float SwingWeight = Holder->GetActiveRagdoll() ? Holder->GetActiveRagdoll()->GetAxeSwingWeight() : 0.f;
        if (bUsesAxeSwing && SwingWeight > 0.f && Holder->GetMesh())
        {
            // 跟随前臂的前后摆动，忽略沿前臂轴线的细小扭转
            const FVector Forearm = (Holder->GetMesh()->GetBoneLocation(TEXT("RightHand")) -
                                     Holder->GetMesh()->GetBoneLocation(TEXT("RightForeArm")))
                                        .GetSafeNormal();
            // 握持资源把斧柄校准到 +Z；前臂朝上时刀头朝上，下劈经过前方时刀刃朝下
            const float Angle = FMath::Atan2(Forearm.Dot(StableForward), Forearm.Z);
            const FVector Right = FVector::CrossProduct(FVector::UpVector, StableForward);
            const FQuat SwingRotation(Right, Angle);
            // 待机保持竖握，动作淡出时回正，避免自然垂手把斧刃插进地面
            StableRotation = FQuat::Slerp(StableRotation, SwingRotation * StableRotation, SwingWeight).GetNormalized();
        }
    }
    const FTransform StableGripFrame(StableRotation, HeldAnchor->GetComponentLocation(), FVector::OneVector);
    const FTransform Target = HeldRelativeTransform * StableGripFrame;
    const float PositionAlpha = bSnap ? 1.f : 1.0f - FMath::Exp(-HeldPositionFollowSpeed * DeltaSeconds);
    const float RotationAlpha = bSnap ? 1.f : 1.0f - FMath::Exp(-HeldRotationFollowSpeed * DeltaSeconds);
    FVector Location = FMath::Lerp(GetActorLocation(), Target.GetLocation(), PositionAlpha);
    FQuat Rotation = FQuat::Slerp(GetActorQuat(), Target.GetRotation(), RotationAlpha).GetNormalized();
    // 在平滑之后限制最终姿态，低帧率时也不把斧刃插进地面
    if (bUsesAxeSwing)
        LimitAxeGroundPenetration(Location, Rotation, UprightGrip);
    SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
}

void ADeliveryHandheldItem::LimitAxeGroundPenetration(FVector& Location, FQuat& Rotation,
                                                     const FQuat& UprightGrip) const
{
    if (!ItemMesh->GetStaticMesh() || !GetWorld())
        return;

    const FQuat GripRotation = Rotation * HeldRelativeTransform.GetRotation().Inverse();
    const FVector Grip = Location - GripRotation.RotateVector(HeldRelativeTransform.GetLocation());
    FCollisionQueryParams Query(SCENE_QUERY_STAT(AxeGround), true, this);
    Query.AddIgnoredActor(HeldAnchor->GetOwner());
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FHitResult Ground;
    if (!GetWorld()->LineTraceSingleByObjectType(Ground, Grip + FVector(0, 0, 20),
            Grip - FVector(0, 0, 250), Objects, Query) || Ground.ImpactNormal.Z < .5f)
        return;

    // 用模型包围盒保守估算刀头和斧柄，绕握点回转，不抬高整把斧头造成脱手
    const FBox Bounds = ItemMesh->GetStaticMesh()->GetBoundingBox();
    auto IsAboveGround = [&](const FQuat& CandidateGrip)
    {
        FTransform Candidate = HeldRelativeTransform * FTransform(CandidateGrip, Grip);
        Candidate.SetScale3D(GetActorScale3D());
        for (int32 Corner = 0; Corner < 8; ++Corner)
        {
            const FVector Point((Corner & 1) ? Bounds.Max.X : Bounds.Min.X,
                                (Corner & 2) ? Bounds.Max.Y : Bounds.Min.Y,
                                (Corner & 4) ? Bounds.Max.Z : Bounds.Min.Z);
            if (FVector::DotProduct(Candidate.TransformPosition(Point) - Ground.ImpactPoint,
                                   Ground.ImpactNormal) < 2.f)
                return false;
        }
        return true;
    };
    // 手本身埋在地面时不强行挪动工具，也不把该表现限制写回角色物理
    if (IsAboveGround(GripRotation) || !IsAboveGround(UprightGrip))
        return;
    float Blocked = 0.f, Clear = 1.f;
    for (int32 Iteration = 0; Iteration < 10; ++Iteration)
    {
        const float Mid = (Blocked + Clear) * .5f;
        if (IsAboveGround(FQuat::Slerp(GripRotation, UprightGrip, Mid).GetNormalized()))
            Clear = Mid;
        else
            Blocked = Mid;
    }
    const FQuat LimitedGrip = FQuat::Slerp(GripRotation, UprightGrip, Clear).GetNormalized();
    const FTransform Limited = HeldRelativeTransform * FTransform(LimitedGrip, Grip);
    Location = Limited.GetLocation();
    Rotation = Limited.GetRotation();
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

// 回调里的新表现或新主人优先，旧调用立即停止
bool ADeliveryHandheldItem::IsPresentationOperationCurrent(uint64 Operation,const UStaticMeshComponent* ExpectedMesh,
    const TWeakObjectPtr<AActor>& ExpectedOwner) const
{
    return !IsActorBeingDestroyed() && Operation==PresentationOperation && IsValid(ExpectedMesh) &&
        ItemMesh==ExpectedMesh && GetRootComponent()==ExpectedMesh && IsValid(InteractableComponent) &&
        TWeakObjectPtr<AActor>(GetOwner()).HasSameIndexAndSerialNumber(ExpectedOwner);
}

// 根据手持或收纳状态调整物品附着、碰撞和可见性
void ADeliveryHandheldItem::SetInventoryPresentation(bool bInHand, bool bInInventory,
                                                     USceneComponent *HandParent)
{
    if (bInHand && !IsValid(HandParent)) return;
    const uint64 Operation=++PresentationOperation;
    const TWeakObjectPtr<ADeliveryHandheldItem> WeakThis(this);
    const TWeakObjectPtr<UStaticMeshComponent> WeakMesh(ItemMesh);
    TWeakObjectPtr<AActor> ExpectedOwner(GetOwner());
    const auto IsCurrent=[WeakThis,WeakMesh,Operation,&ExpectedOwner]()
    {
        return WeakThis.IsValid() && WeakThis->IsPresentationOperationCurrent(Operation,WeakMesh.Get(),ExpectedOwner);
    };
    if (!IsCurrent()) return;

    if (HasAuthority())
        SetReplicateMovement(false);
    if (!IsCurrent()) return;
    // 改变附着关系前先停止物理模拟
    // 物品可能刚从隐藏的背包槽取出，因此需要恢复可见状态

    ItemMesh->SetSimulatePhysics(false);
    if (!IsCurrent()) return;
    ItemMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    if (!IsCurrent()) return;
    SetActorEnableCollision(false);
    if (!IsCurrent()) return;
    SetActorHiddenInGame(!bInHand);
    if (!IsCurrent()) return;
    ItemMesh->SetVisibility(bInHand, true);
    if (!IsCurrent()) return;
    InteractableComponent->bInteractEnabled = false;

    if (bInHand && HandParent)
    {
        DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        if (!IsCurrent()) return;
        if (!IsValid(HandParent)) return;
        HeldAnchor = HandParent;
        bHeldPresentationActive = true;
        SetActorTickEnabled(true);
        if (!IsCurrent()) return;
        // 拾取时立即对齐，此后在物理更新完成后平滑跟随，减少手部抖动
        UpdateHeldTransform(0.0f, true);
    }
    else if (bInInventory)
    {
        DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        if (!IsCurrent()) return;
        bHeldPresentationActive = false;
        HeldAnchor.Reset();
        SetActorTickEnabled(false);
        if (!IsCurrent()) return;
    }
}

// 把物品固定到车辆货架，并停用自由物理
void ADeliveryHandheldItem::SetVehicleCargoPresentation(USceneComponent *CargoParent)
{
    SetAttachedCarryPresentation(CargoParent);
}

void ADeliveryHandheldItem::SetAttachedCarryPresentation(USceneComponent *CargoParent,
    const FVector& RelativeOffset,const FRotator& RelativeRotation)
{
    if (!IsValid(CargoParent) || CargoParent->GetWorld()!=GetWorld() || CargoParent==GetRootComponent() ||
        RelativeOffset.ContainsNaN() || RelativeRotation.ContainsNaN()) return;
    const uint64 Operation=++PresentationOperation;
    const TWeakObjectPtr<ADeliveryHandheldItem> WeakThis(this);
    const TWeakObjectPtr<UStaticMeshComponent> WeakMesh(ItemMesh);
    TWeakObjectPtr<AActor> ExpectedOwner(GetOwner());
    const auto IsCurrent=[WeakThis,WeakMesh,Operation,&ExpectedOwner]()
    {
        return WeakThis.IsValid() && WeakThis->IsPresentationOperationCurrent(Operation,WeakMesh.Get(),ExpectedOwner);
    };
    if (!IsCurrent()) return;

    // 位置由附着关系决定，不该再被移动复制覆盖一遍——两边各写一次会打架，
    // 表现是客户端上的快递在车上微微抖动
    if (HasAuthority())
        SetReplicateMovement(false);
    if (!IsCurrent()) return;

    // 模拟中的根组件附着不可靠，必须先停物理再改附着（和手上那套同一个理由）
    ItemMesh->SetSimulatePhysics(false);
    if (!IsCurrent()) return;
    ItemMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    if (!IsCurrent()) return;
    SetActorEnableCollision(false);
    if (!IsCurrent()) return;

    // 绑在车上是看得见的，和塞进背包不一样
    SetActorHiddenInGame(false);
    if (!IsCurrent()) return;
    ItemMesh->SetVisibility(true, true);
    if (!IsCurrent()) return;

    // 车上的货不给按 E："拿走别人车上的快递"不是现在要做的玩法，
    // 而留着可交互会让提示浮窗在骑车时一直糊在屏幕上
    InteractableComponent->bInteractEnabled = false;

    // 不要每帧跟随：这两个状态清掉，Tick 里那套就不会再动它
    bHeldPresentationActive = false;
    HeldAnchor.Reset();
    SetActorTickEnabled(false);
    if (!IsCurrent()) return;

    DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    if (!IsCurrent()) return;
    if (!IsValid(CargoParent) || CargoParent->GetWorld()!=GetWorld()) return;
    const TWeakObjectPtr<USceneComponent> WeakParent(CargoParent);
    if (!AttachToComponent(CargoParent, FAttachmentTransformRules::SnapToTargetNotIncludingScale)) return;
    if (!IsCurrent() || !WeakParent.IsValid() || ItemMesh->GetAttachParent()!=WeakParent.Get()) return;
    // 只写偏移与朝向，SnapNotIncludingScale 保留同一实物原有世界尺寸
    ItemMesh->SetRelativeLocationAndRotation(RelativeOffset,RelativeRotation,false,nullptr,ETeleportType::TeleportPhysics);
    if (!IsCurrent() || !WeakParent.IsValid() || ItemMesh->GetAttachParent()!=WeakParent.Get()) return;
}

// 解除收纳状态，恢复碰撞和物理并设置丢出速度
void ADeliveryHandheldItem::DropFromInventory(const FVector &WorldLocation, const FVector &InitialVelocity)
{
    if (WorldLocation.ContainsNaN() || InitialVelocity.ContainsNaN()) return;
    const uint64 Operation=++PresentationOperation;
    const TWeakObjectPtr<ADeliveryHandheldItem> WeakThis(this);
    const TWeakObjectPtr<UStaticMeshComponent> WeakMesh(ItemMesh);
    TWeakObjectPtr<AActor> ExpectedOwner(GetOwner());
    const auto IsCurrent=[WeakThis,WeakMesh,Operation,&ExpectedOwner]()
    {
        return WeakThis.IsValid() && WeakThis->IsPresentationOperationCurrent(Operation,WeakMesh.Get(),ExpectedOwner);
    };
    if (!IsCurrent()) return;

    DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    if (!IsCurrent()) return;
    bHeldPresentationActive = false;
    HeldAnchor.Reset();
    SetActorTickEnabled(false);
    if (!IsCurrent()) return;
    ExpectedOwner.Reset();
    SetOwner(nullptr);
    if (!IsCurrent()) return;
    if (HasAuthority())
        SetReplicateMovement(true);
    if (!IsCurrent()) return;
    SetActorHiddenInGame(false);
    if (!IsCurrent()) return;
    ItemMesh->SetVisibility(true, true);
    if (!IsCurrent()) return;
    SetActorEnableCollision(true);
    if (!IsCurrent()) return;
    SetActorLocation(WorldLocation, false, nullptr, ETeleportType::TeleportPhysics);
    if (!IsCurrent()) return;
    ItemMesh->SetCollisionProfileName(TEXT("PhysicsActor"));
    if (!IsCurrent()) return;
    ItemMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    if (!IsCurrent()) return;
    ItemMesh->SetSimulatePhysics(true);
    if (!IsCurrent()) return;
    ItemMesh->SetPhysicsLinearVelocity(InitialVelocity);
    if (!IsCurrent()) return;
    InteractableComponent->bInteractEnabled = true;
}

// 检查物品归属，任务物品还需通过任务条件
bool ADeliveryHandheldItem::CanPickup(APawn *Interactor, bool bCheckTaskAvailability) const
{
    const ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(Interactor);
    const auto* Grabbable=FindComponentByClass<UDeliveryGrabbableComponent>();
    if (!Character || GetOwner() || !ItemComponent->CanTransferTo(Character) ||
        (Grabbable && Grabbable->HasAnyCarrier()))
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

void ADeliveryHandheldItem::RefreshCarrierPresentation()
{
    if (auto* Carrier=Cast<IDeliveryHandheldCarrierInterface>(GetOwner()))
    {
        bHadHandheldCarrierPresentation=true;
        Carrier->ApplyHandheldItemPresentation(this);
    }
    else if (!GetOwner() && bHadHandheldCarrierPresentation)
    {
        bHadHandheldCarrierPresentation=false;
        DropFromInventory(GetActorLocation(),FVector::ZeroVector);
    }
    else if (GetOwner()) bHadHandheldCarrierPresentation=false;
}

void ADeliveryHandheldItem::OnRep_Owner()
{
    Super::OnRep_Owner();
    RefreshCarrierPresentation();
}
