#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "World/DeliveryHome.h"
#include "AIController.h"
#include "Navigation/PathFollowingComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"

bool UDeliveryCompanionComponent::CanGoHome(APlayerState* Requester) const
{
    const auto* Home=ADeliveryHome::FindSharedHome(this);
    FVector Destination;
    return CanCommand(Requester) && !GetCompanion()->IsKnockedDown() && Home &&
        Home->TryGetReturnLocation(Destination) && Home->TryGetInteriorLocation(Destination);
}

void UDeliveryCompanionComponent::UpdateGoHome()
{
    auto* Npc=GetCompanion();
    if (!IsValid(Npc) || !Npc->HasAuthority() || Mode!=EDeliveryCompanionMode::GoHome) return;
    auto* AI=Cast<AAIController>(Npc->GetController());
    if (!AI || Npc->IsKnockedDown() || Npc->Agent->ShouldPauseForConversation() ||
        (Npc->Agent->HasActiveAction() && !Npc->Agent->CanContinueCompanionMovement()))
    { StopFollowMovement(); return; }
    const int32 Revision=CommandRevision;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const TWeakObjectPtr<AAIController> WeakAI(AI);
    const TWeakObjectPtr<APlayerState> ExpectedOwner(OwnerPlayerState);
    const TWeakObjectPtr<APawn> ExpectedOwnerPawn(GetOwnerPawn());
    const auto IsCurrent=[&](EDeliveryCompanionMode ExpectedMode=EDeliveryCompanionMode::GoHome)
    {
        return WeakThis.IsValid() && WeakNpc.IsValid() && !Npc->IsActorBeingDestroyed() && WeakAI.IsValid() &&
            Npc->GetController()==AI && AI->GetPawn()==Npc && CommandRevision==Revision && Mode==ExpectedMode &&
            CanCommand(ExpectedOwner.Get()) &&
            TWeakObjectPtr<APawn>(GetOwnerPawn()).HasSameIndexAndSerialNumber(ExpectedOwnerPawn);
    };
    auto* Home=ADeliveryHome::FindSharedHome(this);
    const TWeakObjectPtr<ADeliveryHome> WeakHome(Home);
    FVector Destination,ParkingApproach;
    if (!Home || !Home->TryGetReturnLocation(ParkingApproach) || !Home->TryGetInteriorLocation(Destination))
    {
        StopFollowMovement();
        if (IsCurrent()) SetSuspension(TEXT("HomeUnavailable"));
        return;
    }
    if (!UpdateVehicleTravel(ParkingApproach,60.f,EDeliveryCompanionTravelPurpose::GoHome) || !IsCurrent() ||
        !WeakHome.IsValid() || Home->IsActorBeingDestroyed()) return;
    // 最后一段沿门洞导航进入屋内，车辆不使用室内地面点
    FAIMoveRequest Move(Destination);
    Move.SetAcceptanceRadius(60);
    Move.SetReachTestIncludesAgentRadius(false); Move.SetReachTestIncludesGoalRadius(false);
    Move.SetAllowPartialPath(false);
    auto* Path=AI->GetPathFollowingComponent();
    if (Path && Path->HasReached(Move))
    {
        StopFollowMovement();
        // 停导航会同步通知监听者，旧回家动作不能覆盖随后接到的安排
        if (!IsCurrent() || !WeakHome.IsValid() || Home->IsActorBeingDestroyed() ||
            ADeliveryHome::FindSharedHome(this)!=Home) return;
        Mode=EDeliveryCompanionMode::Stay;
        WaitLocation=Npc->GetNavAgentLocation();
        SetSuspension(NAME_None);
        RecordMemory(FGuid::NewGuid(),TEXT("ReturnedHome"),TEXT("我已经沿路穿过门洞回到共用房屋里面，留在屋内等主人回来"),0);
        Npc->ObserveFact(TEXT("CompanionHomeArrived"),Home,TEXT("我实际走进了已经认领的共用房屋，现在留在屋内等候主人"),EDeliveryNpcEvidenceSource::ActionResult);
        if (IsCurrent(EDeliveryCompanionMode::Stay)) Npc->ForceNetUpdate();
        return;
    }
    const double Now=GetWorld()->GetTimeSeconds();
    if (Now<NextNavigationAt || (bOwnsNavigation && Path && Path->GetCurrentRequestId()==FollowMoveId &&
        AI->GetMoveStatus()!=EPathFollowingStatus::Idle)) return;
    const auto Result=AI->MoveTo(Move);
    if (!IsCurrent() || !WeakHome.IsValid() || Home->IsActorBeingDestroyed())
    {
        // 只撤销这次回家刚发的请求，保护回调里接管导航的新动作
        if (WeakAI.IsValid() && Result.Code==EPathFollowingRequestResult::RequestSuccessful && Result.MoveId.IsValid())
            if (auto* CurrentPath=AI->GetPathFollowingComponent(); CurrentPath && CurrentPath->GetCurrentRequestId()==Result.MoveId)
                CurrentPath->AbortMove(*AI,FPathFollowingResultFlags::UserAbort,Result.MoveId);
        return;
    }
    NextNavigationAt=Now+GetTuning().BlockedRetrySeconds;
    bOwnsNavigation=Result.Code==EPathFollowingRequestResult::RequestSuccessful;
    FollowMoveId=bOwnsNavigation?Result.MoveId:FAIRequestID::InvalidRequest;
    SetSuspension(Result.Code==EPathFollowingRequestResult::Failed?FName(TEXT("HomePathBlocked")):NAME_None);
}
