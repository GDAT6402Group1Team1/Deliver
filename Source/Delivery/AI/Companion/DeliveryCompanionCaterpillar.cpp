#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "Character/DeliveryCharacter.h"
#include "Inventory/DeliveryCaterpillarItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "World/DeliveryLocationRegistry.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"

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
    Opportunity.Description=TEXT("我和主人到了第一单收件地点附近，外卖还没交付也没加毛毛虫；小绿还在我这里，可以按当前情境决定是否向主人提出恶作剧建议");
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
    return Npc && CanCommand(Requester) && !bCaterpillarGiven && !bGivingCaterpillar && !Npc->IsKnockedDown() &&
        !GetTuning().CaterpillarItemClass.IsNull() && Task && Task->TaskId==Npc->EncounterTaskId &&
        Manager && Manager->GetTaskStatus(Task)==EDeliveryTaskStatus::InProgress && Package && !Package->ContainsCaterpillar() &&
        Registry && Registry->ResolveLocation(Task->DeliveryLocationId,Destination) &&
        FVector::DistSquared(Npc->GetActorLocation(),Destination)<=FMath::Square(900.f);
}

bool UDeliveryCompanionComponent::CanAcceptCaterpillar(APawn* Interactor) const
{
    const auto* Player=Cast<ADeliveryCharacter>(Interactor);
    if (!Player || !bCaterpillarOffered || !CanOfferCaterpillar(Player->GetTaskPlayerState()) ||
        !Player->CanHandleTaskPackage() || Player->GetWorld()!=GetWorld() || !Player->GetInventoryComponent()->HasBackpackSpace() ||
        FVector::DistSquared(Player->GetActorLocation(),GetOwner()->GetActorLocation())>FMath::Square(180.f)) return false;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionGiveCaterpillar),false,GetOwner());
    Params.AddIgnoredActor(Player);
    FHitResult Hit;
    return !GetWorld()->LineTraceSingleByChannel(Hit,GetOwner()->GetActorLocation(),Player->GetActorLocation(),ECC_Visibility,Params);
}

bool UDeliveryCompanionComponent::AcceptCaterpillar(APawn* Interactor)
{
    if (!GetOwner()->HasAuthority() || !CanAcceptCaterpillar(Interactor) || !EnsureOnFoot()) return false;
    auto* Player=CastChecked<ADeliveryCharacter>(Interactor);
    UClass* ItemClass=GetTuning().CaterpillarItemClass.LoadSynchronous();
    if (!ItemClass || !ItemClass->IsChildOf(ADeliveryCaterpillarItem::StaticClass())) return false;
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Item=GetWorld()->SpawnActor<ADeliveryCaterpillarItem>(ItemClass,Player->GetActorLocation(),FRotator::ZeroRotator,Spawn);
    if (!Item) return false;
    bGivingCaterpillar=true;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<APlayerState> ReceivingOwner=OwnerPlayerState;
    const bool bReceived=Player->GetInventoryComponent()->TryPickup(Item);
    if (!WeakThis.IsValid())
    {
        if (!bReceived && IsValid(Item)) Item->Destroy();
        return bReceived;
    }
    if (OwnerPlayerState!=ReceivingOwner.Get())
    {
        if (!bReceived && IsValid(Item)) Item->Destroy();
        return bReceived;
    }
    bGivingCaterpillar=false;
    if (!bReceived)
    {
        if (IsValid(Item)) Item->Destroy();
        return false;
    }
    bCaterpillarGiven=true; bCaterpillarOffered=false;
    RecordMemory(FGuid::NewGuid(),TEXT("XiaolvGiven"),TEXT("主人亲手接过了小绿；是否放进外卖还由主人决定，我没有替他投放或获得奖励"),0);
    GetOwner()->ForceNetUpdate();
    return true;
}
