#include "NPC/DeliveryGrandpa.h"
#include "AIController.h"
#include "Animation/AnimSequence.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BrainComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Delivery.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerState.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Net/UnrealNetwork.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/World/DeliveryBreakableDoor.h"
#include "Task/World/DeliveryTargetComponent.h"
#include "TimerManager.h"
#include "World/DeliveryLocationComponent.h"

ADeliveryGrandpa::ADeliveryGrandpa()
{
    bReplicates = true;
    SetReplicateMovement(true);
    PrimaryActorTick.bCanEverTick = true;
    AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;
    AIControllerClass = AAIController::StaticClass();
    Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Capsule"));
    SetRootComponent(Capsule);
    Capsule->InitCapsuleSize(25.f, 58.f);
    Capsule->SetCollisionProfileName(TEXT("Pawn"));
    Body = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Body"));
    Body->SetupAttachment(Capsule);
    Body->SetRelativeLocation(FVector(0, 0, -58));
    Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Body->SetAnimationMode(EAnimationMode::AnimationSingleNode);
    Body->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    Speech = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Speech"));
    Speech->SetupAttachment(Capsule);
    Speech->SetRelativeLocation(FVector(0, 0, 90));
    Speech->SetHorizontalAlignment(EHorizTextAligment::EHTA_Center);
    Speech->SetWorldSize(15.f);
    Speech->SetTextRenderColor(FColor(255, 241, 180));
    ReceiveInteraction = CreateDefaultSubobject<UDeliveryInteractableComponent>(TEXT("ReceiveInteraction"));
    ReceiveInteraction->InteractionKey = EDeliveryInteractionKey::PickupE;
    ReceiveInteraction->HoldDuration = .5f;
    ReceiveInteraction->PromptOffset = FVector(0, 0, 75);
    ReceiveInteraction->PromptText = NSLOCTEXT("Grandpa", "Deliver", "[Hold E] Give Grandpa the delivery");
    DeliveryTarget = CreateDefaultSubobject<UDeliveryTargetComponent>(TEXT("DeliveryTarget"));
    DeliveryTarget->InteractRadius = 150.f;
    DeliveryTarget->DeliveryRadius = 150.f;
    auto* Location = CreateDefaultSubobject<UDeliveryLocationComponent>(TEXT("RecipientLocation"));
    Location->SetupAttachment(Capsule);
    Location->LocationId = TEXT("NPC_OldMan_001");
    Location->DisplayName = NSLOCTEXT("Grandpa", "Name", "Grandpa");
}

void ADeliveryGrandpa::BeginPlay()
{
    Super::BeginPlay();
    HomeLocation = GetActorLocation();
    StrollLocation = HomeLocation + GetActorRightVector() * StrollDistance;
    DeliveryTarget->DeliveryGate.BindUObject(this, &ADeliveryGrandpa::CanReceive);
    OnRep_FrontDoor();
    if (HasAuthority())
    {
        if (!GetController()) SpawnDefaultController();
        if (AAIController* AI = Cast<AAIController>(GetController()))
        {
            if (!Behavior || !AI->RunBehaviorTree(Behavior))
                UE_LOG(LogDelivery, Error, TEXT("Grandpa %s 行为树未能启动"), *GetName());
        }
    }
    BindTaskManager();
    RefreshInteractions();
    OnRep_Action();
}

void ADeliveryGrandpa::OnRep_FrontDoor()
{
    if (IsValid(KnockInteraction)) return;
    if (IsValid(FrontDoor))
    {
        FrontDoor->OnDoorBroken.AddUniqueDynamic(this, &ADeliveryGrandpa::HandleDoorBroken);
        // 交互组件创建在门上，现有玩家交互 RPC 仍负责距离和长按复核
        KnockInteraction = NewObject<UDeliveryInteractableComponent>(FrontDoor, TEXT("GrandpaKnock"));
        KnockInteraction->InteractionKey = EDeliveryInteractionKey::PickupE;
        KnockInteraction->HoldDuration = .35f;
        KnockInteraction->InteractRadius = 180.f;
        KnockInteraction->PromptOffset = FVector(0, 0, 55);
        // 大门使用复杂碰撞，ClosestPoint 不保证可用，锚点固定在门底上方一米
        KnockAnchor = NewObject<USceneComponent>(FrontDoor, TEXT("GrandpaKnockAnchor"));
        KnockAnchor->SetupAttachment(FrontDoor->GetRootComponent());
        KnockAnchor->RegisterComponent();
        const auto* DoorMesh = FrontDoor->GetIntactMesh();
        const FBox Bounds = DoorMesh->GetStaticMesh()->GetBoundingBox();
        const FVector Bottom = DoorMesh->GetComponentTransform().TransformPosition(
            FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.Min.Z));
        KnockAnchor->SetWorldLocation(Bottom + FVector(0,0,100));
        KnockInteraction->InteractAnchor = KnockAnchor;
        KnockInteraction->CanUse.BindUObject(this, &ADeliveryGrandpa::CanKnock);
        KnockInteraction->Use.BindUObject(this, &ADeliveryGrandpa::Knock);
        KnockInteraction->RegisterComponent();
    }
    UpdateBlackboard();
    RefreshInteractions();
}

void ADeliveryGrandpa::BindTaskManager()
{
    TaskManager = UDeliveryTaskManagerComponent::Get(this);
    if (!TaskManager)
    {
        GetWorldTimerManager().SetTimer(BindTimer, this, &ADeliveryGrandpa::BindTaskManager, .5f, false);
        return;
    }
    TaskManager->OnTaskStatusChanged.AddUniqueDynamic(this, &ADeliveryGrandpa::HandleTaskChanged);
    HandleTaskChanged(DeliveryTarget->ExpectedTask, TaskManager->GetTaskStatus(DeliveryTarget->ExpectedTask));
}

void ADeliveryGrandpa::EndPlay(const EEndPlayReason::Type Reason)
{
    GetWorldTimerManager().ClearTimer(BindTimer);
    if (IsValid(TaskManager)) TaskManager->OnTaskStatusChanged.RemoveDynamic(this, &ADeliveryGrandpa::HandleTaskChanged);
    if (IsValid(FrontDoor)) FrontDoor->OnDoorBroken.RemoveDynamic(this, &ADeliveryGrandpa::HandleDoorBroken);
    if (IsValid(KnockInteraction))
    {
        KnockInteraction->CanUse.Unbind();
        KnockInteraction->Use.Unbind();
        KnockInteraction->DestroyComponent();
    }
    if (IsValid(KnockAnchor)) KnockAnchor->DestroyComponent();
    DeliveryTarget->DeliveryGate.Unbind();
    if (AAIController* AI = Cast<AAIController>(GetController()))
        if (AI->GetBrainComponent()) AI->GetBrainComponent()->StopLogic(TEXT("Grandpa EndPlay"));
    Super::EndPlay(Reason);
}

void ADeliveryGrandpa::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ADeliveryGrandpa, FrontDoor);
    DOREPLIFETIME(ADeliveryGrandpa, KnockCount);
    DOREPLIFETIME(ADeliveryGrandpa, bDelivered);
    DOREPLIFETIME(ADeliveryGrandpa, CurrentAction);
    DOREPLIFETIME(ADeliveryGrandpa, bWalking);
}

bool ADeliveryGrandpa::IsDoorBroken() const { return IsValid(FrontDoor) && FrontDoor->IsBroken(); }

bool ADeliveryGrandpa::CanKnock(APawn* Interactor, bool) const
{
    const auto* Player = Cast<ADeliveryCharacter>(Interactor);
    const auto* Manager = UDeliveryTaskManagerComponent::Get(this);
    return Player && Player->GetWorld() == GetWorld() && !Player->IsIncapacitated() &&
        !Player->IsRidingVehicle() && IsValid(FrontDoor) && !IsDoorBroken() && !bDelivered &&
        Manager && Manager->GetTaskStatus(DeliveryTarget->ExpectedTask) == EDeliveryTaskStatus::InProgress;
}

bool ADeliveryGrandpa::Knock(APawn* Interactor)
{
    if (!HasAuthority() || !CanKnock(Interactor, true) || !IsValid(KnockInteraction) ||
        !KnockInteraction->CanInteract(Interactor)) return false;
    const float Now = GetWorld()->GetTimeSeconds();
    if (Now - LastKnockTime < KnockInterval) return false;
    LastKnockTime = Now;
    // 第一次无回应，第二次回应但永远不开门，后续敲门不重播或重置任务时钟
    KnockCount = FMath::Min(KnockCount + 1, 3);
    if (KnockCount == 2) bReplyPending = true;
    OnRep_State();
    ForceNetUpdate();
    UE_LOG(LogDelivery, Log, TEXT("Grandpa knock=%d doorBroken=%d"), KnockCount, IsDoorBroken());
    return true;
}

bool ADeliveryGrandpa::CanReceive(AActor* Package, APlayerState* Player) const
{
    if (!IsDoorBroken() || bDelivered || !IsValid(Package) || !Player || Player->GetWorld() != GetWorld()) return false;
    const auto* Holder = Cast<ADeliveryCharacter>(Package->GetOwner());
    if (!Holder || Holder->GetTaskPlayerState() != Player || Holder->GetWorld() != GetWorld() ||
        Holder->IsIncapacitated() || Holder->IsRidingVehicle() || Holder->GetCarriedTaskPackage() != Package) return false;
    if (FVector::Dist(Holder->GetActorLocation(), GetActorLocation()) > DeliveryTarget->InteractRadius + 30.f) return false;
    // 即使门已破，也不能在外墙另一边隔墙完成交付
    FCollisionQueryParams Params(SCENE_QUERY_STAT(GrandpaDelivery), false, this);
    Params.AddIgnoredActor(Holder);
    Params.AddIgnoredActor(Package);
    FHitResult Hit;
    return !GetWorld()->LineTraceSingleByChannel(Hit, Holder->GetActorLocation() + FVector(0,0,25),
        GetActorLocation() + FVector(0,0,25), ECC_Visibility, Params);
}

void ADeliveryGrandpa::HandleTaskChanged(UDeliveryTaskDefinition* Task, EDeliveryTaskStatus Status)
{
    if (Task != DeliveryTarget->ExpectedTask) return;
    if (HasAuthority())
    {
        bDelivered = Status == EDeliveryTaskStatus::Completed;
        UpdateBlackboard();
        ForceNetUpdate();
    }
    RefreshInteractions();
}

void ADeliveryGrandpa::HandleDoorBroken()
{
    UpdateBlackboard();
    RefreshInteractions();
}

void ADeliveryGrandpa::UpdateBlackboard()
{
    if (!HasAuthority()) return;
    if (AAIController* AI = Cast<AAIController>(GetController()))
        if (UBlackboardComponent* BB = AI->GetBlackboardComponent())
        {
            BB->SetValueAsBool(TEXT("Delivered"), bDelivered);
            BB->SetValueAsBool(TEXT("DoorBroken"), IsDoorBroken());
            BB->SetValueAsBool(TEXT("ReplyPending"), bReplyPending);
        }
}

void ADeliveryGrandpa::RefreshInteractions()
{
    ReceiveInteraction->bInteractEnabled = IsDoorBroken() && !bDelivered;
    if (IsValid(KnockInteraction))
    {
        KnockInteraction->bInteractEnabled = !IsDoorBroken() && !bDelivered;
        KnockInteraction->PromptText = KnockCount >= 2
            ? NSLOCTEXT("Grandpa", "StillClosed", "Grandpa: Wait a moment!  The door stays shut. Find the axe.")
            : KnockCount == 1 ? NSLOCTEXT("Grandpa", "KnockAgain", "No answer... [Hold E] Knock again")
            : NSLOCTEXT("Grandpa", "Knock", "[Hold E] Knock on Grandpa's door");
    }
}

void ADeliveryGrandpa::OnRep_State() { UpdateBlackboard(); RefreshInteractions(); }

void ADeliveryGrandpa::ApplyBehaviorAction(EDeliveryGrandpaAction Action)
{
    if (!HasAuthority()) return;
    if (Action == EDeliveryGrandpaAction::FinishReply)
    {
        bReplyPending = false;
        UpdateBlackboard();
        return;
    }
    if (CurrentAction == Action) return;
    CurrentAction = Action;
    bWalking = Action == EDeliveryGrandpaAction::StrollOut || Action == EDeliveryGrandpaAction::StrollHome;
    bMotionActive = bWalking || Action == EDeliveryGrandpaAction::Startled;
    if (bWalking) MotionTarget = Action == EDeliveryGrandpaAction::StrollOut ? StrollLocation : HomeLocation;
    else if (Action == EDeliveryGrandpaAction::Startled && IsValid(FrontDoor))
        MotionTarget = GetActorLocation() + (GetActorLocation() - FrontDoor->GetActorLocation()).GetSafeNormal2D() * 35.f;
    else bMotionActive = false;
    if (!bWalking && IsValid(FrontDoor))
        SetActorRotation((FrontDoor->GetActorLocation() - GetActorLocation()).GetSafeNormal2D().Rotation());
    OnRep_Action();
    ForceNetUpdate();
}

void ADeliveryGrandpa::OnRep_Action()
{
    UAnimSequence* Clip = IdleAnimation;
    bool bLoop = true;
    FText Line;
    switch (CurrentAction)
    {
    case EDeliveryGrandpaAction::LookAround:
        Clip = LookAnimation ? LookAnimation.Get() : IdleAnimation.Get(); bLoop = false; break;
    case EDeliveryGrandpaAction::StrollOut:
    case EDeliveryGrandpaAction::StrollHome:
        Clip = bWalking && WalkAnimation ? WalkAnimation.Get() : IdleAnimation.Get(); break;
    case EDeliveryGrandpaAction::Reply:
        Clip = TalkAnimation; Line = NSLOCTEXT("Grandpa", "Reply", "Wait a moment!"); break;
    case EDeliveryGrandpaAction::Startled:
        Clip = StartledAnimation; bLoop = false; Line = NSLOCTEXT("Grandpa", "Broken", "My door! What are you doing?!"); break;
    case EDeliveryGrandpaAction::Receive:
        Line = NSLOCTEXT("Grandpa", "Hungry", "Well, where is my food?"); break;
    case EDeliveryGrandpaAction::Thanks:
        Clip = ReceiveAnimation; bLoop = false; Line = NSLOCTEXT("Grandpa", "Thanks", "Finally! Thank you."); break;
    case EDeliveryGrandpaAction::Finished:
        Line = NSLOCTEXT("Grandpa", "Done", "Delivery received"); break;
    default: break;
    }
    if (Clip)
    {
        Body->PlayAnimation(Clip, bLoop);
        Body->SetPlayRate(CurrentAction == EDeliveryGrandpaAction::Startled ? .65f : 1.f);
    }
    Speech->SetText(Line);
    Speech->SetVisibility(!Line.IsEmpty());
}

// 行为树决定何时走动；服务器只在屋内两点之间扫掠移动，不穿家具、不离开地板
void ADeliveryGrandpa::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!HasAuthority() || !bMotionActive) return;
    const FVector Delta = (MotionTarget - GetActorLocation()).GetSafeNormal2D() * StrollSpeed * FMath::Min(DeltaSeconds, .1f);
    if (FVector::DistSquared2D(MotionTarget, GetActorLocation()) <= FMath::Square(Delta.Size() + 1.f))
    {
        bMotionActive = false;
        if (bWalking) { bWalking = false; OnRep_Action(); ForceNetUpdate(); } return;
    }
    if (bWalking) SetActorRotation(FMath::RInterpConstantTo(GetActorRotation(), Delta.Rotation(), DeltaSeconds, 180.f));
    FCollisionQueryParams Params(SCENE_QUERY_STAT(GrandpaFloor), false, this);
    FHitResult Floor, Block;
    const FVector Next = GetActorLocation() + Delta;
    if (!GetWorld()->LineTraceSingleByChannel(Floor, Next, Next - FVector(0,0,85), ECC_Visibility, Params))
    {
        bMotionActive = false;
        if (bWalking) { bWalking = false; OnRep_Action(); ForceNetUpdate(); } return;
    }
    AddActorWorldOffset(Delta, true, &Block);
    if (Block.bBlockingHit)
    {
        bMotionActive = false;
        if (bWalking) { bWalking = false; OnRep_Action(); ForceNetUpdate(); }
    }
}
