#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Characters/DeliveryNpcAIController.h"
#include "Carry/DeliveryPackageCarrierInterface.h"
#include "Character/DeliveryCharacter.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "NPC/DeliveryGrandpa.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Tracking/DeliveryTaskTrackerComponent.h"
#include "Task/World/DeliveryBreakableDoor.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/World/DeliveryPickupPointComponent.h"
#include "Task/World/DeliveryTargetComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Vehicle/DeliveryVehicleSummonComponent.h"
#include "Vehicle/DeliveryCompanionMotorbike.h"
#include "World/DeliveryLocationRegistry.h"
#include "World/DeliveryHome.h"
#include "AIController.h"
#include "Animation/AnimSequence.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "NavLinkCustomComponent.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "Navigation/PathFollowingComponent.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"

namespace
{
    constexpr int32 MaxRejectedDeliveryStands=4;
    constexpr float RejectedDeliveryStandRadius=55.f;
    constexpr float RejectedDeliveryReferenceChangeDistance=35.f;
    constexpr float ErrandMoveAcceptanceRadius=20.f;
    constexpr float DoorWaitReach=150.f;
    constexpr double GroundPackageFloorTolerance=45.;
    const FName ErrandHitReaction(TEXT("HitReaction"));

    struct FDeliveryDoorStreet
    {
        FVector DoorCenter;
        FVector Address;
        FVector Outward;
        double LowFloor;
        double HighFloor;

        FVector GetWaitGoal(float Clearance) const { return Address+Outward*(Clearance+90.f); }
    };

    TOptional<FDeliveryDoorStreet> ResolveDeliveryDoorStreet(const ADeliveryHome& Home)
    {
        const auto* Grandpa=Home.FormerResident.Get();
        const auto* Door=IsValid(Grandpa)?Grandpa->FrontDoor.Get():nullptr;
        if (!IsValid(Door) || !IsValid(Home.ReturnAnchor) || Door->GetWorld()!=Home.GetWorld()) return {};
        const FVector Center=Door->GetActorLocation();
        const FVector Address=Home.ReturnAnchor->GetComponentLocation();
        if (Center.ContainsNaN() || Address.ContainsNaN()) return {};
        FVector Outward=(Address-Center).GetSafeNormal2D();
        if (Outward.IsNearlyZero()) return {};
        constexpr float FloorTolerance=60.f;
        double LowFloor=Address.Z-FloorTolerance;
        double HighFloor=Address.Z+FloorTolerance;
        if (const auto* Link=Home.DoorwayLink.Get(); IsValid(Link) && Link->IsRegistered())
        {
            const FVector Start=Link->GetStartPoint(), End=Link->GetEndPoint();
            const FVector Axis=(End-Start).GetSafeNormal2D();
            const double AddressSide=FVector::DotProduct(Address-Center,Axis);
            if (!Start.ContainsNaN() && !End.ContainsNaN() && FMath::Abs(AddressSide)>20.)
            {
                Outward=AddressSide>0.?Axis:-Axis;
                LowFloor=FMath::Min(LowFloor,FMath::Min(Start.Z,End.Z)-FloorTolerance);
                HighFloor=FMath::Max(HighFloor,FMath::Max(Start.Z,End.Z)+FloorTolerance);
            }
        }
        return FDeliveryDoorStreet{Center,Address,Outward,LowFloor,HighFloor};
    }

    TOptional<FVector> GetDeliveryStandReference(const UDeliveryTargetComponent& Delivery)
    {
        const auto* Receiver=Delivery.GetOwner();
        if (!IsValid(Receiver) || Receiver->IsActorBeingDestroyed()) return {};
        const auto* Interaction=Receiver->FindComponentByClass<UDeliveryInteractableComponent>();
        const auto* Anchor=Interaction?Interaction->InteractAnchor.Get():nullptr;
        // 无关卡锚点时用收件人的原点，朝两侧取到的胶囊表面只用于实际交互
        const FVector Reference=IsValid(Anchor) && Anchor->GetWorld()==Receiver->GetWorld()?
            Anchor->GetComponentLocation():Receiver->GetActorLocation();
        return Reference.ContainsNaN()?TOptional<FVector>():TOptional<FVector>(Reference);
    }

    bool HasClearErrandApproach(const ADeliveryCompanionCharacter* Npc,const FVector& From,const FVector& Goal,AActor* Target)
    {
        FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionErrandApproach),false,Npc);
        if (Target && Target->IsA<APawn>()) Query.AddIgnoredActor(Target);
        if (const auto* Item=Npc->Companion->GetCarriedItem()) Query.AddIgnoredActor(Item);
        if (const auto* Package=Npc->Companion->GetErrand().Package.Get(); IsValid(Package)) Query.AddIgnoredActor(Package);
        FCollisionObjectQueryParams Objects;
        for (const auto Channel:{ECC_WorldStatic,ECC_WorldDynamic,ECC_Pawn,ECC_Vehicle,ECC_PhysicsBody}) Objects.AddObjectTypesToQuery(Channel);
        FHitResult Hit;
        return !Npc->GetWorld()->LineTraceSingleByObjectType(Hit,From,Goal,Objects,Query) ||
            FVector::DistSquared(Hit.ImpactPoint,Goal)<=FMath::Square(2.f);
    }

    UDeliveryTaskDefinition* PackageTask(const ADeliveryPackageItem* Package)
    {
        const auto* Item=IsValid(Package)?Package->GetDeliveryItemComponent():nullptr;
        return Item?Item->OwningTask.Get():nullptr;
    }

    bool IsNearGroundPackageFloor(const ADeliveryPackageItem& Package,const FVector& Feet)
    {
        const auto* Mesh=Package.GetItemMesh();
        return IsValid(Mesh) && Mesh->IsRegistered() &&
            FMath::Abs(Feet.Z-(Mesh->Bounds.Origin.Z-Mesh->Bounds.BoxExtent.Z))<=GroundPackageFloorTolerance;
    }

    AActor* FindErrandLocation(const UObject* Context,const UDeliveryTaskDefinition* Task,bool bPickup)
    {
        const auto* Registry=UDeliveryLocationRegistry::Get(Context);
        if (!Registry || !IsValid(Task)) return nullptr;
        if (!bPickup && !Task->ReceiverNpcId.IsNone())
        {
            // 门外地址用于引导，订单指定的收件人才持有交付组件
            AActor* Receiver=nullptr;
            for (auto* Actor:Registry->ResolveAllActors(Task->ReceiverNpcId))
            {
                if (!IsValid(Actor) || Actor->IsActorBeingDestroyed() || Actor->GetWorld()!=Context->GetWorld()) continue;
                const auto* Target=Actor->FindComponentByClass<UDeliveryTargetComponent>();
                if (!Target || Target->ExpectedTask!=Task) continue;
                if (Receiver) return nullptr;
                Receiver=Actor;
            }
            return Receiver;
        }
        for (auto* Actor:Registry->ResolveAllActors(bPickup?Task->PickupLocationId:Task->DeliveryLocationId))
        {
        if (!IsValid(Actor) || Actor->IsActorBeingDestroyed() || Actor->GetWorld()!=Context->GetWorld()) continue;
            if (bPickup)
            {
                const auto* Point=UDeliveryPickupPointComponent::FindOn(Actor);
                if (Point && Point->ResolvePendingTask()==Task) return Actor;
            }
            else if (const auto* Target=Actor->FindComponentByClass<UDeliveryTargetComponent>(); Target && Target->ExpectedTask==Task)
                return Actor;
        }
        return nullptr;
    }
}

bool UDeliveryCompanionComponent::HasActiveErrand() const
{
    return Errand.Status==EDeliveryNpcActionStatus::Running;
}
bool UDeliveryCompanionComponent::HasErrandPackageInTransit() const
{
    return HasActiveErrand() &&
        (Errand.Phase==EDeliveryCompanionErrandPhase::GoToDelivery || Errand.Phase==EDeliveryCompanionErrandPhase::ReturnToOwner) &&
        IsValid(Errand.Task) && IsValid(Errand.Package) && GetCarriedTaskPackage()==Errand.Package && PackageTask(Errand.Package)==Errand.Task;
}

bool UDeliveryCompanionComponent::HasReceivedErrandPackage() const
{
    return HasErrandPackageInTransit() || (HasActiveErrand() && Errand.Phase==EDeliveryCompanionErrandPhase::ReceiveFromOwner &&
        IsValid(Errand.Task) && IsValid(Errand.Package) && GetCarriedTaskPackage()==Errand.Package &&
        PackageTask(Errand.Package)==Errand.Task && HasCommittedPackageHandoff());
}

APawn* UDeliveryCompanionComponent::GetOwnerPackageCarrier() const
{
    auto* Pawn=GetOwnerPawn();
    auto* RidingBike=Cast<ADeliveryMotorbike>(Pawn);
    auto* Character=RidingBike?RidingBike->GetDriver():Cast<ADeliveryCharacter>(Pawn);
    if (!IsValid(Character) || Character->GetWorld()!=GetWorld() || Character->GetTaskPlayerState()!=OwnerPlayerState) return nullptr;
    // 玩家当前库存优先，车上的原件仍以实际车辆作为转出方
    if (auto* Package=Character->GetCarriedTaskPackage(); IsValid(Package) && Package->GetOwner()==Character) return Character;
    const auto* Summon=Character->FindComponentByClass<UDeliveryVehicleSummonComponent>();
    if (auto* Bike=Summon?Summon->GetRegisteredCargoVehicle():nullptr) return Bike;
    if (RidingBike && Character->GetCarriedTaskPackage()) return nullptr;
    return Character;
}

ADeliveryPackageItem* UDeliveryCompanionComponent::GetCommandPackage() const
{
    if (auto* Package=GetCarriedTaskPackage()) return Package;
    const auto* Carrier=Cast<IDeliveryPackageCarrierInterface>(GetOwnerPackageCarrier());
    auto* Package=Carrier?Carrier->GetCarriedTaskPackage():nullptr;
    if (IsValid(Package)) return Package;
    return const_cast<ADeliveryPackageItem*>(GetOwnerDroppedPackage());
}

const ADeliveryPackageItem* UDeliveryCompanionComponent::GetOwnerDroppedPackage() const
{
    const auto* Pawn=GetOwnerPawn();
    const auto* Bike=Cast<ADeliveryMotorbike>(Pawn);
    const auto* Owner=Bike?Bike->GetDriver():Cast<ADeliveryCharacter>(Pawn);
    if (!IsValid(Owner) || Owner->IsActorBeingDestroyed() || Owner->GetWorld()!=GetWorld() ||
        Owner->GetTaskPlayerState()!=OwnerPlayerState) return nullptr;
    const auto* Inventory=Owner->GetInventoryComponent();
    const auto* Package=Inventory?Inventory->GetDroppedFirstOrderPackage():nullptr;
    if (!IsValid(Package) || !Package->GetActorEnableCollision() || Package->GetActorLocation().ContainsNaN()) return nullptr;
    const auto* Task=PackageTask(Package);
    const auto* Tasks=UDeliveryTaskManagerComponent::Get(this);
    return IsValid(Task) && Task->TaskId==TEXT("Task_001") && Tasks &&
        Tasks->GetTaskStatus(Task)==EDeliveryTaskStatus::InProgress && Tasks->IsTaskAccepted(Task)?Package:nullptr;
}

UDeliveryTaskDefinition* UDeliveryCompanionComponent::GetCommandTask() const
{
    if (auto* Task=PackageTask(GetCommandPackage())) return Task;
    const auto* Tracker=UDeliveryTaskTrackerComponent::FindTracker(OwnerPlayerState);
    return Tracker?Tracker->GetTrackedTask():nullptr;
}

EDeliveryCompanionTaskDoorState UDeliveryCompanionComponent::GetObservedTaskDoorState(const UDeliveryTaskDefinition* Task) const
{
    using EDoorState=EDeliveryCompanionTaskDoorState;
    const auto* Npc=GetCompanion();
    const auto* OwnerPawn=GetOwnerPawn();
    const auto* Bike=Cast<ADeliveryMotorbike>(OwnerPawn);
    const auto* Player=Bike?Bike->GetDriver():Cast<ADeliveryCharacter>(OwnerPawn);
    if (!IsValid(Task) || Task->ReceiverNpcId.IsNone() || !IsValid(Npc) || !Npc->HasAuthority() ||
        Npc->IsActorBeingDestroyed() || Npc->IsHidden() || Npc->IsKnockedDown() ||
        !IsValid(Player) || Player->GetWorld()!=GetWorld() || Player->GetTaskPlayerState()!=OwnerPlayerState) return EDoorState::Unknown;

    const auto* Grandpa=Cast<ADeliveryGrandpa>(FindErrandLocation(this,Task,false));
    const auto* Home=IsValid(Grandpa)?Grandpa->SharedHome.Get():nullptr;
    if (!IsValid(Home) || Home->GetWorld()!=GetWorld() || Home->FormerResident!=Grandpa ||
        ADeliveryHome::FindSharedHome(this)!=Home) return EDoorState::Unknown;
    const auto* Door=Grandpa->FrontDoor.Get();
    const auto* Link=Home->DoorwayLink.Get();
    if (!IsValid(Door) || Door->IsActorBeingDestroyed() || Door->IsHidden() || Door->GetWorld()!=GetWorld() ||
        !IsValid(Link) || !Link->IsRegistered()) return EDoorState::Unknown;
    const auto* Mesh=Door->GetIntactMesh();
    if (!IsValid(Mesh) || !Mesh->IsRegistered() || !Mesh->GetStaticMesh()) return EDoorState::Unknown;

    // 门的资产原点高于实际门叶，同层以导航端点为准，观察点使用门叶中心
    const FVector Center=Mesh->Bounds.Origin;
    const FVector Start=Link->GetStartPoint(),End=Link->GetEndPoint();
    if (Center.ContainsNaN() || Start.ContainsNaN() || End.ContainsNaN()) return EDoorState::Unknown;
    constexpr double Range=500.;
    constexpr double FloorTolerance=75.;
    const auto IsAtEntrance=[&](const FVector& Feet)
    {
        return !Feet.ContainsNaN() && FVector::DistSquared2D(Feet,Center)<=FMath::Square(Range) &&
            FMath::IsWithinInclusive(Feet.Z,FMath::Min(Start.Z,End.Z)-FloorTolerance,FMath::Max(Start.Z,End.Z)+FloorTolerance);
    };
    const FVector PlayerFeet=Player->GetActorLocation()-FVector(0,0,Player->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
    if (!IsAtEntrance(Npc->GetNavAgentLocation()) || !IsAtEntrance(PlayerFeet)) return EDoorState::Unknown;
    FVector Eye; FRotator ViewRotation;
    Npc->GetActorEyesViewPoint(Eye,ViewRotation);
    if (Eye.ContainsNaN()) return EDoorState::Unknown;
    const FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionTaskDoorSight),false,Npc);
    FHitResult Hit;
    if (GetWorld()->LineTraceSingleByChannel(Hit,Eye,Center,ECC_Visibility,Query) && Hit.GetActor()!=Door)
        return EDoorState::Unknown;
    return Door->IsBroken()?EDoorState::Broken:EDoorState::Intact;
}

bool UDeliveryCompanionComponent::CanArrangeErrand(EDeliveryNpcAction Action,APlayerState* Requester) const
{
    const auto* Npc=GetCompanion();
    const auto* Task=GetCommandTask();
    const auto* Manager=UDeliveryTaskManagerComponent::Get(this);
    if (!CanCommand(Requester) || !Npc || Npc->IsKnockedDown() || !Task || !Manager ||
        !Manager->IsTaskAccepted(Task) || !Manager->CanAcquireItem(Task)) return false;
    const auto* Held=GetCarriedTaskPackage();
    if (GetCarriedItem() && !Held) return false;
    const auto* OwnerCarrier=Cast<IDeliveryPackageCarrierInterface>(GetOwnerPackageCarrier());
    const auto* OwnerPackage=OwnerCarrier?OwnerCarrier->GetCarriedTaskPackage():nullptr;
    const bool bOwnerHasTask=IsValid(OwnerPackage) && PackageTask(OwnerPackage)==Task;
    const auto* GroundPackage=!Held && !bOwnerHasTask?GetOwnerDroppedPackage():nullptr;
    const bool bCanRecoverGround=IsValid(GroundPackage) && PackageTask(GroundPackage)==Task &&
        GroundPackage->GetDeliveryItemComponent()->CanBeAcquiredBy(Npc);
    if (Action==EDeliveryNpcAction::ReturnPackage || Action==EDeliveryNpcAction::DropPackage ||
        Action==EDeliveryNpcAction::StorePackageInBackpack) return Held && PackageTask(Held)==Task;
    if (Action==EDeliveryNpcAction::TakePackage) return !Held && (bOwnerHasTask || bCanRecoverGround);
    const bool bCanPickup=!Held && !bOwnerHasTask && Manager->GetTaskStatus(Task)==EDeliveryTaskStatus::AwaitingPickup &&
        FindErrandLocation(this,Task,true);
    if (Action==EDeliveryNpcAction::FetchPackage) return bCanPickup || bCanRecoverGround;
    return Action==EDeliveryNpcAction::DeliverPackage && FindErrandLocation(this,Task,false) &&
        ((Held && PackageTask(Held)==Task) || bOwnerHasTask || bCanPickup || bCanRecoverGround);
}

bool UDeliveryCompanionComponent::CanKeepErrandCommand(const FDeliveryNpcPublicState& State) const
{
    if (DeliveryNpc::IsCompanionItemErrand(State.Action))
        return State.ItemChoice.IsValid() && State.ItemChoice->Item.Get()==State.Target &&
            State.ItemChoice->bReturnItem==(State.Action==EDeliveryNpcAction::ReturnItem) &&
            CanKeepItemErrand(*State.ItemChoice,State.Requester,State.CompanionCommandRevision,State.CompanionTravelMode);
    return DeliveryNpc::IsCompanionErrand(State.Action) && CanKeepPackageErrand(State.Action,State.Requester,
        State.CompanionCommandRevision,State.CompanionTask,State.CompanionPackage,State.CompanionTravelMode);
}

bool UDeliveryCompanionComponent::CanKeepPackageErrand(EDeliveryNpcAction Action,APlayerState* Requester,int32 ExpectedRevision,
    UDeliveryTaskDefinition* ExpectedTask,ADeliveryPackageItem* ExpectedPackage,EDeliveryCompanionTravelMode RequestedTravelMode) const
{
    const auto* Npc=GetCompanion();
    if (!IsValid(Npc) || !Npc->HasAuthority() || Npc->IsActorBeingDestroyed() || bChangingHomeArrangement ||
        !HasActiveErrand() || ExpectedRevision!=CommandRevision || Errand.CommandRevision!=CommandRevision ||
        Errand.Action!=Action || !IsValid(ExpectedTask) || Errand.Task!=ExpectedTask || Errand.Package!=ExpectedPackage ||
        (RequestedTravelMode!=EDeliveryCompanionTravelMode::Unspecified && RequestedTravelMode!=TravelMode) ||
        !CanArrangeErrand(Action,Requester) || GetCommandTask()!=ExpectedTask || GetCommandPackage()!=ExpectedPackage) return false;

    // 同一个包裹仍须位于当前阶段的真实交接一侧
    if (Errand.Phase==EDeliveryCompanionErrandPhase::GoToPickup)
        return Errand.bRecoverGroundPackage?IsValid(ExpectedPackage) && !GetCarriedTaskPackage() && GetOwnerDroppedPackage()==ExpectedPackage:
            ExpectedPackage==nullptr;
    if (!IsValid(ExpectedPackage)) return false;
    const auto* Held=GetCarriedTaskPackage();
    if (Errand.Phase==EDeliveryCompanionErrandPhase::ReceiveFromOwner)
    {
        if (!Held) return true;
        return Held==ExpectedPackage && HasCommittedPackageHandoff();
    }
    return Held==ExpectedPackage && (Errand.Phase==EDeliveryCompanionErrandPhase::GoToDelivery ||
        Errand.Phase==EDeliveryCompanionErrandPhase::ReturnToOwner);
}

bool UDeliveryCompanionComponent::StartErrand(EDeliveryNpcAction Action,APlayerState* Requester,int32 ExpectedRevision,
    UDeliveryTaskDefinition* ExpectedTask,ADeliveryPackageItem* ExpectedPackage,EDeliveryCompanionTravelMode RequestedTravelMode)
{
    if (CanKeepPackageErrand(Action,Requester,ExpectedRevision,ExpectedTask,ExpectedPackage,RequestedTravelMode)) return true;
    auto* Npc=GetCompanion();
    if (!IsValid(Npc) || Npc->IsActorBeingDestroyed() || !Npc->HasAuthority() ||
        ExpectedRevision!=CommandRevision || !CanArrangeErrand(Action,Requester) ||
        !IsValid(ExpectedTask) || ExpectedTask!=GetCommandTask() || ExpectedPackage!=GetCommandPackage()) return false;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const TWeakObjectPtr<APlayerState> WeakRequester(Requester);
    const TWeakObjectPtr<UDeliveryTaskDefinition> WeakTask(ExpectedTask);
    const TWeakObjectPtr<ADeliveryPackageItem> WeakPackage(ExpectedPackage);
    const bool bExpectedPackage=ExpectedPackage!=nullptr;
    int32 AcceptedRevision=ExpectedRevision;
    const auto StillCurrent=[&]()
    {
        return WeakThis.IsValid() && WeakNpc.IsValid() && !Npc->IsActorBeingDestroyed() &&
            WeakRequester.IsValid() && WeakTask.IsValid() && CommandRevision==AcceptedRevision &&
            (!bExpectedPackage || WeakPackage.IsValid()) && CanArrangeErrand(Action,WeakRequester.Get()) &&
            GetCommandTask()==WeakTask.Get() && GetCommandPackage()==WeakPackage.Get();
    };
    // 每次取消都可能同步触发新安排，迟到的旧调用不能继续覆盖新指令或出行偏好
    CancelHomeArrangement(TEXT("OwnerChangedArrangement"));
    if (!StillCurrent()) return false;
    if (HasActiveErrand())
    {
        const int32 OldErrandRevision=Errand.CommandRevision;
        FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("ReplacedByOwner"));
        if (!WeakThis.IsValid() || !WeakNpc.IsValid() || Npc->IsActorBeingDestroyed()) return false;
        // 只接纳这次结束旧委托自身产生的一次修订，回调中的新安排优先
        ++AcceptedRevision;
        if (Errand.CommandRevision!=OldErrandRevision || Errand.Status!=EDeliveryNpcActionStatus::Cancelled ||
            Errand.Reason!=TEXT("ReplacedByOwner") || !StillCurrent()) return false;
    }
    StopFollowMovement();
    if (!StillCurrent()) return false;
    StopErrandMovement();
    if (!StillCurrent()) return false;
    ++CommandRevision;
    ApplyTravelInstruction(RequestedTravelMode);
    Errand={};
    ResetDeliveryStandRejections();
    Errand.Action=Action;
    Errand.Status=EDeliveryNpcActionStatus::Running;
    Errand.Task=ExpectedTask;
    Errand.Package=ExpectedPackage;
    Errand.CommandRevision=CommandRevision;
    Errand.bRecoverGroundPackage=IsValid(ExpectedPackage) && ExpectedPackage==GetOwnerDroppedPackage();
    if (Action==EDeliveryNpcAction::ReturnPackage || Action==EDeliveryNpcAction::DropPackage ||
        Action==EDeliveryNpcAction::StorePackageInBackpack) Errand.Phase=EDeliveryCompanionErrandPhase::ReturnToOwner;
    else if (GetCarriedTaskPackage()) Errand.Phase=EDeliveryCompanionErrandPhase::GoToDelivery;
    else if (ExpectedPackage && !Errand.bRecoverGroundPackage) Errand.Phase=EDeliveryCompanionErrandPhase::ReceiveFromOwner;
    else Errand.Phase=EDeliveryCompanionErrandPhase::GoToPickup;
    ErrandActiveSeconds=0;
    ErrandTimeLimitSeconds=GetTuning().ErrandTimeoutSeconds;
    bErrandTravelBudgetAdded=false;
    bErrandVehicleApproachBudgetAdded=false;
    ErrandInteractionSeconds=0;
    LastErrandPause=NAME_None;
    bHasErrandDestination=false;
    NextErrandNavigationAt=0;
    SetSuspension(NAME_None);
    GetOwner()->ForceNetUpdate();
    return true;
}

bool UDeliveryCompanionComponent::HasCommittedPackageHandoff() const
{
    if (!ErrandCargoHandoffId.IsValid()) return false;
    if (bErrandCargoHandoffCommitted) return true;
    const auto* Bike=ErrandCargoVehicle.Get();
    bool bComplete=false,bSucceeded=false;
    FName Reason;
    return IsValid(Bike) && Bike->GetCargoHandoffResult(ErrandCargoHandoffId,bComplete,bSucceeded,Reason) &&
        bComplete && bSucceeded;
}

void UDeliveryCompanionComponent::CancelPackageHandoff()
{
    auto* Bike=ErrandCargoVehicle.Get();
    const FGuid OldId=ErrandCargoHandoffId;
    ErrandCargoVehicle.Reset();
    ErrandCargoHandoffId.Invalidate();
    bErrandCargoHandoffCommitted=false;
    NextCargoHandoffAt=0.;
    if (IsValid(Bike) && OldId.IsValid()) Bike->CancelCargoHandoff(OldId);
}

void UDeliveryCompanionComponent::StopErrandMovement(bool bKeepVehicleSeat)
{
    if (!GetOwner() || !GetOwner()->HasAuthority()) return;
    // 取消交接、技能或换车都可能同步结束旧路径，先停止监听收件路线
    ClearDeliveryApproachWatch();
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const int32 Revision=CommandRevision, ErrandRevision=Errand.CommandRevision;
    const auto StillCurrent=[&]() { return WeakThis.IsValid() && CommandRevision==Revision && Errand.CommandRevision==ErrandRevision; };
    StopOwnerHandoffFollow();
    if (!StillCurrent()) return;
    // 移动暂停可以中止正在开的箱，但服务器已提交的原交接留给委托读取
    if (HasCommittedPackageHandoff()) bErrandCargoHandoffCommitted=true;
    else CancelPackageHandoff();
    if (!StillCurrent()) return;
    CancelItemUse();
    // 取消技能和导航都会同步通知外部，旧停止不能清掉回调中新接的安排
    if (!StillCurrent()) return;
    StopVehicleTravel(EDeliveryCompanionTravelPurpose::Errand,bKeepVehicleSeat && CanKeepVehicleSeatWhilePaused());
    if (!StillCurrent()) return;
    LastErrandUpdateAt=0;
    ItemSearchLookStartedAt=-1;
    if (GroundPickupAlignmentStartedAt>=0 || Errand.GroundPickupServerTime>=0)
    {
        bHasErrandDestination=false;
        NextErrandNavigationAt=0;
    }
    GroundPickupAlignmentStartedAt=-1;
    if (Errand.GroundPlacementServerTime>=0)
    {
        Errand.GroundPlacementServerTime=-1;
        GetOwner()->ForceNetUpdate();
    }
    if (Errand.LowItemInspectionServerTime>=0 || Errand.GroundPickupServerTime>=0)
    {
        Errand.LowItemInspectionServerTime=-1;
        Errand.GroundPickupServerTime=-1;
        GetOwner()->ForceNetUpdate();
    }
    const auto* Npc=GetCompanion();
    auto* AI=Npc?Cast<AAIController>(Npc->GetController()):nullptr;
    if (ItemSearchFocus.IsSet())
    {
        if (AI && AI->GetFocalPointForPriority(EAIFocusPriority::Gameplay)==ItemSearchFocus.GetValue())
            AI->ClearFocus(EAIFocusPriority::Gameplay);
        ItemSearchFocus.Reset();
    }
    if (!bErrandOwnsNavigation) return;
    const FAIRequestID OldId=ErrandMoveId;
    bErrandOwnsNavigation=false;
    ErrandMoveId=FAIRequestID::InvalidRequest;
    if (auto* Path=AI?AI->GetPathFollowingComponent():nullptr; Path && OldId.IsValid() && Path->GetCurrentRequestId()==OldId)
        Path->AbortMove(*this,FPathFollowingResultFlags::UserAbort,OldId);
}

void UDeliveryCompanionComponent::StopOwnerHandoffFollow()
{
    if (OwnerHandoffFollowRevision==INDEX_NONE) return;
    OwnerHandoffFollowRevision=INDEX_NONE;
    bHasFollowDestination=false;
    NextNavigationAt=0;
    NextVehicleNavigationAt=0;
    StopFollowMovement();
}

void UDeliveryCompanionComponent::SetErrandPhase(EDeliveryCompanionErrandPhase Phase)
{
    if (!HasActiveErrand() || !GetOwner() || !GetOwner()->HasAuthority()) return;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const int32 Revision=CommandRevision, ErrandRevision=Errand.CommandRevision;
    StopErrandMovement();
    if (!WeakThis.IsValid() || !HasActiveErrand() || CommandRevision!=Revision || Errand.CommandRevision!=ErrandRevision) return;
    Errand.Phase=Phase;
    Errand.Reason=NAME_None;
    ResetDeliveryStandRejections();
    ErrandInteractionSeconds=0;
    bHasErrandDestination=false;
    bErrandTravelBudgetAdded=false;
    bErrandVehicleApproachBudgetAdded=false;
    NextErrandNavigationAt=0;
    LastErrandPause=NAME_None;
    SetSuspension(NAME_None);
    GetOwner()->ForceNetUpdate();
}

void UDeliveryCompanionComponent::FinishErrand(EDeliveryNpcActionStatus Status,FName Reason)
{
    if (!HasActiveErrand() || !GetOwner() || !GetOwner()->HasAuthority()) return;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const int32 Revision=CommandRevision, ErrandRevision=Errand.CommandRevision;
    CancelPackageHandoff();
    if (!WeakThis.IsValid() || !HasActiveErrand() || CommandRevision!=Revision || Errand.CommandRevision!=ErrandRevision) return;
    StopErrandMovement();
    if (!WeakThis.IsValid() || !HasActiveErrand() || CommandRevision!=Revision || Errand.CommandRevision!=ErrandRevision) return;
    Errand.Status=Status;
    Errand.Phase=EDeliveryCompanionErrandPhase::Idle;
    Errand.Reason=Reason;
    ResetDeliveryStandRejections();
    Errand.Package=nullptr;
    Errand.Item.Reset();
    const TArray<FString> SearchSightings=MoveTemp(AreaSearchSeenNames);
    AreaSearchSeenNames.Reset();
    bAreaSearchReachedStand=false;
    ++CommandRevision;
    if (Mode==EDeliveryCompanionMode::Stay && !bCaptureWaitLocationOnFoot)
    {
        const FVector Feet=GetCompanion()->GetNavAgentLocation();
        bReturningToWaitLocation=IsUsingErrandVehicle() || FVector::Dist2D(Feet,WaitLocation)>40.f || FMath::Abs(Feet.Z-WaitLocation.Z)>60.f;
    }
    if (Reason!=TEXT("OwnerLost")) ResumeIndoorFollowAfterErrand();
    bHasFollowDestination=false;
    NextNavigationAt=0;
    // 实际订单在此结束，之后按原持续安排返程，保留主人指定的等候点
    SetSuspension(NAME_None);
    GetOwner()->ForceNetUpdate();
    if (Status==EDeliveryNpcActionStatus::Succeeded && Reason==TEXT("Delivered"))
    {
        // 任务系统已保存实际签收，委托收尾只安排后续活动
        GetCompanion()->WakeDecision();
        return;
    }
    if (Errand.Action==EDeliveryNpcAction::SearchNearOwner)
    {
        const FString Summary=Reason==TEXT("ItemSearchAreaChecked")?
            (SearchSightings.IsEmpty()?TEXT("到过可达的查看点，检查时没看见可取的道具。"):
                FString::Printf(TEXT("在查看时亲眼看见道具：%s。"),*FString::Join(SearchSightings,TEXT("、")))):
            FString::Printf(TEXT("查找没有完成，原因 %s。"),*Reason.ToString());
        GetCompanion()->ObserveFact(TEXT("CompanionAreaSearchFinished"),GetOwnerPawn(),
            Summary+TEXT(" 本次只查看，没有拿取或交接任何物品。"),EDeliveryNpcEvidenceSource::ActionResult);
        return;
    }
    if (DeliveryNpc::IsCompanionItemErrand(Errand.Action))
    {
        GetCompanion()->ObserveFact(TEXT("CompanionItemErrandFinished"),GetOwnerPawn(),
            FString::Printf(TEXT("游戏确认二狗的普通道具 %s 安排结束：%s，原因 %s。二狗当前%s实物；只有 ItemReturnedToOwner 表示同一件物品已实际进入主人库存，没有订单交付或奖励。"),
                *DeliveryNpc::ActionName(Errand.Action),*StaticEnum<EDeliveryNpcActionStatus>()->GetNameStringByValue(static_cast<int64>(Status)),
                *Reason.ToString(),GetCarriedItem()?TEXT("仍持有"):TEXT("未持有")),EDeliveryNpcEvidenceSource::ActionResult);
        return;
    }
    // 只根据同步物品接口的实际结果报告，接受委托、走到附近均不算取件或交付成功
    GetCompanion()->ObserveFact(TEXT("CompanionErrandFinished"),GetOwnerPawn(),
        FString::Printf(TEXT("游戏确认二狗的 %s 委托已结束：%s，原因 %s，订单 %s。当前二狗%s实际包裹；签收与奖励由任务系统确认。"),
            *DeliveryNpc::ActionName(Errand.Action),*StaticEnum<EDeliveryNpcActionStatus>()->GetNameStringByValue(static_cast<int64>(Status)),
            *Reason.ToString(),Errand.Task?*Errand.Task->TaskId.ToString():TEXT("未知"),GetCarriedTaskPackage()?TEXT("仍持有"):TEXT("没有持有")),
        EDeliveryNpcEvidenceSource::ActionResult);
}

void UDeliveryCompanionComponent::ReportErrandPause(FName Reason,bool bKeepMounted)
{
    if (!HasActiveErrand() || !GetOwner() || !GetOwner()->HasAuthority()) return;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const int32 Revision=CommandRevision, ErrandRevision=Errand.CommandRevision;
    if (bKeepMounted && IsUsingErrandVehicle())
    {
        // 当前阶段仍在骑行，停止旧路线并保留同一件货物，等待下一轮找停车位
        bVehicleHasDriveGoal=false;
        ErrandVehicle->StopDriving();
    }
    else StopErrandMovement();
    if (!WeakThis.IsValid() || !HasActiveErrand() || CommandRevision!=Revision || Errand.CommandRevision!=ErrandRevision) return;
    SetSuspension(Reason);
    if (LastErrandPause==Reason) return;
    LastErrandPause=Reason;
    GetCompanion()->ObserveFact(TEXT("CompanionErrandPaused"),GetOwnerPawn(),
        FString::Printf(TEXT("当前跑腿暂时停下：%s。委托和实际携带的物品仍保留，没有确认交接或送达；条件恢复后继续，主人也可以改安排。"),*Reason.ToString()),
        EDeliveryNpcEvidenceSource::ActionResult);
}

AActor* UDeliveryCompanionComponent::ResolveErrandLocation(bool bPickup) const
{
    return FindErrandLocation(this,Errand.Task,bPickup);
}

void UDeliveryCompanionComponent::ResetDeliveryStandRejections()
{
    RejectedDeliveryStands.Reset();
    RejectedDeliveryTarget.Reset();
    RejectedDeliveryReference=FVector::ZeroVector;
    ErrandDeliveryTarget.Reset();
}

void UDeliveryCompanionComponent::RefreshDeliveryStandRejections(const UDeliveryTargetComponent& Delivery)
{
    const auto Reference=GetDeliveryStandReference(Delivery);
    if (!Reference.IsSet())
    {
        RejectedDeliveryStands.Reset();
        RejectedDeliveryTarget.Reset();
        return;
    }
    if (RejectedDeliveryTarget.Get()==&Delivery && !RejectedDeliveryStands.IsEmpty() &&
        FVector::DistSquared(RejectedDeliveryReference,*Reference)<FMath::Square(RejectedDeliveryReferenceChangeDistance)) return;
    // 保留第一次受阻时的参照点，收件人连续小步移动也会累积到重算距离
    RejectedDeliveryStands.Reset();
    RejectedDeliveryTarget=&Delivery;
    RejectedDeliveryReference=*Reference;
}

void UDeliveryCompanionComponent::RejectDeliveryStand(const UDeliveryTargetComponent& Delivery,const FVector& Stand)
{
    RefreshDeliveryStandRejections(Delivery);
    if (RejectedDeliveryTarget.Get()!=&Delivery) return;
    if (RejectedDeliveryStands.Num()>=MaxRejectedDeliveryStands) RejectedDeliveryStands.RemoveAt(0);
    RejectedDeliveryStands.Add(Stand);
    bHasErrandDestination=false;
}

void UDeliveryCompanionComponent::ClearDeliveryApproachWatch()
{
    if (auto* Path=DeliveryApproachPath.Get(); Path && DeliveryApproachFinishedHandle.IsValid())
        Path->OnRequestFinished.Remove(DeliveryApproachFinishedHandle);
    DeliveryApproachFinishedHandle.Reset();
    DeliveryApproachPath.Reset();
}

void UDeliveryCompanionComponent::WatchDeliveryApproach(UPathFollowingComponent& Path,FAIRequestID MoveId,AActor& Target)
{
    ClearDeliveryApproachWatch();
    auto* Delivery=Target.FindComponentByClass<UDeliveryTargetComponent>();
    if (!IsValid(Delivery)) return;
    const auto Reference=GetDeliveryStandReference(*Delivery);
    if (!Reference.IsSet()) return;
    DeliveryApproachPath=&Path;
    const TWeakObjectPtr<AActor> Receiver(&Target);
    const TWeakObjectPtr<UDeliveryTargetComponent> DeliveryTarget(Delivery);
    const int32 Revision=CommandRevision;
    const FVector Stand=ErrandDestination;
    DeliveryApproachFinishedHandle=Path.OnRequestFinished.AddWeakLambda(this,
        [this,Receiver,DeliveryTarget,MoveId,Revision,ReferenceAtMove=*Reference,Stand](FAIRequestID FinishedId,const FPathFollowingResult& Result)
        {
            if (FinishedId!=MoveId) return;
            // 路径组件已清掉当前编号，使用回调携带的编号核对二狗拥有的步行请求
            const auto* FinishedPath=DeliveryApproachPath.Get();
            const auto* Npc=GetCompanion();
            const auto* AI=Npc?Cast<AAIController>(Npc->GetController()):nullptr;
            if (IsValid(Npc) && !Npc->IsActorBeingDestroyed() && Npc->HasAuthority() &&
                AI && FinishedPath && AI->GetPathFollowingComponent()==FinishedPath &&
                bErrandOwnsNavigation && ErrandMoveId==FinishedId && HasActiveErrand() &&
                CommandRevision==Revision && Errand.CommandRevision==Revision && Errand.Phase==EDeliveryCompanionErrandPhase::GoToDelivery)
            {
                bErrandOwnsNavigation=false;
                ErrandMoveId=FAIRequestID::InvalidRequest;
                if (Result.Code==EPathFollowingResult::Blocked)
                {
                    bHasErrandDestination=false;
                    if (Receiver.IsValid() && !Receiver->IsActorBeingDestroyed() && DeliveryTarget.IsValid() &&
                        ResolveErrandLocation(false)==Receiver.Get() && Receiver->FindComponentByClass<UDeliveryTargetComponent>()==DeliveryTarget.Get())
                    {
                        const auto CurrentReference=GetDeliveryStandReference(*DeliveryTarget.Get());
                        if (CurrentReference.IsSet() && FVector::DistSquared(ReferenceAtMove,*CurrentReference)<
                            FMath::Square(RejectedDeliveryReferenceChangeDistance)) RejectDeliveryStand(*DeliveryTarget.Get(),Stand);
                        else RefreshDeliveryStandRejections(*DeliveryTarget.Get());
                        SetSuspension(TEXT("ErrandPathBlocked"));
                    }
                }
            }
            ClearDeliveryApproachWatch();
        });
}

bool UDeliveryCompanionComponent::IsSafeDeliveryDoorWait(const ADeliveryHome& Home,const FVector& Feet,float AcceptanceMargin) const
{
    const auto Street=ResolveDeliveryDoorStreet(Home);
    const auto* Npc=GetCompanion();
    if (!Street.IsSet() || !IsValid(Npc) || Home.GetWorld()!=GetWorld() || Feet.ContainsNaN()) return false;
    const auto& Settings=GetTuning();
    const double CapsuleRadius=Npc->GetCapsuleComponent()->GetScaledCapsuleRadius();
    const auto* OwnerPawn=GetOwnerPawn();
    return FMath::IsWithinInclusive(Feet.Z,Street->LowFloor,Street->HighFloor) &&
        FVector::DotProduct(Feet-Street->DoorCenter,Street->Outward)>=CapsuleRadius+AcceptanceMargin &&
        FVector::DistSquared2D(Feet,Street->Address)>=FMath::Square(Settings.DestinationClearance+AcceptanceMargin) &&
        (!IsValid(OwnerPawn) || FVector::DistSquared2D(Feet,OwnerPawn->GetActorLocation())>=FMath::Square(Settings.YieldDistance+AcceptanceMargin)) &&
        IsDestinationClear(Feet);
}

bool UDeliveryCompanionComponent::MoveForErrand(const FVector& Goal,float ReachRadius,AActor* InteractionTarget,float PreferredOffset,
    UDeliveryTargetComponent* Delivery,const ADeliveryHome* DoorWaitHome)
{
    auto* Npc=GetCompanion();
    auto* AI=Npc?Cast<AAIController>(Npc->GetController()):nullptr;
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!AI || !Nav || Goal.ContainsNaN()) return false;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const TWeakObjectPtr<AAIController> WeakAI(AI);
    const TWeakObjectPtr<AActor> Target(InteractionTarget);
    const TWeakObjectPtr<UDeliveryTargetComponent> ExpectedDelivery(Delivery);
    const TWeakObjectPtr<const ADeliveryHome> ExpectedDoorWait(DoorWaitHome);
    const int32 Revision=CommandRevision;
    const auto Phase=Errand.Phase;
    const auto* GroundPackage=Phase==EDeliveryCompanionErrandPhase::GoToPickup && Errand.bRecoverGroundPackage?
        Cast<ADeliveryPackageItem>(InteractionTarget):nullptr;
    const auto IsCurrent=[&]()
    {
        return WeakThis.IsValid() && WeakNpc.IsValid() && !Npc->IsActorBeingDestroyed() && WeakAI.IsValid() &&
            Npc->GetController()==AI && (!InteractionTarget || (Target.IsValid() && !Target->IsActorBeingDestroyed())) &&
            HasActiveErrand() && CommandRevision==Revision && Errand.CommandRevision==Revision && Errand.Phase==Phase &&
            (!DoorWaitHome || (ExpectedDoorWait.IsValid() && IsValid(DoorWaitHome->FormerResident) && !DoorWaitHome->FormerResident->IsDoorBroken())) &&
            (!Delivery || (ExpectedDelivery.IsValid() && Target.IsValid() && ResolveErrandLocation(false)==Target.Get() &&
                Target->FindComponentByClass<UDeliveryTargetComponent>()==ExpectedDelivery.Get()));
    };
    const bool bPrecise=PreferredOffset>0.f;
    const bool bAreaSearch=Errand.Action==EDeliveryNpcAction::SearchNearOwner;
    const auto IsInSearchFloor=[&](const FVector& Feet)
    {
        return !bAreaSearch || (FMath::Abs(Feet.Z-ItemLastSeenLocation.Z)<=110. &&
            FVector::DistSquared2D(Feet,ItemLastSeenLocation)<=FMath::Square(DeliveryCompanionItemSearch::Radius));
    };
    if (Delivery && !RejectedDeliveryStands.IsEmpty()) RefreshDeliveryStandRejections(*Delivery);
    const bool bRetryingDelivery=Delivery && !RejectedDeliveryStands.IsEmpty();
    const auto HasReachedApproach=[&](const FVector& Destination)
    {
        const FVector Feet=Npc->GetNavAgentLocation();
        if (DoorWaitHome && (!Npc->GetCharacterMovement()->IsMovingOnGround() || !IsSafeDeliveryDoorWait(*DoorWaitHome,Feet))) return false;
        if (GroundPackage && !IsNearGroundPackageFloor(*GroundPackage,Feet)) return false;
        return IsInSearchFloor(Feet) && FVector::Dist2D(Feet,Destination)<(bPrecise?12.f:45.f) &&
            FMath::Abs(Feet.Z-Destination.Z)<60 && FVector::Dist(Npc->GetActorLocation(),Goal)<=ReachRadius &&
            (bAreaSearch || HasClearErrandApproach(Npc,Npc->GetActorLocation(),Goal,InteractionTarget));
    };
    const double Now=GetWorld()->GetTimeSeconds();
    const bool bGoalUnchanged=bHasErrandDestination && ErrandDeliveryTarget.HasSameIndexAndSerialNumber(ExpectedDelivery) &&
        FVector::Dist(Goal,ErrandGoal)<(bPrecise?8.f:GetTuning().RepathDistance);
    if (bGoalUnchanged && (!DoorWaitHome || IsSafeDeliveryDoorWait(*DoorWaitHome,ErrandDestination,ErrandMoveAcceptanceRadius)))
    {
        const auto* Following=AI->GetPathFollowingComponent();
        if (bErrandOwnsNavigation && Following && Following->GetCurrentRequestId()==ErrandMoveId &&
            AI->GetMoveStatus()!=EPathFollowingStatus::Idle) return false;
        if (HasReachedApproach(ErrandDestination)) return true;
    }
    if (Now<NextErrandNavigationAt) return false;
    NextErrandNavigationAt=Now+GetTuning().NavigationInterval;
    FVector Approach=(Npc->GetActorLocation()-Goal).GetSafeNormal2D();
    if (Approach.IsNearlyZero()) Approach=-Npc->GetActorForwardVector();
    // 面馆的取件锚点在建筑内部，在实际交互半径内寻找可到达的门前位置
    const float FarOffset=FMath::Max(45.f,ReachRadius*.75f);
    const float MiddleOffset=FMath::Max(45.f,ReachRadius*.45f);
    const float NearOffset=FMath::Clamp(ReachRadius*.2f,30.f,125.f);
    const float Offsets[]={bPrecise?PreferredOffset:bRetryingDelivery?MiddleOffset:FarOffset,
        bRetryingDelivery?NearOffset:MiddleOffset,bRetryingDelivery?FarOffset:NearOffset};
    for (int32 Index=0;Index<(bPrecise?1:UE_ARRAY_COUNT(Offsets));++Index)
    for (const float Angle:{0.f,45.f,-45.f,90.f,-90.f,135.f,-135.f,180.f})
    {
        FNavLocation Projected;
        const FVector Desired=Goal+Approach.RotateAngleAxis(Angle,FVector::UpVector)*Offsets[Index];
        if (!Nav->ProjectPointToNavigation(Desired,Projected,FVector(80,80,250),&Npc->GetNavAgentPropertiesRef()) ||
            (bPrecise && FVector::Dist2D(Projected.Location,Desired)>8.f) ||
            FVector::Dist2D(Projected.Location,Goal)>ReachRadius ||
            (GroundPackage && !IsNearGroundPackageFloor(*GroundPackage,Projected.Location)) || !IsInSearchFloor(Projected.Location) ||
            !(DoorWaitHome?IsSafeDeliveryDoorWait(*DoorWaitHome,Projected.Location,ErrandMoveAcceptanceRadius):IsDestinationClear(Projected.Location)) ||
            (!bAreaSearch && !HasClearErrandApproach(Npc,Projected.Location+FVector(0,0,Npc->GetSimpleCollisionHalfHeight()),Goal,InteractionTarget))) continue;
        if (bRetryingDelivery && RejectedDeliveryStands.ContainsByPredicate([&Projected](const FVector& Stand)
            { return FVector::DistSquared2D(Stand,Projected.Location)<FMath::Square(RejectedDeliveryStandRadius); })) continue;
        const auto* Path=Nav->FindPathToLocationSynchronously(GetWorld(),Npc->GetNavAgentLocation(),Projected.Location,Npc);
        if (!Path || !Path->IsValid() || Path->IsPartial()) continue;
        FAIMoveRequest Request(Projected.Location);
        Request.SetAcceptanceRadius(bPrecise?7.f:ErrandMoveAcceptanceRadius);
        Request.SetReachTestIncludesAgentRadius(false);
        Request.SetReachTestIncludesGoalRadius(false);
        Request.SetAllowPartialPath(false);
        const auto Result=AI->MoveTo(Request);
        // 换路径可同步结束旧请求，回调已经换委托时不能写回旧路或尝试交接
        if (!IsCurrent())
        {
            if (WeakThis.IsValid() && WeakAI.IsValid() && Result.Code==EPathFollowingRequestResult::RequestSuccessful && Result.MoveId.IsValid())
                if (auto* CurrentPath=AI->GetPathFollowingComponent(); CurrentPath && CurrentPath->GetCurrentRequestId()==Result.MoveId)
                    CurrentPath->AbortMove(*this,FPathFollowingResultFlags::UserAbort,Result.MoveId);
            return false;
        }
        if (Result.Code==EPathFollowingRequestResult::Failed) continue;
        if (Result.Code==EPathFollowingRequestResult::AlreadyAtGoal && !HasReachedApproach(Projected.Location)) continue;
        if (!bErrandTravelBudgetAdded)
        {
            // 长路线补足正常行走时间，每阶段只补一次，反复重算或堵路不能无限延长委托
            const double PathLength=Path->GetPathLength();
            const float Speed=Npc->GetCharacterMovement()->GetMaxSpeed();
            if (FMath::IsFinite(PathLength) && PathLength>=0 && FMath::IsFinite(Speed) && Speed>0)
            {
                ErrandTimeLimitSeconds+=PathLength/Speed;
                bErrandTravelBudgetAdded=true;
            }
        }
        ErrandMoveId=Result.MoveId;
        bErrandOwnsNavigation=Result.Code==EPathFollowingRequestResult::RequestSuccessful;
        ErrandDestination=Projected.Location;
        ErrandGoal=Goal;
        ErrandDeliveryTarget=ExpectedDelivery;
        bHasErrandDestination=true;
        auto* Following=AI->GetPathFollowingComponent();
        if (Delivery && Phase==EDeliveryCompanionErrandPhase::GoToDelivery && InteractionTarget && bErrandOwnsNavigation &&
            Following && Following->GetCurrentRequestId()==ErrandMoveId)
            WatchDeliveryApproach(*Following,ErrandMoveId,*InteractionTarget);
        else ClearDeliveryApproachWatch();
        SetSuspension(NAME_None);
        LastErrandPause=NAME_None;
        return Result.Code==EPathFollowingRequestResult::AlreadyAtGoal;
    }
    // 精确拾取站位由调用者限时处理，候选暂不可用时保留原对齐计时
    if (bPrecise) return false;
    // 旁人离开后旧站位可能重新可用，所有候选受限时按导航间隔放回最早的位置
    if (bRetryingDelivery)
    {
        RejectedDeliveryStands.RemoveAt(0);
        return false;
    }
    // 停止也会触发同步回调，暂停只能由仍然对应的委托写入
    StopErrandMovement();
    if (!IsCurrent()) return false;
    bHasErrandDestination=false;
    NextErrandNavigationAt=Now+GetTuning().BlockedRetrySeconds;
    ReportErrandPause(TEXT("ErrandPathBlocked"));
    return false;
}

void UDeliveryCompanionComponent::UpdateErrand()
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority() || !HasActiveErrand()) return;
    if (DeliveryNpc::IsCompanionItemErrand(Errand.Action)) { UpdateItemErrand(); return; }
    if (HasActiveHomeArrangement() || PendingHomeArrangementId.IsValid())
    { StopErrandMovement(); UpdateHomeArrangement(); return; }
    if (!CanCommand(OwnerPlayerState)) { FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("OwnerLost")); return; }
    if (Npc->IsKnockedDown()) { StopErrandMovement(); SetSuspension(TEXT("Down")); return; }
    const auto* Manager=UDeliveryTaskManagerComponent::Get(this);
    if (!IsValid(Errand.Task) || !Manager || !Manager->CanAcquireItem(Errand.Task))
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("TaskNoLongerAvailable")); return; }
    // 此编号在同步取件/交接/投递回调后也复查，旧调用不得结束回调中新接下的委托
    const int32 Revision=Errand.CommandRevision;
    const auto IsCurrent=[&]()
    {
        return IsValid(this) && IsValid(Npc) && !Npc->IsActorBeingDestroyed() &&
            HasActiveErrand() && Errand.CommandRevision==Revision && CommandRevision==Revision;
    };
    if (Npc->Combat->IsReactingToHit() && HasErrandPackageInTransit())
    {
        StopErrandMovement(true);
        if (IsCurrent())
        {
            // 短后仰暂停本次行程计时，受阻重试与路线在恢复时重新检查
            bHasErrandDestination=false;
            SetSuspension(ErrandHitReaction);
        }
        return;
    }
    if (SuspensionReason==ErrandHitReaction)
    {
        LastErrandUpdateAt=0.;
        NextErrandNavigationAt=0.;
        NextVehicleNavigationAt=0.;
        SetSuspension(NAME_None);
    }
    if (Npc->Agent->ShouldPauseForConversation() ||
        (Npc->Agent->HasActiveAction() && !Npc->Agent->CanContinueCompanionMovement()))
    {
        StopErrandMovement();
        if (IsCurrent()) SetSuspension(TEXT("OtherBehavior"));
        return;
    }
    if (const auto* NpcAI=Cast<ADeliveryNpcAIController>(Npc->GetController()); NpcAI && NpcAI->IsTraversingObstacle(ErrandMoveId))
    {
        LastErrandUpdateAt=0;
        return;
    }
    if (!CapturePendingWaitLocation() || !IsCurrent())
    {
        if (IsCurrent()) LastErrandUpdateAt=0;
        return;
    }
    const bool bGroundPackage=Errand.Phase==EDeliveryCompanionErrandPhase::GoToPickup && Errand.bRecoverGroundPackage;
    if (bGroundPackage && Errand.Reason==TEXT("PackagePickedUpFromGround"))
    {
        if (!IsValid(Errand.Package) || GetCarriedTaskPackage()!=Errand.Package || PackageTask(Errand.Package)!=Errand.Task)
        { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("PackageHandoffStateChanged")); return; }
        const auto* Clip=GetTuning().GroundPickupAnimation.Get();
        if (Clip && Errand.GroundPickupServerTime>=0 && GetHomeServerTime()-Errand.GroundPickupServerTime<Clip->GetPlayLength()) return;
        if (Errand.Action==EDeliveryNpcAction::TakePackage)
            FinishErrand(EDeliveryNpcActionStatus::Succeeded,TEXT("PackagePickedUpFromGround"));
        else SetErrandPhase(Errand.Action==EDeliveryNpcAction::FetchPackage?EDeliveryCompanionErrandPhase::ReturnToOwner:
            EDeliveryCompanionErrandPhase::GoToDelivery);
        return;
    }
    // 开箱期间持有原请求编号，不能把自己的 TrunkBusy 当成主人失效后反复取消
    if (ErrandCargoHandoffId.IsValid())
    {
        auto* Bike=ErrandCargoVehicle.Get();
        bool bComplete=bErrandCargoHandoffCommitted,bSucceeded=bErrandCargoHandoffCommitted;
        FName Reason;
        if (!bComplete && (!IsValid(Bike) || !Bike->GetCargoHandoffResult(ErrandCargoHandoffId,bComplete,bSucceeded,Reason)))
        { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("CargoHandoffLost")); return; }
        const double Now=GetWorld()->GetTimeSeconds();
        if (LastErrandUpdateAt>0.) ErrandActiveSeconds+=FMath::Clamp(float(Now-LastErrandUpdateAt),0.f,1.f);
        LastErrandUpdateAt=Now;
        if (!bComplete)
        {
            if (ErrandActiveSeconds>=ErrandTimeLimitSeconds)
                FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("ErrandTimedOut"));
            return;
        }
        ErrandCargoHandoffId.Invalidate();
        ErrandCargoVehicle.Reset();
        bErrandCargoHandoffCommitted=false;
        // 动画被环境打断且原货还在主人这一侧，保留委托并重新校验停稳和距离
        if (!bSucceeded && Reason==TEXT("CargoHandoffCancelled") && Errand.Phase==EDeliveryCompanionErrandPhase::ReceiveFromOwner &&
            IsValid(Errand.Package) && PackageTask(Errand.Package)==Errand.Task && GetCommandPackage()==Errand.Package &&
            !GetCarriedTaskPackage())
        {
            if (ErrandActiveSeconds>=ErrandTimeLimitSeconds)
            { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("ErrandTimedOut")); return; }
            Errand.Reason=NAME_None;
            ReportErrandPause(TEXT("CargoHandoffInterrupted"));
            if (IsCurrent()) NextCargoHandoffAt=GetWorld()->GetTimeSeconds()+.5;
            return;
        }
        if (!bSucceeded || GetCarriedTaskPackage()!=Errand.Package || PackageTask(GetCarriedTaskPackage())!=Errand.Task)
        { FinishErrand(EDeliveryNpcActionStatus::Failed,bSucceeded?FName(TEXT("PackageHandoffStateChanged")):Reason); return; }
        if (Errand.Action==EDeliveryNpcAction::DeliverPackage)
        {
            SetErrandPhase(EDeliveryCompanionErrandPhase::GoToDelivery);
            if (!IsCurrent()) return;
            Npc->ObserveFact(TEXT("CompanionPackageReceived"),Bike,
                TEXT("游戏确认我已从主人停放的车上接到本次委托的同一个实物包裹，尚未送达。"),EDeliveryNpcEvidenceSource::ActionResult);
        }
        else FinishErrand(EDeliveryNpcActionStatus::Succeeded,TEXT("PackageReceived"));
        return;
    }
    // 交还始终面向主人当前步行身体，不能因为车上有别的货就改为装车
    APawn* OwnerCarrierPawn=Errand.Phase==EDeliveryCompanionErrandPhase::ReturnToOwner?
        Cast<ADeliveryCharacter>(GetOwnerPawn()):GetOwnerPackageCarrier();
    auto* OwnerCarrier=Cast<IDeliveryPackageCarrierInterface>(OwnerCarrierPawn);
    const bool bNeedsOwner=Errand.Phase==EDeliveryCompanionErrandPhase::ReceiveFromOwner || Errand.Phase==EDeliveryCompanionErrandPhase::ReturnToOwner;
    const bool bWaitingForOwner=bNeedsOwner && (!OwnerCarrier || !OwnerCarrier->CanHandleTaskPackage());
    auto* OwnerBike=bWaitingForOwner?Cast<ADeliveryMotorbike>(GetOwnerPawn()):nullptr;
    const auto* Driver=IsValid(OwnerBike) && !OwnerBike->IsActorBeingDestroyed()?OwnerBike->GetDriver():nullptr;
    if (Mode==EDeliveryCompanionMode::Follow && Errand.Phase==EDeliveryCompanionErrandPhase::ReceiveFromOwner &&
        !Npc->Combat->IsReactingToHit() && IsValid(Driver) && !Driver->IsActorBeingDestroyed() && Driver->GetTaskPlayerState()==OwnerPlayerState &&
        Driver->GetCurrentVehicle()==OwnerBike && OwnerCarrier && IsValid(Errand.Package) &&
        OwnerCarrier->GetCarriedTaskPackage()==Errand.Package && PackageTask(Errand.Package)==Errand.Task)
    {
        if (OwnerHandoffFollowRevision!=Revision)
        {
            ReportErrandPause(TEXT("OwnerRiding"));
            if (!IsCurrent() || GetOwnerPawn()!=OwnerBike) return;
            OwnerHandoffFollowRevision=Revision;
        }
        // 委托节点保留接件，只复用跟随的移动，停车前不交接原件
        UpdateFollowMovement();
        return;
    }
    if (OwnerHandoffFollowRevision!=INDEX_NONE)
    {
        StopOwnerHandoffFollow();
        // 停旧路线会同步通知，需要下一帧重读主人身体和交接物
        return;
    }
    if (bWaitingForOwner)
    { ReportErrandPause(OwnerBike?TEXT("OwnerRiding"):TEXT("OwnerUnavailable")); return; }
    if (bGroundPackage)
    {
        if (!IsValid(Errand.Package) || GetOwnerDroppedPackage()!=Errand.Package || PackageTask(Errand.Package)!=Errand.Task)
        { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("GroundPackageUnavailable")); return; }
    }
    else if (Errand.Phase!=EDeliveryCompanionErrandPhase::GoToPickup)
    {
        const auto* Actual=Errand.Phase==EDeliveryCompanionErrandPhase::ReceiveFromOwner?OwnerCarrier->GetCarriedTaskPackage():GetCarriedTaskPackage();
        if (!IsValid(Errand.Package) || Actual!=Errand.Package || PackageTask(Actual)!=Errand.Task)
        { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("ExpectedPackageChanged")); return; }
    }
    const bool bRequestedBackpack=Errand.Action==EDeliveryNpcAction::StorePackageInBackpack;
    bool bToBackpack=bRequestedBackpack;
    if (Errand.Phase==EDeliveryCompanionErrandPhase::ReturnToOwner && Errand.Action!=EDeliveryNpcAction::DropPackage)
    {
        const bool bCanReceiveInHand=!bRequestedBackpack && OwnerCarrier->CanReceiveTaskPackage(Errand.Package);
        // 交还仍优先入手。主人手忙时使用真实空背包槽，保留已经手持的物件
        bToBackpack=!bCanReceiveInHand && OwnerCarrier->CanReceiveTaskPackageInBackpack(Errand.Package);
        if (!bCanReceiveInHand && !bToBackpack)
        { ReportErrandPause(bRequestedBackpack?TEXT("OwnerBackpackUnavailable"):TEXT("OwnerHandsBusy")); return; }
    }
    const double Now=GetWorld()->GetTimeSeconds();
    if (Errand.Phase==EDeliveryCompanionErrandPhase::ReceiveFromOwner && Now<NextCargoHandoffAt)
    { LastErrandUpdateAt=Now; return; }
    const float Dt=LastErrandUpdateAt>0?FMath::Clamp(static_cast<float>(Now-LastErrandUpdateAt),0.f,1.f):0.f;
    LastErrandUpdateAt=Now;
    ErrandActiveSeconds+=Dt;
    if (ErrandActiveSeconds>=ErrandTimeLimitSeconds)
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("ErrandTimedOut")); return; }

    FVector Goal;
    float ReachRadius=220;
    UDeliveryPickupPointComponent* Pickup=nullptr;
    UDeliveryTargetComponent* Delivery=nullptr;
    const ADeliveryHome* DeliveryHome=nullptr;
    TOptional<FVector> DeliveryCurb;
    bool bWalkIndoorDelivery=false;
    UDeliveryInteractableComponent* Interaction=nullptr;
    AActor* InteractionTarget=OwnerCarrierPawn;
    if (bNeedsOwner) Goal=OwnerCarrier->GetTaskPackageTransferPoint();
    else if (bGroundPackage)
    {
        InteractionTarget=Errand.Package;
        Goal=Errand.Package->GetActorLocation();
        ReachRadius=GroundPackageReach;
    }
    else
    {
        const bool bPickup=Errand.Phase==EDeliveryCompanionErrandPhase::GoToPickup;
        auto* Target=ResolveErrandLocation(bPickup);
        if (!Target) { FinishErrand(EDeliveryNpcActionStatus::Failed,bPickup?TEXT("PickupNoLongerAvailable"):TEXT("DeliveryTargetLost")); return; }
        InteractionTarget=Target;
        if (bPickup)
        {
            Pickup=UDeliveryPickupPointComponent::FindOn(Target);
            Interaction=UDeliveryInteractableComponent::FindOn(Target,EDeliveryInteractionKey::PickupE);
            if (!Interaction) { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("PickupInteractionMissing")); return; }
            Goal=Interaction->GetInteractionPoint(Npc->GetActorLocation());
            ReachRadius=Pickup->InteractRadius;
        }
        else
        {
            Delivery=Target->FindComponentByClass<UDeliveryTargetComponent>();
            if (!Delivery) { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("DeliveryTargetLost")); return; }
            const auto* Grandpa=Cast<ADeliveryGrandpa>(Target);
            const auto* Home=Grandpa?ADeliveryHome::FindSharedHome(this):nullptr;
            if (IsValid(Home) && Home->FormerResident==Grandpa && IsValid(Home->ReturnAnchor)) DeliveryHome=Home;
            const auto Street=DeliveryHome?ResolveDeliveryDoorStreet(*DeliveryHome):TOptional<FDeliveryDoorStreet>();
            if (Street.IsSet()) DeliveryCurb=Street->GetWaitGoal(GetTuning().DestinationClearance);
            if (DeliveryHome && IsValid(Grandpa->FrontDoor) && !Grandpa->IsDoorBroken())
            {
                if (!DeliveryCurb.IsSet()) { ReportErrandPause(TEXT("ErrandPathBlocked")); return; }
                const bool bReady=UpdateVehicleTravel(*DeliveryCurb,DoorWaitReach,EDeliveryCompanionTravelPurpose::Errand,&DeliveryCurb.GetValue());
                if (!IsCurrent() || !bReady || !IsValid(Home) || !IsValid(Grandpa) || Grandpa->IsDoorBroken()) return;
                const FVector Feet=Npc->GetNavAgentLocation();
                const bool bAtStreetWait=Npc->GetCharacterMovement()->IsMovingOnGround() &&
                    FVector::DistSquared2D(Feet,*DeliveryCurb)<=FMath::Square(DoorWaitReach) && IsSafeDeliveryDoorWait(*Home,Feet);
                if (!bAtStreetWait)
                {
                    MoveForErrand(*DeliveryCurb,DoorWaitReach,nullptr,0.f,nullptr,Home);
                    return;
                }
                // 停下本次走向门外的路，等二狗站稳后再报告正在等门
                if (bErrandOwnsNavigation) StopErrandMovement();
                if (!IsCurrent() || !IsValid(Grandpa) || Grandpa->IsDoorBroken() || Npc->GetVelocity().SizeSquared2D()>FMath::Square(12.f)) return;
                if (LastErrandPause!=TEXT("WaitingAtGrandpaDoor")) ReportErrandPause(TEXT("WaitingAtGrandpaDoor"));
                if (IsCurrent()) { SetSuspension(TEXT("WaitingAtGrandpaDoor")); LastErrandUpdateAt=0; }
                return;
            }
            if (DeliveryHome && (LastErrandPause==TEXT("WaitingAtGrandpaDoor") ||
                (bHasErrandDestination && !ErrandDeliveryTarget.IsValid())))
            {
                // 门可能在二狗走向街侧时打开，停旧路并立即重新计算收件路线
                StopErrandMovement();
                if (!IsCurrent() || !IsValid(Delivery) || !IsValid(DeliveryHome)) return;
                LastErrandPause=NAME_None;
                bHasErrandDestination=false;
                NextErrandNavigationAt=0;
            }
            Goal=Delivery->GetDeliveryPoint(Npc->GetActorLocation());
            ReachRadius=Delivery->DeliveryRadius;
            if (Street.IsSet() && IsValid(Grandpa->FrontDoor) && Grandpa->IsDoorBroken() &&
                IsValid(DeliveryHome->DoorwayLink) && DeliveryHome->DoorwayLink->IsEnabled() && !IsUsingErrandVehicle())
            {
                const FVector Feet=Npc->GetNavAgentLocation();
                bWalkIndoorDelivery=FMath::IsWithinInclusive(Feet.Z,Street->LowFloor,Street->HighFloor) &&
                    FVector::DotProduct(Feet-Street->DoorCenter,Street->Outward)<=0. &&
                    FVector::DotProduct(Goal-Street->DoorCenter,Street->Outward)<0. &&
                    FVector::DistSquared2D(Feet,Goal)<=FMath::Square(ReachRadius+GetTuning().DestinationClearance);
            }
        }
    }
    // 门已经打开时车仍停屋外，收件点是爷爷本人所在的交互位置
    const TWeakObjectPtr<UDeliveryTargetComponent> ExpectedReceiver(Delivery);
    const FVector VehicleGoal=DeliveryCurb.IsSet()?*DeliveryCurb:DeliveryHome?DeliveryHome->ReturnAnchor->GetComponentLocation():Goal;
    const bool bPlayingGroundPickup=bGroundPackage && Errand.GroundPickupServerTime>=0;
    // 已步行进屋时去实际收件点，外面的停车距离不应再次触发取车
    if (bWalkIndoorDelivery) StopVehicleApproach();
    if (!IsCurrent()) return;
    const bool bOnFoot=bWalkIndoorDelivery || (bPlayingGroundPickup?!IsUsingErrandVehicle():
        UpdateVehicleTravel(VehicleGoal,DeliveryHome?DoorWaitReach:ReachRadius,EDeliveryCompanionTravelPurpose::Errand,
            DeliveryCurb.IsSet()?&DeliveryCurb.GetValue():nullptr));
    if (!IsCurrent()) return;
    if (!bOnFoot) { LastErrandUpdateAt=Now; return; }
    if (Delivery && (!ExpectedReceiver.IsValid() || !IsValid(InteractionTarget) || InteractionTarget->IsActorBeingDestroyed())) return;
    if (bGroundPackage && GetOwnerDroppedPackage()!=Errand.Package)
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("GroundPackageUnavailable")); return; }
    const bool bInReach=(bGroundPackage?FVector::DistSquared(Npc->GetActorLocation(),Goal)<=FMath::Square(GroundPackageReach) &&
        IsNearGroundPackageFloor(*Errand.Package,Npc->GetNavAgentLocation()):
        bNeedsOwner?FVector::Dist(Npc->GetActorLocation(),Goal)<=220:
        Pickup?Interaction->CanInteract(Npc):Delivery->CanAcceptDelivery(Errand.Package,OwnerPlayerState)) &&
        HasClearErrandApproach(Npc,Npc->GetActorLocation(),Goal,InteractionTarget);
    if (!bInReach)
    {
        if (Errand.GroundPlacementServerTime>=0)
        {
            Errand.GroundPlacementServerTime=-1;
            Npc->ForceNetUpdate();
        }
        if (bPlayingGroundPickup) StopErrandMovement();
        if (!IsCurrent()) return;
        const bool bArrived=MoveForErrand(Goal,ReachRadius,InteractionTarget,0.f,Delivery);
        if (!IsCurrent()) return;
        if (bArrived && Delivery && bHasErrandDestination)
        {
            // 人已沿路径到位但包裹的距离或射线还不符合收件规则，按实际站位换点
            RejectDeliveryStand(*Delivery,Npc->GetNavAgentLocation());
            NextErrandNavigationAt=Now+GetTuning().NavigationInterval;
        }
    }
    if (!IsCurrent()) return;
    // 路线重试也算本次执行时间；BT中断和等主人恢复时则重置采样起点
    LastErrandUpdateAt=Now;
    if (!bInReach) { ErrandInteractionSeconds=0; return; }
    if (!bPlayingGroundPickup && (Errand.Action!=EDeliveryNpcAction::DropPackage || Errand.GroundPlacementServerTime<0)) StopErrandMovement();
    if (!IsCurrent()) return;
    LastErrandUpdateAt=Now;
    SetSuspension(NAME_None);
    FName Failure;
    bool bSucceeded=false;
    if (bGroundPackage)
    {
        bSucceeded=TryRecoverGroundPackage(Failure);
        if (!IsCurrent()) return;
        if (bSucceeded)
        {
            Errand.Reason=TEXT("PackagePickedUpFromGround");
            Npc->ObserveFact(TEXT("CompanionPackagePickedUp"),Errand.Package,
                TEXT("游戏确认我拿起主人放在地上的本单原包裹，实物已经在我手中，尚未交还或送达。"),EDeliveryNpcEvidenceSource::ActionResult);
            return;
        }
        if (Failure==TEXT("PickingUpGroundPackage")) return;
        if (bPlayingGroundPickup) StopErrandMovement();
        if (!IsCurrent()) return;
    }
    else if (Pickup)
    {
        bSucceeded=Pickup->TryDispenseToCarrier(Npc,Errand.Task);
        if (!IsCurrent()) return;
        if (bSucceeded && GetCarriedTaskPackage() && PackageTask(GetCarriedTaskPackage())==Errand.Task)
        {
            Errand.Package=GetCarriedTaskPackage();
            SetErrandPhase(Errand.Action==EDeliveryNpcAction::FetchPackage?EDeliveryCompanionErrandPhase::ReturnToOwner:EDeliveryCompanionErrandPhase::GoToDelivery);
            Npc->ObserveFact(TEXT("CompanionPackagePickedUp"),Npc,TEXT("游戏确认我已从取件点拿到本次委托的实物包裹，后续交还或投递尚未完成。"),EDeliveryNpcEvidenceSource::ActionResult);
            return;
        }
        Failure=TEXT("PickupRejected");
    }
    else if (Delivery)
    {
        // 停止走路可能同步通知其他系统，签收仍须由本单当前登记的收件组件处理
        if (!ExpectedReceiver.IsValid() || ResolveErrandLocation(false)!=InteractionTarget ||
            InteractionTarget->FindComponentByClass<UDeliveryTargetComponent>()!=ExpectedReceiver.Get()) return;
        BindTaskCompletion();
        bSucceeded=Delivery->TryDeliver(Errand.Package,OwnerPlayerState);
        if (!IsCurrent()) return;
        if (bSucceeded) { FinishErrand(EDeliveryNpcActionStatus::Succeeded,TEXT("Delivered")); return; }
        Failure=TEXT("DeliveryRejected");
    }
    else if (Errand.Action==EDeliveryNpcAction::DropPackage)
    {
        bSucceeded=TryPlacePackageOnGround(Failure);
        if (!IsCurrent()) return;
        if (bSucceeded)
        {
            FinishErrand(EDeliveryNpcActionStatus::Succeeded,TEXT("PackagePlacedOnGround"));
            return;
        }
        // 支撑或姿势在接触前失效时，下次从新的放置动作开始
        if (Failure!=TEXT("PlacingPackage") && Errand.GroundPlacementServerTime>=0)
        {
            Errand.GroundPlacementServerTime=-1;
            Npc->ForceNetUpdate();
        }
    }
    else
    {
        const bool bReceiving=Errand.Phase==EDeliveryCompanionErrandPhase::ReceiveFromOwner;
        const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
        const TWeakObjectPtr<UDeliveryTaskDefinition> ExpectedTask(Errand.Task.Get());
        const TWeakObjectPtr<ADeliveryPackageItem> ExpectedPackage(Errand.Package.Get());
        const auto Phase=Errand.Phase;
        const auto Continue=[WeakThis,ExpectedTask,ExpectedPackage,Revision,Phase]()
        {
            const auto* C=WeakThis.Get();
            const auto* Character=C?C->GetCompanion():nullptr;
            const auto* Tasks=C?UDeliveryTaskManagerComponent::Get(C):nullptr;
            return C && IsValid(Character) && !Character->IsActorBeingDestroyed() &&
                C->CommandRevision==Revision && C->HasActiveErrand() && C->Errand.CommandRevision==Revision &&
                C->Errand.Phase==Phase && ExpectedTask.IsValid() && C->Errand.Task==ExpectedTask.Get() &&
                ExpectedPackage.IsValid() && C->Errand.Package==ExpectedPackage.Get() &&
                C->CanCommand(C->OwnerPlayerState) && IsValid(C->GetOwnerPawn()) && Character->CanHandleTaskPackage() &&
                !Character->Agent->ShouldPauseForConversation() &&
                (!Character->Agent->HasActiveAction() || Character->Agent->CanContinueCompanionMovement()) &&
                Tasks && Tasks->CanAcquireItem(ExpectedTask.Get());
        };
        if (auto* Bike=bReceiving?Cast<ADeliveryMotorbike>(OwnerCarrierPawn):nullptr)
        {
            const FGuid Id=Bike->RequestCargoHandoff(Npc,Errand.Package,OwnerPlayerState,Continue);
            if (!IsCurrent()) { if (IsValid(Bike)) Bike->CancelCargoHandoff(Id); return; }
            if (Id.IsValid())
            {
                ErrandCargoVehicle=Bike;
                ErrandCargoHandoffId=Id;
                Errand.Reason=TEXT("OpeningOwnerCargo");
                Npc->ForceNetUpdate();
                return;
            }
            Failure=TEXT("CargoHandoffUnavailable");
        }
        else bSucceeded=DeliveryPackageTransfer::TryTransfer(bReceiving?OwnerCarrierPawn:Npc,
            bReceiving?Npc:OwnerCarrierPawn,Failure,Errand.Package,Continue,bToBackpack);
        if (!IsCurrent()) return;
        if (bSucceeded)
        {
            if (bReceiving && Errand.Action==EDeliveryNpcAction::DeliverPackage)
            {
                SetErrandPhase(EDeliveryCompanionErrandPhase::GoToDelivery);
                Npc->ObserveFact(TEXT("CompanionPackageReceived"),OwnerCarrierPawn,TEXT("游戏确认主人携带的同一个包裹已经交到我手里，尚未送达。"),EDeliveryNpcEvidenceSource::ActionResult);
            }
            else FinishErrand(EDeliveryNpcActionStatus::Succeeded,bReceiving?TEXT("PackageReceived"):
                bToBackpack?TEXT("PackageStoredInOwnerBackpack"):TEXT("PackageReturned"));
            return;
        }
    }
    // 距离内也可能仍隔着门或站着别人，给短暂移动让路留出时间，不把走近当成交接成功
    ErrandInteractionSeconds+=Dt;
    Errand.Reason=Failure;
    if (ErrandInteractionSeconds>=GetTuning().ErrandInteractionTimeoutSeconds)
        FinishErrand(EDeliveryNpcActionStatus::Failed,Failure);
}
