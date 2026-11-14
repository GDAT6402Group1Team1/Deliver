#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Carry/DeliveryPackageCarrierInterface.h"
#include "Character/DeliveryCharacter.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Tracking/DeliveryTaskTrackerComponent.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/World/DeliveryPickupPointComponent.h"
#include "Task/World/DeliveryTargetComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Vehicle/DeliveryCompanionMotorbike.h"
#include "World/DeliveryLocationRegistry.h"
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
    if (auto* Bike=Cast<ADeliveryMotorbike>(Pawn)) return Bike->GetDriver();
    return Pawn;
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
    UDeliveryTaskDefinition* ExpectedTask,ADeliveryPackageItem* ExpectedPackage)
{
    if (!GetOwner()->HasAuthority() || ExpectedRevision!=CommandRevision || !CanArrangeErrand(Action,Requester) ||
        !IsValid(ExpectedTask) || ExpectedTask!=GetCommandTask() || ExpectedPackage!=GetCommandPackage()) return false;
    // 先核对新请求再停止旧安排，迟到或无效的建议不能破坏正在执行的委托
    CancelHomeArrangement(TEXT("OwnerChangedArrangement"));
    if (HasActiveErrand()) FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("ReplacedByOwner"));
    StopFollowMovement();
    StopErrandMovement();
    ++CommandRevision;
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

void UDeliveryCompanionComponent::StopErrandMovement()
{
    if (!GetOwner() || !GetOwner()->HasAuthority()) return;
    StopVehicleTravel(EDeliveryCompanionTravelPurpose::Errand);
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
    StopErrandMovement();
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
    if (!HasActiveErrand() || !GetOwner()->HasAuthority()) return;
    StopErrandMovement();
    Errand.Status=Status;
    Errand.Phase=EDeliveryCompanionErrandPhase::Idle;
    Errand.Reason=Reason;
    Errand.Package=nullptr;
    ++CommandRevision;
    bHasFollowDestination=false;
    NextNavigationAt=0;
    // 实际订单在此结束，之后按原持续安排返程，保留主人指定的等候点
    SetSuspension(NAME_None);
    GetOwner()->ForceNetUpdate();
    // 只根据同步物品接口的实际结果报告，接受委托、走到附近均不算取件或交付成功
    GetCompanion()->ObserveFact(TEXT("CompanionErrandFinished"),GetOwnerPawn(),
        FString::Printf(TEXT("游戏确认二狗的 %s 委托已结束：%s，原因 %s，订单 %s。当前二狗%s实际包裹；只有 Delivered 表示已完成投递并由任务系统处理奖励。"),
            *DeliveryNpc::ActionName(Errand.Action),*StaticEnum<EDeliveryNpcActionStatus>()->GetNameStringByValue(static_cast<int64>(Status)),
            *Reason.ToString(),Errand.Task?*Errand.Task->TaskId.ToString():TEXT("未知"),GetCarriedTaskPackage()?TEXT("仍持有"):TEXT("没有持有")),
        EDeliveryNpcEvidenceSource::ActionResult);
}

void UDeliveryCompanionComponent::ReportErrandPause(FName Reason)
{
    StopErrandMovement();
    SetSuspension(Reason);
    if (LastErrandPause==Reason) return;
    LastErrandPause=Reason;
    GetCompanion()->ObserveFact(TEXT("CompanionErrandPaused"),GetOwnerPawn(),
        FString::Printf(TEXT("当前跑腿暂时停下：%s。委托和实际包裹仍保留，没有确认交接或送达；条件恢复后继续，主人也可以改安排。"),*Reason.ToString()),
        EDeliveryNpcEvidenceSource::ActionResult);
}

AActor* UDeliveryCompanionComponent::ResolveErrandLocation(bool bPickup) const
{
    return FindErrandLocation(this,Errand.Task,bPickup);
}

bool UDeliveryCompanionComponent::MoveForErrand(const FVector& Goal,float ReachRadius,AActor* InteractionTarget)
{
    auto* Npc=GetCompanion();
    auto* AI=Cast<AAIController>(Npc->GetController());
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!AI || !Nav || Goal.ContainsNaN()) return false;
    const double Now=GetWorld()->GetTimeSeconds();
    const bool bGoalUnchanged=bHasErrandDestination && FVector::Dist2D(Goal,ErrandGoal)<GetTuning().RepathDistance;
    if (bGoalUnchanged)
    {
        if (AI->GetMoveStatus()!=EPathFollowingStatus::Idle && bErrandOwnsNavigation) return false;
        if (FVector::Dist2D(Npc->GetNavAgentLocation(),ErrandDestination)<45 &&
            FMath::Abs(Npc->GetNavAgentLocation().Z-ErrandDestination.Z)<60 &&
            FVector::Dist(Npc->GetActorLocation(),Goal)<=ReachRadius &&
            HasClearErrandApproach(Npc,Npc->GetActorLocation(),Goal,InteractionTarget)) return true;
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
        if (Result.Code==EPathFollowingRequestResult::Failed) continue;
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
    StopErrandMovement();
    bHasErrandDestination=false;
    NextErrandNavigationAt=Now+GetTuning().BlockedRetrySeconds;
    ReportErrandPause(TEXT("ErrandPathBlocked"));
    return false;
}

void UDeliveryCompanionComponent::UpdateErrand()
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority() || !HasActiveErrand()) return;
    if (HasActiveHomeArrangement() || PendingHomeArrangementId.IsValid())
    { StopErrandMovement(); UpdateHomeArrangement(); return; }
    if (!CanCommand(OwnerPlayerState)) { FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("OwnerLost")); return; }
    if (Npc->IsKnockedDown() || Npc->Agent->ShouldPauseForConversation() || Npc->Agent->HasActiveAction())
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
    APawn* OwnerCarrierPawn=GetOwnerPackageCarrier();
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
    const float Dt=LastErrandUpdateAt>0?FMath::Clamp(static_cast<float>(Now-LastErrandUpdateAt),0.f,1.f):0.f;
    LastErrandUpdateAt=Now;
    ErrandActiveSeconds+=Dt;
    if (ErrandActiveSeconds>=ErrandTimeLimitSeconds)
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("ErrandTimedOut")); return; }

    FVector Goal;
    float ReachRadius=220;
    UDeliveryPickupPointComponent* Pickup=nullptr;
    UDeliveryTargetComponent* Delivery=nullptr;
    UDeliveryInteractableComponent* Interaction=nullptr;
    AActor* InteractionTarget=OwnerCarrierPawn;
    if (bNeedsOwner) Goal=OwnerCarrierPawn->GetActorLocation();
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
            Goal=Delivery->GetDeliveryPoint(Npc->GetActorLocation());
            ReachRadius=Delivery->DeliveryRadius;
        }
    }
    const bool bOnFoot=UpdateVehicleTravel(Goal,ReachRadius,EDeliveryCompanionTravelPurpose::Errand);
    if (!IsCurrent()) return;
    if (!bOnFoot) { LastErrandUpdateAt=Now; return; }
    const bool bInReach=(bNeedsOwner?FVector::Dist(Npc->GetActorLocation(),Goal)<=220:
        Pickup?Interaction->CanInteract(Npc):Delivery->CanAcceptDelivery(Errand.Package,OwnerPlayerState)) &&
        HasClearErrandApproach(Npc,Npc->GetActorLocation(),Goal,InteractionTarget);
    const bool bAtDestination=bInReach || MoveForErrand(Goal,ReachRadius,InteractionTarget);
    // 路线重试也算本次执行时间；BT中断和等主人恢复时则重置采样起点
    LastErrandUpdateAt=Now;
    if (!bAtDestination) { ErrandInteractionSeconds=0; return; }
    StopErrandMovement();
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
        bSucceeded=DeliveryPackageTransfer::TryTransfer(bReceiving?OwnerCarrierPawn:Npc,bReceiving?Npc:OwnerCarrierPawn,Failure);
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
