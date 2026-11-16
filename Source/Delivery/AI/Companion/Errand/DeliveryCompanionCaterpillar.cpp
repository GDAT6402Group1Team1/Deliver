#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "Character/DeliveryCharacter.h"
#include "Inventory/Items/DeliveryCaterpillarItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Inventory/Items/DeliveryPackageItem.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Interaction/DeliveryInteractionRegistry.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "World/Location/DeliveryLocationRegistry.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"

// ── 小绿 ──
// 小绿的递出和交接：二狗提出要给，主人长按 E 时由 AcceptCaterpillar 交到主人背包

/// @brief 在 Pawn 周围 Radius 内找一只登记在交互注册表里、看得见的院子小绿
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

/// @brief 院子里发现小绿的机会还没过期、还没提议过，且主人就在二狗身边
bool UDeliveryCompanionComponent::HasCaterpillarOpportunity() const
{
    const auto* Pawn=GetOwnerPawn();
    return CaterpillarOpportunityId.IsValid() && !bCaterpillarOffered && GetWorld() &&
        GetWorld()->GetTimeSeconds()<CaterpillarOpportunityExpiresAt && CanOfferCaterpillar(OwnerPlayerState) &&
        Pawn && FVector::DistSquared(Pawn->GetActorLocation(),GetOwner()->GetActorLocation())<=FMath::Square(900.f);
}

/// @brief 还没提议过时：有请求人就按请求人检查，没有请求人只认当前院子里的机会
bool UDeliveryCompanionComponent::CanRequestCaterpillarOffer(APlayerState* Requester) const
{
    if (bCaterpillarOffered) return false;
    // 没有玩家请求者时只允许来自当前院子机会的提议，访客仍必须按自己的真实身份拒绝
    return Requester?CanOfferCaterpillar(Requester):HasCaterpillarOpportunity();
}

/// @brief 主人带着初遇那一单走到院子附近时生成一次提议小绿的机会，离开就作废
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

/// @brief 请求人是主人，且手上是初遇那一单的包裹、离收件人还有一段距离时可以提议
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

/// @brief 已经提议过、主人站在 180 cm 内、背包有空位且二狗手上或院子里有小绿时可以接过
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

/// @brief 只由主人实际交互调用，true 表示原背包接口已收到真实道具
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
