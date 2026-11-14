#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "World/DeliveryHome.h"
#include "AIController.h"
#include "Navigation/PathFollowingComponent.h"
#include "Engine/World.h"

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
    auto* AI=Npc?Cast<AAIController>(Npc->GetController()):nullptr;
    if (!AI || Npc->IsKnockedDown() || Npc->Agent->ShouldPauseForConversation() || Npc->Agent->HasActiveAction())
    { StopFollowMovement(); return; }
    const auto* Home=ADeliveryHome::FindSharedHome(this);
    FVector Destination,ParkingApproach;
    if (!Home || !Home->TryGetReturnLocation(ParkingApproach) || !Home->TryGetInteriorLocation(Destination))
    { StopFollowMovement(); SetSuspension(TEXT("HomeUnavailable")); return; }
    if (!UpdateVehicleTravel(ParkingApproach,60.f,EDeliveryCompanionTravelPurpose::GoHome)) return;
    // 最后一段沿门洞导航进入屋内，车辆不使用室内地面点
    FAIMoveRequest Move(Destination);
    Move.SetAcceptanceRadius(60);
    Move.SetReachTestIncludesAgentRadius(false); Move.SetReachTestIncludesGoalRadius(false);
    Move.SetAllowPartialPath(false);
    auto* Path=AI->GetPathFollowingComponent();
    if (Path && Path->HasReached(Move))
    {
        StopFollowMovement();
        Mode=EDeliveryCompanionMode::Stay;
        WaitLocation=Npc->GetNavAgentLocation();
        SetSuspension(NAME_None);
        RecordMemory(FGuid::NewGuid(),TEXT("ReturnedHome"),TEXT("我已经沿路穿过门洞回到共用房屋里面，留在屋内等主人回来"),0);
        Npc->ObserveFact(TEXT("CompanionHomeArrived"),const_cast<ADeliveryHome*>(Home),TEXT("我实际走进了已经认领的共用房屋，现在留在屋内等候主人"),EDeliveryNpcEvidenceSource::ActionResult);
        Npc->ForceNetUpdate();
        return;
    }
    const double Now=GetWorld()->GetTimeSeconds();
    if (Now<NextNavigationAt || (bOwnsNavigation && AI->GetMoveStatus()!=EPathFollowingStatus::Idle)) return;
    NextNavigationAt=Now+GetTuning().BlockedRetrySeconds;
    const auto Result=AI->MoveTo(Move);
    bOwnsNavigation=Result.Code==EPathFollowingRequestResult::RequestSuccessful;
    FollowMoveId=bOwnsNavigation?Result.MoveId:FAIRequestID::InvalidRequest;
    SetSuspension(Result.Code==EPathFollowingRequestResult::Failed?FName(TEXT("HomePathBlocked")):NAME_None);
}
