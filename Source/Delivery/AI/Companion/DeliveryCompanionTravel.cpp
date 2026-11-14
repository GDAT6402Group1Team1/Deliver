#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "Vehicle/DeliveryCompanionMotorbike.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Carry/DeliveryPackageCarrierInterface.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Engine/World.h"

namespace
{
    bool AllowsTravelChoice(EDeliveryNpcAction Action)
    {
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

bool UDeliveryCompanionComponent::CanTakeNearbyPackage() const
{
    const auto* Npc=GetCompanion();
    const auto* CarrierPawn=GetOwnerPackageCarrier();
    const auto* Carrier=Cast<IDeliveryPackageCarrierInterface>(CarrierPawn);
    const auto* Package=Carrier?Carrier->GetCarriedTaskPackage():nullptr;
    if (!Npc || !CanCommand(OwnerPlayerState) || !IsValid(CarrierPawn) || !IsValid(Package) ||
        CarrierPawn->GetWorld()!=GetWorld() || Carrier->GetTaskPlayerState()!=OwnerPlayerState) return false;
    const FVector Goal=Carrier->GetTaskPackageTransferPoint();
    if (Goal.ContainsNaN() || FVector::DistSquared(Npc->GetActorLocation(),Goal)>FMath::Square(240.f)) return false;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionNearbyPackage),false,Npc);
    Query.AddIgnoredActor(CarrierPawn);
    Query.AddIgnoredActor(Package);
    FCollisionObjectQueryParams Objects;
    for (const auto Channel:{ECC_WorldStatic,ECC_WorldDynamic,ECC_Pawn,ECC_Vehicle,ECC_PhysicsBody}) Objects.AddObjectTypesToQuery(Channel);
    FHitResult Hit;
    return !GetWorld()->LineTraceSingleByObjectType(Hit,Npc->GetActorLocation(),Goal,Objects,Query);
}

void UDeliveryCompanionComponent::ExtendTravelDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const
{
    Request.RequestedCapabilityVersion=8;
    const auto CurrentMode=Mode==EDeliveryCompanionMode::Follow && !HasActiveErrand()?
        (bRideWhileFollowing?EDeliveryCompanionTravelMode::OwnVehicle:EDeliveryCompanionTravelMode::Walk):TravelMode;
    Request.CompanionTravelMode=StaticEnum<EDeliveryCompanionTravelMode>()->GetNameStringByValue(int64(CurrentMode));
    Request.bCompanionOwnVehicleAvailable=IsValid(ErrandVehicle) && ErrandVehicle->GetWorld()==GetWorld() &&
        (!ErrandVehicle->GetReservedCompanion() || ErrandVehicle->GetReservedCompanion()==GetOwner());
    if (const auto* Task=GetCommandTask())
        Request.CompanionTaskDescription=(Task->SimpleDescription.ToString()+TEXT(" ")+Task->DetailedDescription.ToString()).TrimStartAndEnd().Left(512);
    if (!CanCommand(Requester) || GetCompanion()->IsKnockedDown()) return;
    const TArray<FDeliveryNpcCandidate> Original=MoveTemp(Request.Candidates);
    for (const auto& Candidate:Original)
    {
        if (!AllowsTravelChoice(Candidate.Action)) { Request.Candidates.Add(Candidate); continue; }
        // 初次跟随默认步行，单纯接过手边货物也不替主人决定远行方式
        if (TravelMode!=EDeliveryCompanionTravelMode::Unspecified || Candidate.Action==EDeliveryNpcAction::Follow ||
            (Candidate.Action==EDeliveryNpcAction::TakePackage && CanTakeNearbyPackage()))
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
