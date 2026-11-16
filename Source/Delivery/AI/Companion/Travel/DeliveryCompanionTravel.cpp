#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/Companion/DeliveryCompanionMotorbike.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Carry/DeliveryPackageCarrierInterface.h"
#include "Inventory/Items/DeliveryPackageItem.h"
#include "Engine/World.h"

namespace
{
    bool AllowsTravelChoice(EDeliveryNpcAction Action)
    {
        if (Action==EDeliveryNpcAction::CollectCaterpillar || Action==EDeliveryNpcAction::InsertCaterpillar ||
            Action==EDeliveryNpcAction::SearchNearOwner) return false;
        return Action==EDeliveryNpcAction::Follow || Action==EDeliveryNpcAction::GoHome ||
            DeliveryNpc::IsCompanionErrand(Action) || DeliveryNpc::IsCompanionItemErrand(Action);
    }
}

void UDeliveryCompanionComponent::ApplyTravelInstruction(EDeliveryCompanionTravelMode NewMode)
{
    // 只在服务器接受新安排的位置调用，短对白和空参数都保留主人的上次选择
    if (GetOwner()->HasAuthority() && NewMode!=EDeliveryCompanionTravelMode::Unspecified)
        TravelMode=NewMode;
}

bool UDeliveryCompanionComponent::CanTransferNearbyPackage(EDeliveryNpcAction Action) const
{
    const bool bFetch=Action==EDeliveryNpcAction::FetchPackage;
    const bool bReturningGroundPackage=bFetch && HasActiveErrand() && Errand.Action==Action && Errand.bRecoverGroundPackage &&
        Errand.Phase==EDeliveryCompanionErrandPhase::ReturnToOwner && IsValid(Errand.Package) && GetCarriedTaskPackage()==Errand.Package;
    const bool bReturning=Action==EDeliveryNpcAction::ReturnPackage || Action==EDeliveryNpcAction::DropPackage ||
        Action==EDeliveryNpcAction::StorePackageInBackpack || bReturningGroundPackage;
    if (!bReturning && !bFetch && Action!=EDeliveryNpcAction::TakePackage) return false;
    const auto* Npc=GetCompanion();
    if (!IsValid(Npc) || !CanCommand(OwnerPlayerState)) return false;
    const auto CanReach=[&](const FVector& From,const FVector& Goal,const AActor* Receiver,const ADeliveryPackageItem* Package)
    {
        if (From.ContainsNaN() || Goal.ContainsNaN() || FVector::DistSquared(From,Goal)>FMath::Square(220.f)) return false;
        FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionNearbyPackage),false,Npc);
        if (Receiver) Query.AddIgnoredActor(Receiver);
        Query.AddIgnoredActor(Package);
        FCollisionObjectQueryParams Objects;
        for (const auto Channel:{ECC_WorldStatic,ECC_WorldDynamic,ECC_Pawn,ECC_Vehicle,ECC_PhysicsBody}) Objects.AddObjectTypesToQuery(Channel);
        return !GetWorld()->LineTraceTestByObjectType(From,Goal,Objects,Query);
    };
    if (!bReturning)
    {
        if (const auto* GroundPackage=GetOwnerDroppedPackage(); IsValid(GroundPackage) && GetCommandPackage()==GroundPackage)
        {
            const FVector Point=GroundPackage->GetActorLocation();
            if (!CanReach(Npc->GetActorLocation(),Point,nullptr,GroundPackage)) return false;
            if (!bFetch) return true;
            // 取来交还要同时核对地上原件和主人，普通取件点仍需要出行方式
            const auto* Player=Cast<ADeliveryCharacter>(GetOwnerPawn());
            return IsValid(Player) && Player->GetTaskPlayerState()==OwnerPlayerState && Player->CanHandleTaskPackage() &&
                CanReach(Point,Player->GetTaskPackageTransferPoint(),Player,GroundPackage);
        }
        if (bFetch) return false;
    }
    const APawn* CarrierPawn=bReturning?Cast<ADeliveryCharacter>(GetOwnerPawn()):GetOwnerPackageCarrier();
    const auto* Carrier=Cast<IDeliveryPackageCarrierInterface>(CarrierPawn);
    const auto* Package=bReturning?GetCarriedTaskPackage():Carrier?Carrier->GetCarriedTaskPackage():nullptr;
    if (!IsValid(CarrierPawn) || !IsValid(Package) ||
        CarrierPawn->GetWorld()!=GetWorld() || Carrier->GetTaskPlayerState()!=OwnerPlayerState) return false;
    return CanReach(Npc->GetActorLocation(),Carrier->GetTaskPackageTransferPoint(),CarrierPawn,Package);
}

void UDeliveryCompanionComponent::ExtendTravelDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const
{
    Request.RequestedCapabilityVersion=FDeliveryNpcDecisionRequest::CapabilityVersion;
    auto CurrentMode=TravelMode;
    const auto* TravelModes=StaticEnum<EDeliveryCompanionTravelMode>();
    Request.CompanionErrandTravelMode.Reset();
    if (HasActiveErrand() && Errand.bLocalItemTransfer)
        CurrentMode=EDeliveryCompanionTravelMode::Walk;
    else if (Mode==EDeliveryCompanionMode::Follow && !HasActiveErrand())
    {
        CurrentMode=FollowTravelMode;
        Request.CompanionErrandTravelMode=TravelModes->GetNameStringByValue(int64(TravelMode));
    }
    Request.CompanionTravelMode=TravelModes->GetNameStringByValue(int64(CurrentMode));
    Request.bCompanionOwnVehicleAvailable=IsValid(ErrandVehicle) && ErrandVehicle->GetWorld()==GetWorld() &&
        (!ErrandVehicle->GetReservedCompanion() || ErrandVehicle->GetReservedCompanion()==GetOwner());
    // 出行约定与座椅附着分开，出口受阻时仍按实际骑乘状态报告
    const auto* Npc=GetCompanion();
    const bool bMounted=Npc && Npc->IsMountedOnCompanionBike();
    Request.CompanionVehicleState=bMounted?TEXT("Mounted"):TEXT("NotMounted");
    Request.CompanionOwnerVehicleState.Reset();
    if (Request.bPlayerRequest && Request.bRequesterIsCompanionOwner && CanCommand(Requester))
    {
        // 以主人实际控制的 Pawn 为准，交接中暂缺 Pawn 时省略骑乘状态
        const auto* OwnerPawn=GetOwnerPawn();
        if (IsValid(OwnerPawn) && !OwnerPawn->IsActorBeingDestroyed() && OwnerPawn->GetWorld()==GetWorld())
        {
            if (const auto* Bike=Cast<ADeliveryMotorbike>(OwnerPawn))
            {
                const auto* Driver=Bike->GetDriver();
                if (IsValid(Driver) && !Driver->IsActorBeingDestroyed() && Driver->GetWorld()==GetWorld() &&
                    Driver->GetTaskPlayerState()==Requester && Driver->GetCurrentVehicle()==Bike)
                    Request.CompanionOwnerVehicleState=TEXT("Riding");
            }
            else if (const auto* Walker=Cast<ADeliveryCharacter>(OwnerPawn); Walker &&
                Walker->GetTaskPlayerState()==Requester && !Walker->GetCurrentVehicle())
                Request.CompanionOwnerVehicleState=TEXT("OnFoot");
        }
    }
    if (Request.bPlayerRequest && Request.bRequesterIsCompanionOwner)
    {
        const auto* Held=GetCarriedItem();
        const auto* Parent=Held && Held->GetRootComponent()?Held->GetRootComponent()->GetAttachParent():nullptr;
        if (IsValid(Parent) && Npc)
        {
            // 以此刻实物挂点为准，骑上车或占用手槽都不能单独证明物品位置
            if (bMounted)
            {
                const auto* Bike=CastChecked<ADeliveryCompanionMotorbike>(Npc->GetAttachParentActor());
                if (Parent==Bike->CompanionCargoAnchor.Get()) Request.CompanionCarriedItemLocation=TEXT("OwnBikeRack");
            }
            else if (Parent==Npc->HandheldAnchor.Get() || (Request.bCompanionHasPackage && Parent==Npc->PackageAnchor.Get()))
                Request.CompanionCarriedItemLocation=TEXT("Hand");
        }
    }
    const auto* ParkedFollowPawn=VehicleFollowOwnerPawn.Get();
    if (Request.CompanionVehicleState==TEXT("NotMounted") && Mode==EDeliveryCompanionMode::Follow &&
        FollowTravelMode!=EDeliveryCompanionTravelMode::Walk && !HasActiveErrand() && !HasActiveHomeArrangement() && !PendingHomeArrangementId.IsValid() &&
        !HomeState.CarriedProp && Request.bCompanionOwnVehicleAvailable && !ErrandVehicle->IsActorBeingDestroyed() &&
        FollowOnFootRevision==CommandRevision && IsValid(ParkedFollowPawn) && !ParkedFollowPawn->IsActorBeingDestroyed() &&
        GetOwnerPawn()==ParkedFollowPawn)
        Request.CompanionVehicleState=TEXT("ParkedForWalkingFollow");
    if (const auto* Task=GetCommandTask())
        Request.CompanionTaskDescription=(Task->SimpleDescription.ToString()+TEXT(" ")+Task->DetailedDescription.ToString()).TrimStartAndEnd().Left(512);
    if (!CanCommand(Requester) || GetCompanion()->IsKnockedDown()) return;
    const TArray<FDeliveryNpcCandidate> Original=MoveTemp(Request.Candidates);
    for (const auto& Candidate:Original)
    {
        if (!AllowsTravelChoice(Candidate.Action)) { Request.Candidates.Add(Candidate); continue; }
        // 手边的货物与普通道具可以当场取还
        if (TravelMode!=EDeliveryCompanionTravelMode::Unspecified || Candidate.Action==EDeliveryNpcAction::Follow ||
            CanTransferNearbyPackage(Candidate.Action) ||
            (Candidate.ItemChoice.IsValid() && CanWalkForNearbyItem(*Candidate.ItemChoice)))
            Request.Candidates.Add(Candidate);
        for (const auto Choice:{EDeliveryCompanionTravelMode::Walk,EDeliveryCompanionTravelMode::OwnVehicle,EDeliveryCompanionTravelMode::Automatic})
        {
            if (Choice==EDeliveryCompanionTravelMode::OwnVehicle && !Request.bCompanionOwnVehicleAvailable) continue;
            auto Variant=Candidate;
            Variant.CandidateId.Invalidate();
            Variant.TravelMode=Choice;
            Request.Candidates.Add(MoveTemp(Variant));
        }
    }
}
