#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "Character/DeliveryCharacter.h"
#include "Inventory/DeliveryCaterpillarItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Interaction/DeliveryInteractionRegistry.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "World/DeliveryLocationRegistry.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"

ADeliveryCaterpillarItem* UDeliveryCompanionComponent::FindReachableYardCaterpillar(const APawn* Pawn,float Radius) const
{
    const auto* Registry=GetWorld()?GetWorld()->GetSubsystem<UDeliveryInteractionRegistry>():nullptr;
    if (!Registry || !IsValid(Pawn) || Pawn->GetWorld()!=GetWorld()) return nullptr;
    const FVector Origin=Pawn->GetActorLocation();
    TArray<UDeliveryInteractableComponent*> Nearby;
    Registry->GetRegisteredInRadius(Origin,Radius,Nearby);
    FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionYardCaterpillar),false,Pawn);
    Query.AddIgnoredActor(GetOwner());
    ADeliveryCaterpillarItem* Nearest=nullptr;
    double NearestSquared=FMath::Square(Radius);
    for (const auto* Interaction:Nearby)
    {
        auto* Worm=Cast<ADeliveryCaterpillarItem>(Interaction->GetOwner());
        if (Interaction!=DeliveryHandheldTransfer::FindWorldPickupInteraction(Worm,GetWorld()) ||
            Worm->GetItemComponent()->IsTransferInProgress()) continue;
        const FVector Point=Interaction->GetInteractionPoint(Origin);
        const double DistanceSquared=FVector::DistSquared(Origin,Point);
        if (Point.ContainsNaN() || DistanceSquared>NearestSquared) continue;
        FHitResult Hit;
        if (GetWorld()->LineTraceSingleByChannel(Hit,Origin+FVector(0,0,35),Point,ECC_Visibility,Query) && Hit.GetActor()!=Worm) continue;
        Nearest=Worm;
        NearestSquared=DistanceSquared;
    }
    return Nearest;
}

bool UDeliveryCompanionComponent::HasCaterpillarOpportunity() const
{
    const auto* Pawn=GetOwnerPawn();
    return CaterpillarOpportunityId.IsValid() && !bCaterpillarOffered && GetWorld() &&
        GetWorld()->GetTimeSeconds()<CaterpillarOpportunityExpiresAt && CanOfferCaterpillar(OwnerPlayerState) &&
        Pawn && FVector::DistSquared(Pawn->GetActorLocation(),GetOwner()->GetActorLocation())<=FMath::Square(900.f);
}

bool UDeliveryCompanionComponent::CanRequestCaterpillarOffer(APlayerState* Requester) const
{
    if (bCaterpillarOffered) return false;
    // 没有玩家请求者时只允许来自当前院子机会的提议，访客仍必须按自己的真实身份拒绝
    return Requester?CanOfferCaterpillar(Requester):HasCaterpillarOpportunity();
}

void UDeliveryCompanionComponent::UpdateCaterpillarOpportunity()
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority()) return;
    const auto* Pawn=GetOwnerPawn();
    const bool bInPlace=CanOfferCaterpillar(OwnerPlayerState) && Pawn &&
        FVector::DistSquared(Pawn->GetActorLocation(),Npc->GetActorLocation())<=FMath::Square(900.f);
    if (!bInPlace)
    {
        CaterpillarOpportunityId.Invalidate(); CaterpillarOpportunityExpiresAt=0;
        bAnnouncedCaterpillarOpportunity=false;
        if (bCaterpillarOffered) { bCaterpillarOffered=false; Npc->ForceNetUpdate(); }
        return;
    }
    const double Now=GetWorld()->GetTimeSeconds();
    if (CaterpillarOpportunityId.IsValid() && Now>=CaterpillarOpportunityExpiresAt) CaterpillarOpportunityId.Invalidate();
    if (bCaterpillarOffered || bAnnouncedCaterpillarOpportunity || Now<NextCaterpillarOpportunityAt ||
        Npc->Agent->IsListening() || Npc->Agent->IsWaitingToReplyToPlayer() || Npc->Agent->IsDecisionPending()) return;
    FDeliveryNpcObservation Opportunity;
    Opportunity.EventId=FGuid::NewGuid(); Opportunity.Kind=TEXT("CaterpillarOpportunity");
    Opportunity.Source=EDeliveryNpcEvidenceSource::ActionResult; Opportunity.Subject=const_cast<APawn*>(Pawn);
    Opportunity.Description=GetCarriedTaskPackage()?
        TEXT("我还拿着第一单的原包裹，附近有小绿。可以询问主人要不要我把这只小绿放进手里的原包裹，等主人同意再行动"):
        TEXT("我和主人到了第一单收件地点附近，原包裹还没交付也没加毛毛虫。附近的小绿可以拿来；可以提议恶作剧，拿取和投放要由主人另行同意");
    Opportunity.Lifetime=45; Opportunity.bNeedsReply=false;
    if (!Npc->Agent->ObserveEvent(Opportunity)) return;
    bAnnouncedCaterpillarOpportunity=true;
    CaterpillarOpportunityId=Opportunity.EventId;
    CaterpillarOpportunityExpiresAt=Now+Opportunity.Lifetime;
    NextCaterpillarOpportunityAt=Now+60;
    // 这里只催一次正常模型请求，最终可选提议、说别的话或保持安静
    Npc->WakeDecision();
}

bool UDeliveryCompanionComponent::CanOfferCaterpillar(APlayerState* Requester) const
{
    const auto* Npc=GetCompanion();
    const auto* Task=GetCommandTask();
    const auto* Manager=UDeliveryTaskManagerComponent::Get(this);
    const auto* Registry=UDeliveryLocationRegistry::Get(this);
    const auto* Package=GetCommandPackage();
    FVector Destination;
    if (!(Npc && CanCommand(Requester) && !bCaterpillarGiven && !bGivingCaterpillar && !Npc->IsKnockedDown() &&
        Task && Task->TaskId==Npc->EncounterTaskId &&
        Manager && Manager->GetTaskStatus(Task)==EDeliveryTaskStatus::InProgress && Package && !Package->ContainsCaterpillar() &&
        Registry && Registry->ResolveLocation(Task->DeliveryLocationId,Destination) &&
        FVector::DistSquared(Npc->GetActorLocation(),Destination)<=FMath::Square(900.f))) return false;
    auto* Nearby=FindReachableYardCaterpillar(GetOwnerPawn(),DeliveryCompanionItemSearch::Radius);
    if (GetCarriedTaskPackage()==Package)
        return CanArrangeCaterpillar(EDeliveryNpcAction::InsertCaterpillar,Nearby,Requester,true);
    return Cast<ADeliveryCaterpillarItem>(GetCarriedItem()) || Nearby;
}

bool UDeliveryCompanionComponent::CanAcceptCaterpillar(APawn* Interactor) const
{
    const auto* Player=Cast<ADeliveryCharacter>(Interactor);
    if (!Player || !bCaterpillarOffered || !CanOfferCaterpillar(Player->GetTaskPlayerState()) ||
        !Player->CanHandleTaskPackage() || Player->GetWorld()!=GetWorld() || !Player->GetInventoryComponent()->HasBackpackSpace() ||
        FVector::DistSquared(Player->GetActorLocation(),GetOwner()->GetActorLocation())>FMath::Square(180.f)) return false;
    auto* Worm=Cast<ADeliveryCaterpillarItem>(GetCarriedItem());
    if (!Worm)
    {
        Worm=FindReachableYardCaterpillar(Player,180.f);
        const auto* Interaction=UDeliveryInteractableComponent::FindOn(Worm,EDeliveryInteractionKey::PickupE);
        if (!Interaction || !Interaction->CanInteract(Player)) return false;
    }
    FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionGiveCaterpillar),false,GetOwner());
    Params.AddIgnoredActor(Player);
    FHitResult Hit;
    return !GetWorld()->LineTraceSingleByChannel(Hit,GetOwner()->GetActorLocation(),Player->GetActorLocation(),ECC_Visibility,Params);
}

bool UDeliveryCompanionComponent::AcceptCaterpillar(APawn* Interactor)
{
    if (!GetOwner()->HasAuthority() || !CanAcceptCaterpillar(Interactor) || !EnsureOnFoot()) return false;
    auto* Player=CastChecked<ADeliveryCharacter>(Interactor);
    auto* Npc=GetCompanion();
    auto* Held=Cast<ADeliveryCaterpillarItem>(GetCarriedItem());
    auto* Worm=Held?Held:FindReachableYardCaterpillar(Player,180.f);
    if (!IsValid(Npc) || !Worm) return false;
    bGivingCaterpillar=true;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<APlayerState> ReceivingOwner=OwnerPlayerState;
    const TWeakObjectPtr<ADeliveryCharacter> WeakPlayer(Player);
    const int32 Revision=CommandRevision;
    const auto Current=[WeakThis,ReceivingOwner,WeakPlayer,Revision]()
    {
        return WeakThis.IsValid() && WeakThis->CommandRevision==Revision && WeakThis->bGivingCaterpillar &&
            WeakThis->CanCommand(ReceivingOwner.Get()) && WeakPlayer.IsValid() && WeakThis->GetOwnerPawn()==WeakPlayer.Get();
    };
    FName Reason;
    const bool bReceived=Held?DeliveryHandheldTransfer::TryTransfer(Npc,Player,Worm,Reason,Current):
        DeliveryHandheldTransfer::TryPickupWorldItem(Player,Worm,Reason,Current);
    if (!WeakThis.IsValid() || OwnerPlayerState!=ReceivingOwner.Get()) return bReceived;
    bGivingCaterpillar=false;
    if (!bReceived) return false;
    bCaterpillarGiven=true; bCaterpillarOffered=false;
    RecordMemory(FGuid::NewGuid(),TEXT("XiaolvGiven"),TEXT("主人亲手接过了小绿；是否放进外卖还由主人决定，我没有替他投放或获得奖励"),0);
    GetOwner()->ForceNetUpdate();
    return true;
}
