// 实现带任务身份的手持快递物品
// Copyright Epic Games, Inc. All Rights Reserved.

#include "Inventory/DeliveryPackageItem.h"
#include "Inventory/DeliveryCaterpillarItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Net/UnrealNetwork.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
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
    ItemComponent->DisplayName = NSLOCTEXT("DeliveryItems", "DeliveryPackage", "Package");
    static ConstructorHelpers::FObjectFinder<UTexture2D> PackageIcon(
        TEXT("/Game/UI/Inventory/Textures/T_ItemIconPackage.T_ItemIconPackage"));
    if (PackageIcon.Succeeded())
        ItemComponent->Icon = PackageIcon.Object;
    ItemComponent->bUsesDurability = false;
    ItemComponent->MaxDurability = 0.0f;
    ItemComponent->Durability = 0.0f;
    InteractableComponent->PromptText = NSLOCTEXT("DeliveryItems", "HoldPickupPackage", "[Hold E] Pick Up Package");
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
}

void ADeliveryPackageItem::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ADeliveryPackageItem, bContainsCaterpillar);
}

bool ADeliveryPackageItem::CanInsertCaterpillar(const APawn* Interactor) const
{
    const auto* Character = Cast<ADeliveryCharacter>(Interactor);
    if (!Character || Character->GetWorld() != GetWorld() || GetOwner() || bContainsCaterpillar ||
        Character->IsIncapacitated() || Character->IsRidingVehicle() || !InteractableComponent->CanInteract(Character)) return false;
    const auto* Worm = Cast<ADeliveryCaterpillarItem>(Character->GetInventoryComponent()->GetHeldItem());
    const auto* Task = DeliveryItemComponent->OwningTask.Get();
    const auto* Manager = UDeliveryTaskManagerComponent::Get(this);
    if (!IsValid(Worm) || Worm->GetOwner() != Character || !Task || !Manager ||
        Manager->GetTaskStatus(DeliveryItemComponent->OwningTask) != EDeliveryTaskStatus::InProgress ||
        !Task->SpecialEventRules.ContainsByPredicate([](const FDeliverySpecialEventRule& Rule)
            { return Rule.EventTag == TAG_Event_Delivery_Caterpillar; })) return false;
    // 完成长按时再次校验遮挡；不能隔墙向地面外卖投放，也不能修改别人手里或后箱里的外卖
    FCollisionQueryParams Query(SCENE_QUERY_STAT(InsertCaterpillar), false, this);
    Query.AddIgnoredActor(Character);
    Query.AddIgnoredActor(Worm);
    FHitResult Hit;
    return !GetWorld()->LineTraceSingleByChannel(Hit, Character->GetActorLocation() + FVector(0,0,25),
        GetActorLocation() + FVector(0,0,5), ECC_Visibility, Query);
}

bool ADeliveryPackageItem::TryInsertCaterpillar(APawn* Interactor)
{
    if (!HasAuthority() || !CanInsertCaterpillar(Interactor)) return false;
    auto* Character = CastChecked<ADeliveryCharacter>(Interactor);
    auto* Inventory = Character->GetInventoryComponent();
    auto* Worm = Cast<ADeliveryCaterpillarItem>(Inventory->GetHeldItem());
    if (!Inventory->ConsumeHeldItem(Worm)) return false;
    // 同一服务器调用中消耗物品并置位，后续并发请求只能看到已经放入的结果
    bContainsCaterpillar = true;
    OnRep_Caterpillar();
    ForceNetUpdate();
    Worm->Destroy();
    Character->ClientPickupSucceeded();
    return true;
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
            ? NSLOCTEXT("DeliveryItems", "CaterpillarAlreadyAdded", "Caterpillar already added - put away the caterpillar to pick up food")
            : NSLOCTEXT("DeliveryItems", "InsertCaterpillar", "[Hold E] Put caterpillar in food (delivery bonus)");
    return bContainsCaterpillar
        ? NSLOCTEXT("DeliveryItems", "PickupFoodWithCaterpillar", "[Hold E] Pick up food - caterpillar bonus ready")
        : InteractableComponent->PromptText;
}
