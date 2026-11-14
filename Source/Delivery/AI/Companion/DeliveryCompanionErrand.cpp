#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Carry/DeliveryPackageCarrierInterface.h"
#include "Character/DeliveryCharacter.h"
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
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "Navigation/PathFollowingComponent.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"

namespace
{
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
            if (!IsValid(Actor) || Actor->GetWorld()!=Context->GetWorld()) continue;
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

APawn* UDeliveryCompanionComponent::GetOwnerPackageCarrier() const
{
    auto* Pawn=GetOwnerPawn();
    auto* RidingBike=Cast<ADeliveryMotorbike>(Pawn);
    auto* Character=RidingBike?RidingBike->GetDriver():Cast<ADeliveryCharacter>(Pawn);
    if (!IsValid(Character) || Character->GetWorld()!=GetWorld() || Character->GetTaskPlayerState()!=OwnerPlayerState) return nullptr;
    // 手持实物优先；骑车时 Carry 返回车上的货物，须继续用真实车辆作为转出方
    if (auto* Package=Character->GetCarriedTaskPackage(); IsValid(Package) && Package->GetOwner()==Character) return Character;
    const auto* Summon=Character->FindComponentByClass<UDeliveryVehicleSummonComponent>();
    auto* Bike=Summon?Summon->GetRegisteredVehicle():nullptr;
    if (Bike && Bike->GetCargoPlayerState()==OwnerPlayerState && Bike->GetCargo()) return Bike;
    if (RidingBike && Character->GetCarriedTaskPackage()) return nullptr;
    return Character;
}

ADeliveryPackageItem* UDeliveryCompanionComponent::GetCommandPackage() const
{
    if (auto* Package=GetCarriedTaskPackage()) return Package;
    const auto* Carrier=Cast<IDeliveryPackageCarrierInterface>(GetOwnerPackageCarrier());
    auto* Package=Carrier?Carrier->GetCarriedTaskPackage():nullptr;
    return IsValid(Package)?Package:nullptr;
}

UDeliveryTaskDefinition* UDeliveryCompanionComponent::GetCommandTask() const
{
    if (auto* Task=PackageTask(GetCommandPackage())) return Task;
    const auto* Tracker=UDeliveryTaskTrackerComponent::FindTracker(OwnerPlayerState);
    return Tracker?Tracker->GetTrackedTask():nullptr;
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
    if (Action==EDeliveryNpcAction::ReturnPackage) return Held && PackageTask(Held)==Task;
    if (Action==EDeliveryNpcAction::TakePackage) return !Held && bOwnerHasTask;
    const bool bCanPickup=!Held && !bOwnerHasTask && Manager->GetTaskStatus(Task)==EDeliveryTaskStatus::AwaitingPickup &&
        FindErrandLocation(this,Task,true);
    if (Action==EDeliveryNpcAction::FetchPackage) return bCanPickup;
    return Action==EDeliveryNpcAction::DeliverPackage && FindErrandLocation(this,Task,false) &&
        ((Held && PackageTask(Held)==Task) || bOwnerHasTask || bCanPickup);
}

bool UDeliveryCompanionComponent::StartErrand(EDeliveryNpcAction Action,APlayerState* Requester,int32 ExpectedRevision,
    UDeliveryTaskDefinition* ExpectedTask,ADeliveryPackageItem* ExpectedPackage,EDeliveryCompanionTravelMode RequestedTravelMode)
{
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
    Errand.Action=Action;
    Errand.Status=EDeliveryNpcActionStatus::Running;
    Errand.Task=ExpectedTask;
    Errand.Package=ExpectedPackage;
    Errand.CommandRevision=CommandRevision;
    if (Action==EDeliveryNpcAction::ReturnPackage) Errand.Phase=EDeliveryCompanionErrandPhase::ReturnToOwner;
    else if (GetCarriedTaskPackage()) Errand.Phase=EDeliveryCompanionErrandPhase::GoToDelivery;
    else if (ExpectedPackage) Errand.Phase=EDeliveryCompanionErrandPhase::ReceiveFromOwner;
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

void UDeliveryCompanionComponent::CancelPackageHandoff()
{
    auto* Bike=ErrandCargoVehicle.Get();
    const FGuid OldId=ErrandCargoHandoffId;
    ErrandCargoVehicle.Reset();
    ErrandCargoHandoffId.Invalidate();
    NextCargoHandoffAt=0.;
    if (IsValid(Bike) && OldId.IsValid()) Bike->CancelCargoHandoff(OldId);
}

void UDeliveryCompanionComponent::StopErrandMovement()
{
    if (!GetOwner() || !GetOwner()->HasAuthority()) return;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const int32 Revision=CommandRevision, ErrandRevision=Errand.CommandRevision;
    const auto StillCurrent=[&]() { return WeakThis.IsValid() && CommandRevision==Revision && Errand.CommandRevision==ErrandRevision; };
    CancelPackageHandoff();
    if (!StillCurrent()) return;
    CancelItemUse();
    // 取消技能和导航都会同步通知外部，旧停止不能清掉回调中新接的安排
    if (!StillCurrent()) return;
    StopVehicleTravel(EDeliveryCompanionTravelPurpose::Errand);
    if (!StillCurrent()) return;
    LastErrandUpdateAt=0;
    if (!bErrandOwnsNavigation) return;
    const FAIRequestID OldId=ErrandMoveId;
    bErrandOwnsNavigation=false;
    ErrandMoveId=FAIRequestID::InvalidRequest;
    const auto* Npc=GetCompanion();
    const auto* AI=Npc?Cast<AAIController>(Npc->GetController()):nullptr;
    if (auto* Path=AI?AI->GetPathFollowingComponent():nullptr; Path && OldId.IsValid() && Path->GetCurrentRequestId()==OldId)
        Path->AbortMove(*this,FPathFollowingResultFlags::UserAbort,OldId);
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
    StopErrandMovement();
    if (!WeakThis.IsValid() || !HasActiveErrand() || CommandRevision!=Revision || Errand.CommandRevision!=ErrandRevision) return;
    Errand.Status=Status;
    Errand.Phase=EDeliveryCompanionErrandPhase::Idle;
    Errand.Reason=Reason;
    Errand.Package=nullptr;
    Errand.Item.Reset();
    ++CommandRevision;
    bHasFollowDestination=false;
    NextNavigationAt=0;
    // 实际订单在此结束，之后按原持续安排返程，保留主人指定的等候点
    SetSuspension(NAME_None);
    GetOwner()->ForceNetUpdate();
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
        FString::Printf(TEXT("游戏确认二狗的 %s 委托已结束：%s，原因 %s，订单 %s。当前二狗%s实际包裹；只有 Delivered 表示已完成投递并由任务系统处理奖励。"),
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

bool UDeliveryCompanionComponent::MoveForErrand(const FVector& Goal,float ReachRadius,AActor* InteractionTarget)
{
    auto* Npc=GetCompanion();
    auto* AI=Npc?Cast<AAIController>(Npc->GetController()):nullptr;
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!AI || !Nav || Goal.ContainsNaN()) return false;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const TWeakObjectPtr<AAIController> WeakAI(AI);
    const TWeakObjectPtr<AActor> Target(InteractionTarget);
    const int32 Revision=CommandRevision;
    const auto Phase=Errand.Phase;
    const auto IsCurrent=[&]()
    {
        return WeakThis.IsValid() && WeakNpc.IsValid() && !Npc->IsActorBeingDestroyed() && WeakAI.IsValid() &&
            Npc->GetController()==AI && (!InteractionTarget || (Target.IsValid() && !Target->IsActorBeingDestroyed())) &&
            HasActiveErrand() && CommandRevision==Revision && Errand.CommandRevision==Revision && Errand.Phase==Phase;
    };
    const auto HasReachedApproach=[&](const FVector& Destination)
    {
        const FVector Feet=Npc->GetNavAgentLocation();
        return FVector::Dist2D(Feet,Destination)<45 && FMath::Abs(Feet.Z-Destination.Z)<60 &&
            FVector::Dist(Npc->GetActorLocation(),Goal)<=ReachRadius &&
            HasClearErrandApproach(Npc,Npc->GetActorLocation(),Goal,InteractionTarget);
    };
    const double Now=GetWorld()->GetTimeSeconds();
    const bool bGoalUnchanged=bHasErrandDestination && FVector::Dist(Goal,ErrandGoal)<GetTuning().RepathDistance;
    if (bGoalUnchanged)
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
    for (const float Offset:{FMath::Max(45.f,ReachRadius*.75f),FMath::Max(45.f,ReachRadius*.45f),FMath::Clamp(ReachRadius*.2f,30.f,125.f)})
    for (const float Angle:{0.f,45.f,-45.f,90.f,-90.f,135.f,-135.f,180.f})
    {
        FNavLocation Projected;
        const FVector Desired=Goal+Approach.RotateAngleAxis(Angle,FVector::UpVector)*Offset;
        if (!Nav->ProjectPointToNavigation(Desired,Projected,FVector(80,80,250),&Npc->GetNavAgentPropertiesRef()) ||
            FVector::Dist2D(Projected.Location,Goal)>ReachRadius || !IsDestinationClear(Projected.Location) ||
            !HasClearErrandApproach(Npc,Projected.Location+FVector(0,0,Npc->GetSimpleCollisionHalfHeight()),Goal,InteractionTarget)) continue;
        const auto* Path=Nav->FindPathToLocationSynchronously(GetWorld(),Npc->GetNavAgentLocation(),Projected.Location,Npc);
        if (!Path || !Path->IsValid() || Path->IsPartial()) continue;
        FAIMoveRequest Request(Projected.Location);
        Request.SetAcceptanceRadius(20);
        Request.SetReachTestIncludesAgentRadius(false);
        Request.SetReachTestIncludesGoalRadius(false);
        Request.SetAllowPartialPath(false);
        const auto Result=AI->MoveTo(Request);
        // 换路径可同步结束旧请求，回调已经换委托时不能写回旧路或尝试交接
        if (!IsCurrent()) return false;
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
        bHasErrandDestination=true;
        SetSuspension(NAME_None);
        LastErrandPause=NAME_None;
        return Result.Code==EPathFollowingRequestResult::AlreadyAtGoal;
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
    if (Npc->IsKnockedDown() || Npc->Agent->ShouldPauseForConversation() ||
        (Npc->Agent->HasActiveAction() && !Npc->Agent->CanContinueCompanionMovement()))
    { StopErrandMovement(); SetSuspension(Npc->IsKnockedDown()?TEXT("Down"):TEXT("OtherBehavior")); return; }
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
    // 开箱期间持有原请求编号，不能把自己的 TrunkBusy 当成主人失效后反复取消
    if (ErrandCargoHandoffId.IsValid())
    {
        auto* Bike=ErrandCargoVehicle.Get();
        bool bComplete=false, bSucceeded=false;
        FName Reason;
        if (!IsValid(Bike) || !Bike->GetCargoHandoffResult(ErrandCargoHandoffId,bComplete,bSucceeded,Reason))
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
    if (bNeedsOwner && (!OwnerCarrier || !OwnerCarrier->CanHandleTaskPackage()))
    { ReportErrandPause(GetOwnerPawn() && GetOwnerPawn()->IsA<ADeliveryMotorbike>()?TEXT("OwnerRiding"):TEXT("OwnerUnavailable")); return; }
    if (Errand.Phase!=EDeliveryCompanionErrandPhase::GoToPickup)
    {
        const auto* Actual=Errand.Phase==EDeliveryCompanionErrandPhase::ReceiveFromOwner?OwnerCarrier->GetCarriedTaskPackage():GetCarriedTaskPackage();
        if (!IsValid(Errand.Package) || Actual!=Errand.Package || PackageTask(Actual)!=Errand.Task)
        { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("ExpectedPackageChanged")); return; }
    }
    if (Errand.Phase==EDeliveryCompanionErrandPhase::ReturnToOwner && !OwnerCarrier->CanReceiveTaskPackage(Errand.Package))
    { ReportErrandPause(TEXT("OwnerHandsBusy")); return; }
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
    UDeliveryInteractableComponent* Interaction=nullptr;
    AActor* InteractionTarget=OwnerCarrierPawn;
    if (bNeedsOwner) Goal=OwnerCarrier->GetTaskPackageTransferPoint();
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
            // 第一单的门未破时先到屋外等候，继续保留同一件包裹
            if (IsValid(Home) && Home->FormerResident==Grandpa && IsValid(Home->ReturnAnchor)) DeliveryHome=Home;
            if (DeliveryHome && IsValid(Grandpa->FrontDoor) && !Grandpa->IsDoorBroken())
            {
                const FVector Outside=Home->ReturnAnchor->GetComponentLocation();
                constexpr float OutsideReach=140.f;
                const FVector Feet=Npc->GetNavAgentLocation();
                const FVector AnchorAtBodyHeight=Outside+(Npc->GetActorLocation()-Feet);
                // 车停稳且二狗已站在同一高度的门外，保留包裹并等待门破
                if (!IsUsingErrandVehicle() && FVector::Dist2D(Feet,Outside)<=OutsideReach &&
                    FMath::Abs(Feet.Z-Outside.Z)<=60.f &&
                    HasClearErrandApproach(Npc,Npc->GetActorLocation(),AnchorAtBodyHeight,nullptr))
                {
                    if (LastErrandPause!=TEXT("WaitingAtGrandpaDoor")) ReportErrandPause(TEXT("WaitingAtGrandpaDoor"));
                    if (IsCurrent())
                    {
                        SetSuspension(TEXT("WaitingAtGrandpaDoor"));
                        LastErrandUpdateAt=0;
                    }
                    return;
                }
                const bool bReady=UpdateVehicleTravel(Outside,OutsideReach,EDeliveryCompanionTravelPurpose::Errand);
                if (!IsCurrent() || !bReady) return;
                if (!MoveForErrand(Outside,OutsideReach,nullptr) || !IsCurrent()) return;
                ReportErrandPause(TEXT("WaitingAtGrandpaDoor"));
                if (IsCurrent()) LastErrandUpdateAt=0;
                return;
            }
            if (LastErrandPause==TEXT("WaitingAtGrandpaDoor"))
            {
                // 门外站位已经完成，打开门后立即计算走向爷爷的新路径
                LastErrandPause=NAME_None;
                bHasErrandDestination=false;
                NextErrandNavigationAt=0;
            }
            Goal=Delivery->GetDeliveryPoint(Npc->GetActorLocation());
            ReachRadius=Delivery->DeliveryRadius;
        }
    }
    // 门已经打开时车仍停屋外，收件点是爷爷本人所在的交互位置
    const FVector VehicleGoal=DeliveryHome?DeliveryHome->ReturnAnchor->GetComponentLocation():Goal;
    const bool bOnFoot=UpdateVehicleTravel(VehicleGoal,DeliveryHome?140.f:ReachRadius,EDeliveryCompanionTravelPurpose::Errand);
    if (!IsCurrent()) return;
    if (!bOnFoot) { LastErrandUpdateAt=Now; return; }
    const bool bInReach=(bNeedsOwner?FVector::Dist(Npc->GetActorLocation(),Goal)<=220:
        Pickup?Interaction->CanInteract(Npc):Delivery->CanAcceptDelivery(Errand.Package,OwnerPlayerState)) &&
        HasClearErrandApproach(Npc,Npc->GetActorLocation(),Goal,InteractionTarget);
    if (!bInReach) MoveForErrand(Goal,ReachRadius,InteractionTarget);
    if (!IsCurrent()) return;
    // 路线重试也算本次执行时间；BT中断和等主人恢复时则重置采样起点
    LastErrandUpdateAt=Now;
    if (!bInReach) { ErrandInteractionSeconds=0; return; }
    StopErrandMovement();
    if (!IsCurrent()) return;
    LastErrandUpdateAt=Now;
    SetSuspension(NAME_None);
    FName Failure;
    bool bSucceeded=false;
    if (Pickup)
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
        bSucceeded=Delivery->TryDeliver(Errand.Package,OwnerPlayerState);
        if (!IsCurrent()) return;
        if (bSucceeded) { FinishErrand(EDeliveryNpcActionStatus::Succeeded,TEXT("Delivered")); return; }
        Failure=TEXT("DeliveryRejected");
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
            bReceiving?Npc:OwnerCarrierPawn,Failure,Errand.Package,Continue);
        if (!IsCurrent()) return;
        if (bSucceeded)
        {
            if (bReceiving && Errand.Action==EDeliveryNpcAction::DeliverPackage)
            {
                SetErrandPhase(EDeliveryCompanionErrandPhase::GoToDelivery);
                Npc->ObserveFact(TEXT("CompanionPackageReceived"),OwnerCarrierPawn,TEXT("游戏确认主人手里的同一个包裹已经交到我手里，尚未送达。"),EDeliveryNpcEvidenceSource::ActionResult);
            }
            else FinishErrand(EDeliveryNpcActionStatus::Succeeded,bReceiving?TEXT("PackageReceived"):TEXT("PackageReturned"));
            return;
        }
    }
    // 距离内也可能仍隔着门或站着别人，给短暂移动让路留出时间，不把走近当成交接成功
    ErrandInteractionSeconds+=Dt;
    Errand.Reason=Failure;
    if (ErrandInteractionSeconds>=GetTuning().ErrandInteractionTimeoutSeconds)
        FinishErrand(EDeliveryNpcActionStatus::Failed,Failure);
}
