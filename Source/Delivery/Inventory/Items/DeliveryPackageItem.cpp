// 实现带任务身份的手持快递物品
// Copyright Epic Games, Inc. All Rights Reserved.

#include "Inventory/Items/DeliveryPackageItem.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Carry/DeliveryPackageCarrierInterface.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "Inventory/Items/DeliveryCaterpillarItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Net/UnrealNetwork.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/PlayerController.h"
#include "Misc/ScopeExit.h"
#include "Character/DeliveryCharacter.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "UObject/ConstructorHelpers.h"

ADeliveryPackageItem::ADeliveryPackageItem()
{
    DeliveryItemComponent = CreateDefaultSubobject<UDeliveryItemComponent>(TEXT("DeliveryTaskItem"));
    CaterpillarVisual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CaterpillarVisual"));
    CaterpillarVisual->SetupAttachment(ItemMesh);
    CaterpillarVisual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    CaterpillarVisual->SetAbsolute(false, false, true);
    CaterpillarVisual->SetHiddenInGame(true);
    ItemComponent->ItemType = EDeliveryInventoryItemType::DeliveryItem;
    ItemComponent->DisplayName = NSLOCTEXT("DeliveryItems", "DeliveryPackage", "快递");
    static ConstructorHelpers::FObjectFinder<UTexture2D> PackageIcon(
        TEXT("/Game/UI/Inventory/Textures/T_ItemIconPackage.T_ItemIconPackage"));
    if (PackageIcon.Succeeded())
        ItemComponent->Icon = PackageIcon.Object;
    ItemComponent->bUsesDurability = false;
    ItemComponent->MaxDurability = 0.0f;
    ItemComponent->Durability = 0.0f;
    InteractableComponent->PromptText = NSLOCTEXT("DeliveryItems", "HoldPickupPackage", "[按住 E] 拾取快递");
    InteractableComponent->InteractionKey = EDeliveryInteractionKey::PickupE;
    InteractableComponent->HoldDuration = 0.5f;
    InteractableComponent->InteractRadius = 100.0f;
    HeldRelativeTransform = FTransform(FRotator::ZeroRotator, FVector(18.0f, 0.0f, 0.0f));
}

ADeliveryTestPackage::ADeliveryTestPackage()
{
    static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (CubeFinder.Succeeded())
    {
        ItemMesh->SetStaticMesh(CubeFinder.Object);
        ItemMesh->SetRelativeScale3D(FVector(0.32f, 0.26f, 0.20f));
    }
    static ConstructorHelpers::FObjectFinder<UDeliveryTaskDefinition> TaskFinder(
        TEXT("/Game/Task/Definitions/DA_Task_001.DA_Task_001"));
    if (TaskFinder.Succeeded())
        DeliveryItemComponent->OwningTask = TaskFinder.Object;
}

// 为测试快递补齐测试任务配置
void ADeliveryTestPackage::BeginPlay()
{
    Super::BeginPlay();
    if (UStaticMesh *Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
    {
        ItemMesh->SetStaticMesh(Cube);
        ItemMesh->SetRelativeScale3D(FVector(0.32f, 0.26f, 0.20f));
    }
    ItemMesh->SetMassOverrideInKg(NAME_None, 1.5f, true);
}

bool ADeliveryPackageItem::CanPickup(APawn *Interactor, bool bCheckTaskAvailability) const
{
    const auto* Character = Cast<ADeliveryCharacter>(Interactor);
    if (Character && Cast<ADeliveryCaterpillarItem>(Character->GetInventoryComponent()->GetHeldItem()))
        return CanInsertCaterpillar(Interactor);
    return Cast<ADeliveryCharacter>(Interactor) && !GetOwner() &&
           (!bCheckTaskAvailability || (DeliveryItemComponent && DeliveryItemComponent->CanBeAcquired()));
}

bool ADeliveryPackageItem::Pickup(APawn *Interactor)
{
    ADeliveryCharacter *Character = Cast<ADeliveryCharacter>(Interactor);
    if (Character && Cast<ADeliveryCaterpillarItem>(Character->GetInventoryComponent()->GetHeldItem()))
        return TryInsertCaterpillar(Interactor);
    return Character && Character->ReceiveTaskPackage(this);
}

void ADeliveryPackageItem::BeginPlay()
{
    Super::BeginPlay();
    OnRep_Caterpillar();
    RefreshCarrierPresentation();
}

void ADeliveryPackageItem::EndPlay(const EEndPlayReason::Type Reason)
{
    ClearRecoveryInventory();
    Super::EndPlay(Reason);
}

void ADeliveryPackageItem::SetOwner(AActor* NewOwner)
{
    Super::SetOwner(NewOwner);
    if (HasAuthority() && GetOwner()) ClearRecoveryInventory();
}

void ADeliveryPackageItem::ClearRecoveryInventory()
{
    if (auto* Inventory = RecoveryInventory.Get())
        Inventory->ForgetDroppedFirstOrderPackage(this);
    RecoveryInventory.Reset();
}

void ADeliveryPackageItem::RefreshCarrierPresentation()
{
    if (auto* Bike = Cast<ADeliveryMotorbike>(GetOwner()))
    {
        // 车辆装卸仍由 Cargo 复制恢复，持件接口只负责服务器交接
        bHadCarrierPresentation = false;
        Bike->RefreshCargoPresentation(this);
    }
    else if (auto* Carrier = Cast<IDeliveryPackageCarrierInterface>(GetOwner()))
    {
        bHadCarrierPresentation = true;
        Carrier->ApplyTaskPackagePresentation(this);
    }
    else if (!GetOwner() && bHadCarrierPresentation)
    {
        bHadCarrierPresentation = false;
        // 掉落恢复物理；服务器后续的移动复制继续提供实际落点
        DropFromInventory(GetActorLocation(), FVector::ZeroVector);
    }
    else if (GetOwner())
    {
        bHadCarrierPresentation = false;
    }
}

void ADeliveryPackageItem::OnRep_Owner()
{
    // 基类按虚函数选择包裹原有表现，避免普通持物与任务持物重复刷新
    Super::OnRep_Owner();
}

void ADeliveryPackageItem::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ADeliveryPackageItem, bContainsCaterpillar);
}

bool ADeliveryPackageItem::IsPrankPackageAvailable() const
{
    const auto* Task = DeliveryItemComponent->OwningTask.Get();
    const auto* Manager = UDeliveryTaskManagerComponent::Get(this);
    return !IsActorBeingDestroyed() && !IsHidden() && !bContainsCaterpillar && IsValid(Task) && Manager &&
        Manager->GetTaskStatus(Task)==EDeliveryTaskStatus::InProgress &&
        Task->SpecialEventRules.ContainsByPredicate([](const FDeliverySpecialEventRule& Rule)
            { return Rule.EventTag==TAG_Event_Delivery_Caterpillar; });
}

bool ADeliveryPackageItem::HasCaterpillarInsertionReach(const APawn* Interactor,const AActor* Worm) const
{
    if (!IsValid(Interactor) || Interactor->GetWorld()!=GetWorld()) return false;
    const auto* Carrier=GetOwner();
    // 手持原件关闭了通用 E 地面拾取，投放仍按当前实物网格检查接触距离
    if (!Carrier && !InteractableComponent->CanInteract(Interactor)) return false;
    const FVector From=Interactor->GetActorLocation()+FVector(0,0,25);
    const FVector Point=Carrier?ItemMesh->Bounds.Origin:InteractableComponent->GetInteractionPoint(From)+FVector(0,0,5);
    if (From.ContainsNaN() || Point.ContainsNaN()) return false;
    if (Carrier)
    {
        const double Reach=InteractableComponent->InteractRadius+Interactor->GetSimpleCollisionRadius();
        if (!FMath::IsFinite(Reach) || Reach<=0 || FVector::DistSquared(From,Point)>FMath::Square(Reach)) return false;
    }
    FCollisionQueryParams Query(SCENE_QUERY_STAT(InsertCaterpillar), false, this);
    Query.AddIgnoredActor(Interactor);
    if (Worm) Query.AddIgnoredActor(Worm);
    if (Carrier) Query.AddIgnoredActor(Carrier);
    FHitResult Hit;
    return !GetWorld()->LineTraceSingleByChannel(Hit,From,Point,ECC_Visibility,Query);
}

bool ADeliveryPackageItem::CanInsertCaterpillar(const APawn* Interactor) const
{
    const auto* Character=Cast<ADeliveryCharacter>(Interactor);
    if (!Character || Character->GetWorld()!=GetWorld() || GetOwner() || !IsPrankPackageAvailable() ||
        Character->IsIncapacitated() || Character->IsRidingVehicle()) return false;
    const auto* Worm=Cast<ADeliveryCaterpillarItem>(Character->GetInventoryComponent()->GetHeldItem());
    return IsValid(Worm) && Worm->GetOwner()==Character && HasCaterpillarInsertionReach(Character,Worm);
}

bool ADeliveryPackageItem::CanAcceptCompanionCaterpillar(const ADeliveryCompanionCharacter* Companion,
    const APlayerState* Requester,bool bCheckReach,bool bCompanionHoldsPackage) const
{
    if (!HasAuthority() || !IsValid(Companion) || Companion->GetWorld()!=GetWorld() || Companion->IsActorBeingDestroyed() ||
        !IsValid(Requester) || Requester->IsInactive() || Requester->GetWorld()!=GetWorld() ||
        Companion->GetHandheldOwnerPlayerState()!=Requester || !IsPrankPackageAvailable()) return false;
    const auto* Controller=Requester->GetPlayerController();
    const auto* Player=IsValid(Controller)?Cast<ADeliveryCharacter>(Controller->GetPawn()):nullptr;
    if (!IsValid(Player) || Player->IsActorBeingDestroyed() || Player->GetWorld()!=GetWorld() ||
        Player->GetTaskPlayerState()!=Requester || !Player->CanHandleTaskPackage()) return false;
    if (bCompanionHoldsPackage)
    {
        if (GetOwner()!=Companion || Companion->GetCarriedTaskPackage()!=this || !Companion->CanHandleTaskPackage()) return false;
    }
    else if (GetOwner())
    {
        if (GetOwner()!=Player || Player->GetInventoryComponent()->GetHeldItem()!=this) return false;
    }
    else if (GetAttachParentActor() || !GetActorEnableCollision() ||
        Player->GetInventoryComponent()->GetDroppedFirstOrderPackage()!=this) return false;
    return !bCheckReach || (Companion->CanHandleHandheldItem() &&
        HasCaterpillarInsertionReach(Companion,Companion->GetCarriedHandheldItem()));
}

bool ADeliveryPackageItem::TryInsertGroundCaterpillarIntoHeldPackage(ADeliveryCompanionCharacter* Companion,
    ADeliveryCaterpillarItem* Worm,const APlayerState* Requester,TFunctionRef<bool()> IsStillAtContact)
{
    if (!IsInGameThread() || !IsStillAtContact() || !CanAcceptCompanionCaterpillar(Companion,Requester,true,true))
        return false;
    const auto* Pickup=DeliveryHandheldTransfer::FindWorldPickupInteraction(Worm,GetWorld());
    if (!Pickup || !Pickup->CanInteract(Companion) || !DeliveryItemComponent->BeginTransfer(Companion)) return false;
    const TWeakObjectPtr<UDeliveryItemComponent> WeakPackageData(DeliveryItemComponent);
    ON_SCOPE_EXIT { if (WeakPackageData.IsValid()) WeakPackageData->EndTransfer(); };

    auto* WormData=Worm->GetItemComponent();
    const FGuid WormTransfer=WormData->BeginTransfer(Companion);
    if (!WormTransfer.IsValid()) return false;
    const TWeakObjectPtr<UDeliveryInventoryItemComponent> WeakWormData(WormData);
    ON_SCOPE_EXIT { if (WeakWormData.IsValid()) WeakWormData->EndTransfer(WormTransfer); };

    // 未转移任何持有关系，核验失败时原件仍在手里，原虫仍在原地
    if (!IsStillAtContact() || !CanAcceptCompanionCaterpillar(Companion,Requester,true,true) ||
        DeliveryHandheldTransfer::FindWorldPickupInteraction(Worm,GetWorld())!=Pickup) return false;
    CommitCaterpillar(Worm);
    return true;
}

bool ADeliveryPackageItem::TryInsertCompanionCaterpillar(ADeliveryCompanionCharacter* Companion,ADeliveryCaterpillarItem* Worm,
    const APlayerState* Requester,TFunctionRef<bool()> IsCurrent)
{
    if (!IsInGameThread() || !IsCurrent() || !CanAcceptCompanionCaterpillar(Companion,Requester,true) || !IsValid(Worm) ||
        Worm->GetWorld()!=GetWorld() || Worm->GetOwner()!=Companion || !Companion->OwnsHandheldItem(Worm) ||
        !DeliveryItemComponent->BeginTransfer(Companion)) return false;
    const TWeakObjectPtr<UDeliveryItemComponent> WeakPackageData(DeliveryItemComponent);
    ON_SCOPE_EXIT { if (WeakPackageData.IsValid()) WeakPackageData->EndTransfer(); };
    auto* WormData=Worm->GetItemComponent();
    const FGuid WormTransfer=WormData->BeginTransfer(Companion);
    if (!WormTransfer.IsValid()) return false;
    const TWeakObjectPtr<UDeliveryInventoryItemComponent> WeakWormData(WormData);
    ON_SCOPE_EXIT { if (WeakWormData.IsValid()) WeakWormData->EndTransfer(WormTransfer); };
    const TWeakObjectPtr<ADeliveryPackageItem> WeakPackage(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakCompanion(Companion);
    const TWeakObjectPtr<ADeliveryCaterpillarItem> WeakWorm(Worm);
    const TWeakObjectPtr<const APlayerState> WeakRequester(Requester);
    const FVector OriginalLocation=Companion->GetActorLocation();
    if (!Companion->ReleaseHandheldItem(Worm)) return false;
    // 释放后仍只允许原安排提交，失效时把同一只小绿交还二狗或放回脚边
    if (IsCurrent() && WeakPackage.IsValid() && WeakWorm.IsValid() && !WeakWorm->GetOwner() &&
        CanAcceptCompanionCaterpillar(WeakCompanion.Get(),WeakRequester.Get(),false) &&
        WeakCompanion->CanHandleHandheldItem() && HasCaterpillarInsertionReach(WeakCompanion.Get(),WeakWorm.Get()))
    {
        CommitCaterpillar(WeakWorm.Get());
        return true;
    }
    if (WeakWorm.IsValid() && !WeakWorm->GetOwner())
    {
        if (WeakCompanion.IsValid() && WeakCompanion->TryReceiveHandheldItem(WeakWorm.Get())) return false;
        WeakWorm->DropFromInventory(OriginalLocation,FVector::ZeroVector);
    }
    return false;
}

bool ADeliveryPackageItem::TryInsertCaterpillar(APawn* Interactor)
{
    if (!HasAuthority() || !CanInsertCaterpillar(Interactor)) return false;
    auto* Character = CastChecked<ADeliveryCharacter>(Interactor);
    auto* Inventory = Character->GetInventoryComponent();
    auto* Worm = Cast<ADeliveryCaterpillarItem>(Inventory->GetHeldItem());
    if (!DeliveryItemComponent->BeginTransfer(Character)) return false;
    const TWeakObjectPtr<UDeliveryItemComponent> WeakPackageData(DeliveryItemComponent);
    ON_SCOPE_EXIT { if (WeakPackageData.IsValid()) WeakPackageData->EndTransfer(); };
    auto* WormData=Worm->GetItemComponent();
    const FGuid WormTransfer=WormData->BeginTransfer(Character);
    if (!WormTransfer.IsValid()) return false;
    const TWeakObjectPtr<UDeliveryInventoryItemComponent> WeakWormData(WormData);
    ON_SCOPE_EXIT { if (WeakWormData.IsValid()) WeakWormData->EndTransfer(WormTransfer); };
    const TWeakObjectPtr<ADeliveryPackageItem> WeakPackage(this);
    const TWeakObjectPtr<ADeliveryCharacter> WeakCharacter(Character);
    const TWeakObjectPtr<ADeliveryCaterpillarItem> WeakWorm(Worm);
    const FVector OriginalLocation=Character->GetActorLocation();
    if (!Inventory->ConsumeHeldItem(Worm)) return false;
    // 背包回调可能改变当前站位或销毁包裹，仍需以原件和原持有者提交
    if (!WeakPackage.IsValid() || !WeakCharacter.IsValid() || !WeakWorm.IsValid() || WeakWorm->GetOwner() ||
        GetOwner() || !IsPrankPackageAvailable() || !Character->CanHandleTaskPackage() || !HasCaterpillarInsertionReach(Character,Worm))
    {
        if (WeakWorm.IsValid() && !WeakWorm->GetOwner() &&
            (!WeakCharacter.IsValid() || !WeakCharacter->TryReceiveHandheldItem(WeakWorm.Get())))
            WeakWorm->DropFromInventory(OriginalLocation,FVector::ZeroVector);
        return false;
    }
    CommitCaterpillar(Worm);
    if (WeakCharacter.IsValid()) WeakCharacter->ClientPickupSucceeded();
    return true;
}

void ADeliveryPackageItem::CommitCaterpillar(ADeliveryCaterpillarItem* Worm)
{
    // 只在同一只实物已释放后置位，额外奖励仍由原交付路径评定
    bContainsCaterpillar = true;
    OnRep_Caterpillar();
    ForceNetUpdate();
    if (IsValid(Worm)) Worm->Destroy();
}

void ADeliveryPackageItem::OnRep_Caterpillar()
{
    CaterpillarVisual->SetHiddenInGame(!bContainsCaterpillar);
    if (ItemMesh->GetStaticMesh() && CaterpillarVisual->GetStaticMesh())
    {
        const FBox Bounds = ItemMesh->GetStaticMesh()->GetBoundingBox();
        const FBox WormBounds = CaterpillarVisual->GetStaticMesh()->GetBoundingBox();
        const float Scale = 14.f / FMath::Max(WormBounds.GetSize().GetMax(), 1.f);
        CaterpillarVisual->SetWorldScale3D(FVector(Scale));
        const FVector ParentScale = ItemMesh->GetComponentScale().GetAbs().ComponentMax(FVector(.001f));
        CaterpillarVisual->SetRelativeLocation(FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y,
            Bounds.Max.Z + 1.f / ParentScale.Z) - WormBounds.GetCenter() * Scale / ParentScale);
    }
}

FText ADeliveryPackageItem::GetPackagePrompt(const APawn* Interactor) const
{
    const auto* Character = Cast<ADeliveryCharacter>(Interactor);
    if (Character && Cast<ADeliveryCaterpillarItem>(Character->GetInventoryComponent()->GetHeldItem()))
        return bContainsCaterpillar
            ? NSLOCTEXT("DeliveryItems", "CaterpillarAlreadyAdded", "外卖里已经有毛毛虫，先收起手里的毛毛虫再取餐")
            : NSLOCTEXT("DeliveryItems", "InsertCaterpillar", "[按住 E] 把毛毛虫放进外卖（交付时有额外奖励）");
    return bContainsCaterpillar
        ? NSLOCTEXT("DeliveryItems", "PickupFoodWithCaterpillar", "[按住 E] 拾取外卖（已加入毛毛虫）")
        : InteractableComponent->PromptText;
}
