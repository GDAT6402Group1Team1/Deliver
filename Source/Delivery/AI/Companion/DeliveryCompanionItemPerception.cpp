#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Interaction/DeliveryInteractionRegistry.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Components/StaticMeshComponent.h"
#include "AIController.h"
#include "AIHelpers.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISense_Sight.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"

namespace
{
    bool GetItemSight(const ADeliveryCompanionCharacter* Npc,const UDeliveryCompanionTuning& Tuning,
        FVector& Eye,FVector& Direction,const UAISenseConfig_Sight*& Sight,float& Radius)
    {
        if (!IsValid(Npc) || !Npc->HasAuthority() || !Npc->GetWorld() || Npc->IsActorBeingDestroyed() ||
            Npc->IsHidden() || Npc->IsKnockedDown()) return false;
        const auto* Controller=Cast<AAIController>(Npc->GetController());
        const auto* Perception=Controller?Controller->GetAIPerceptionComponent():nullptr;
        Sight=Perception?Perception->GetSenseConfig<UAISenseConfig_Sight>():nullptr;
        // 物品只借用现有视觉配置；停用感知后不能继续提供新的实见候选
        if (!Sight || Controller->GetWorld()!=Npc->GetWorld() || Perception->GetWorld()!=Npc->GetWorld() ||
            !Perception->IsRegistered() || !Perception->IsSenseEnabled(UAISense_Sight::StaticClass()) ||
            !FMath::IsFinite(Tuning.ItemSightRadius) || Tuning.ItemSightRadius<100 || Tuning.ItemSightRadius>3000 ||
            !FMath::IsFinite(Sight->SightRadius) || Sight->SightRadius<=0 ||
            !FMath::IsFinite(Sight->PeripheralVisionAngleDegrees) ||
            !FMath::IsFinite(Sight->PointOfViewBackwardOffset) || Sight->PointOfViewBackwardOffset<0 || Sight->PointOfViewBackwardOffset>3000 ||
            !FMath::IsFinite(Sight->NearClippingRadius) || Sight->NearClippingRadius<0 || Sight->NearClippingRadius>6000) return false;
        Radius=FMath::Min(Tuning.ItemSightRadius,Sight->SightRadius);
        Perception->GetLocationAndDirection(Eye,Direction);
        return !Eye.ContainsNaN() && !Direction.ContainsNaN() && Direction.Normalize();
    }

    FString PublicItemName(const ADeliveryHandheldItem* Item)
    {
        FString Name=IsValid(Item) && Item->GetItemComponent()?Item->GetItemComponent()->DisplayName.ToString():FString();
        for (TCHAR& Character:Name) if (Character<32) Character=TEXT(' ');
        return Name.TrimStartAndEnd().Left(64);
    }
}

bool UDeliveryCompanionComponent::CanSeeWorldItem(const ADeliveryHandheldItem* Item,FVector& ObservedPoint) const
{
    const auto* Npc=GetCompanion();
    const auto* Mesh=IsValid(Item)?Item->GetItemMesh():nullptr;
    const auto* Interaction=IsValid(Item)?UDeliveryInteractableComponent::FindOn(Item,EDeliveryInteractionKey::PickupE):nullptr;
    if (!IsValid(Npc) || !GetWorld() || Npc->GetWorld()!=GetWorld() || !DeliveryHandheldTransfer::IsFetchableItem(Item) ||
        Item->IsActorBeingDestroyed() || Item->GetWorld()!=GetWorld() || Item->GetOwner() || Item->GetAttachParentActor() || Item->IsHidden() ||
        !Item->GetActorEnableCollision() || !Mesh || !Mesh->GetStaticMesh() || Item->GetRootComponent()!=Mesh ||
        !Mesh->IsSimulatingPhysics() || !Interaction || !Interaction->bInteractEnabled || PublicItemName(Item).IsEmpty()) return false;
    FVector Eye=FVector::ZeroVector,Direction=FVector::ZeroVector;
    const UAISenseConfig_Sight* Sight=nullptr;
    float Radius=0;
    if (!GetItemSight(Npc,GetTuning(),Eye,Direction,Sight,Radius)) return false;
    const FVector Center=Mesh->Bounds.Origin;
    if (Center.ContainsNaN() || FVector::DistSquared(Eye,Center)>FMath::Square(Radius)) return false;
    const float PeripheralCos=FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Sight->PeripheralVisionAngleDegrees,0.f,180.f)));
    if (!FAISystem::CheckIsTargetInSightCone(Eye,Direction,PeripheralCos,Sight->PointOfViewBackwardOffset,
        FMath::Square(Sight->NearClippingRadius),FMath::Square(Radius+Sight->PointOfViewBackwardOffset),Center)) return false;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionItemSight),false,Npc);
    FCollisionObjectQueryParams Objects;
    for (const auto Channel:{ECC_WorldStatic,ECC_WorldDynamic,ECC_Pawn,ECC_Vehicle,ECC_PhysicsBody}) Objects.AddObjectTypesToQuery(Channel);
    FHitResult Hit;
    if (GetWorld()->LineTraceSingleByObjectType(Hit,Eye,Center,Objects,Query) && Hit.GetActor()!=Item) return false;
    ObservedPoint=Interaction->GetInteractionPoint(Npc->GetActorLocation());
    return !ObservedPoint.ContainsNaN();
}

void UDeliveryCompanionComponent::RefreshItemObservations()
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority() || !Npc->Agent || Mode==EDeliveryCompanionMode::Unrecruited || !CanCommand(OwnerPlayerState)) return;
    const double Now=GetWorld()->GetTimeSeconds();
    if (Now<NextItemObservationAt) return;
    const float Interval=GetTuning().ItemObservationInterval;
    NextItemObservationAt=Now+(FMath::IsFinite(Interval)?FMath::Clamp(Interval,.2f,2.f):.5f);
    if (Npc->IsKnockedDown()) return;
    TArray<UDeliveryInteractableComponent*> Registered;
    FVector Eye=FVector::ZeroVector,Direction=FVector::ZeroVector;
    const UAISenseConfig_Sight* Sight=nullptr;
    float Radius=0;
    if (GetItemSight(Npc,GetTuning(),Eye,Direction,Sight,Radius))
        if (const auto* Registry=GetWorld()->GetSubsystem<UDeliveryInteractionRegistry>())
            Registry->GetRegisteredInRadius(Eye,Radius,Registered);
    Registered.Sort([Npc](const auto& A,const auto& B)
        { return FVector::DistSquared(A.GetOwner()->GetActorLocation(),Npc->GetActorLocation())<FVector::DistSquared(B.GetOwner()->GetActorLocation(),Npc->GetActorLocation()); });
    TSet<ADeliveryHandheldItem*> Visible;
    for (auto* Interaction:Registered)
    {
        if (Visible.Num()>=FMath::Clamp(GetTuning().MaximumSeenItems,1,8)) break;
        auto* Item=Cast<ADeliveryHandheldItem>(Interaction->GetOwner());
        FVector Point;
        if (!CanSeeWorldItem(Item,Point)) continue;
        Visible.Add(Item);
        FSeenItem* Seen=SeenItems.FindByPredicate([Item](const auto& Entry) { return Entry.Item.Get()==Item; });
        if (!Seen) { Seen=&SeenItems.AddDefaulted_GetRef(); Seen->Item=Item; }
        const bool bNeedFact=!Seen->bVisible || !Npc->Agent->HasObservationEvent(Seen->EventId) || Now-Seen->FactAt>=30;
        Seen->bVisible=true; Seen->Location=Point; Seen->SeenAt=Now;
        if (bNeedFact)
        {
            FDeliveryNpcObservation Fact;
            Fact.EventId=FGuid::NewGuid(); Fact.Kind=TEXT("WorldItemSeen"); Fact.Source=EDeliveryNpcEvidenceSource::Seen;
            Fact.Subject=Item; Fact.bHasObservedLocation=true; Fact.ObservedLocation=Point; Fact.Lifetime=60; Fact.bNeedsReply=false;
            Fact.Description=FString::Printf(TEXT("在这条观察发生时，我看见地上的公共道具“%s”；这条记录不能证明它现在仍在那里，能否取回以本次候选和接近后的检查为准。"),*PublicItemName(Item));
            if (Npc->Agent->ObserveEvent(Fact)) { Seen->EventId=Fact.EventId; Seen->FactAt=Now; }
        }
    }
    for (auto& Seen:SeenItems)
    {
        if (!Seen.bVisible || Visible.Contains(Seen.Item.Get())) continue;
        Seen.bVisible=false;
        if (Seen.Item.IsValid())
        {
            FDeliveryNpcObservation Fact;
            Fact.Kind=TEXT("WorldItemLostSight"); Fact.Subject=Seen.Item.Get(); Fact.Source=EDeliveryNpcEvidenceSource::Seen;
            // 看不见的记录至少覆盖此前实见记录的剩余寿命，避免过期顺序又露出旧的可见描述
            Fact.Lifetime=60; Fact.bNeedsReply=false; Fact.bHasObservedLocation=true; Fact.ObservedLocation=Seen.Location;
            Fact.Description=TEXT("我现在看不到这件道具，只知道它上次被看见的位置，不能确认它仍在那里或已经被谁拿走。");
            Npc->Agent->ObserveEvent(Fact);
        }
    }
    SeenItems.RemoveAll([Now](const auto& Seen) { return !Seen.Item.IsValid() || (!Seen.bVisible && Now-Seen.SeenAt>60); });
    if (SeenItems.Num()>32)
    {
        SeenItems.Sort([](const FSeenItem& A,const FSeenItem& B) { return A.SeenAt>B.SeenAt; });
        SeenItems.SetNum(32);
    }
    auto* Held=GetCarriedItem();
    if (!DeliveryHandheldTransfer::IsOrdinaryItem(Held))
    { ObservedHeldItem.Reset(); HeldItemEventId.Invalidate(); return; }
    if (ObservedHeldItem.Get()!=Held || !Npc->Agent->HasObservationEvent(HeldItemEventId) || Now-HeldItemObservedAt>=30)
    {
        FDeliveryNpcObservation Fact;
        Fact.EventId=FGuid::NewGuid(); Fact.Kind=TEXT("CompanionHoldingItem"); Fact.Subject=Held;
        Fact.Source=EDeliveryNpcEvidenceSource::ActionResult; Fact.Lifetime=60; Fact.bNeedsReply=false;
        Fact.Description=FString::Printf(TEXT("在这条记录确认时，我实际拿着同一件“%s”；是否仍由我持有以本次持物状态为准，这件道具不属于任务包裹。"),*PublicItemName(Held));
        if (Npc->Agent->ObserveEvent(Fact)) { ObservedHeldItem=Held; HeldItemEventId=Fact.EventId; HeldItemObservedAt=Now; }
    }
}

bool UDeliveryCompanionComponent::CanUseObservedItemTarget(const ADeliveryHandheldItem* Item) const
{
    const auto* Npc=GetCompanion();
    if (!IsValid(Npc) || !Npc->HasAuthority() || Npc->IsActorBeingDestroyed() || !GetWorld() ||
        Npc->GetWorld()!=GetWorld() || !DeliveryHandheldTransfer::IsOrdinaryItem(Item) || Item->GetWorld()!=GetWorld()) return false;
    // 自持知识只来自二狗自己的实际槽和实物 Owner，不能借主人的库存放宽视线
    if (GetCarriedItem()==Item && Item->GetOwner()==Npc) return true;
    FVector Seen;
    return CanSeeWorldItem(Item,Seen);
}

bool UDeliveryCompanionComponent::CanArrangeItem(EDeliveryNpcAction Action,ADeliveryHandheldItem* Item,APlayerState* Requester) const
{
    const auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority() || !CanCommand(Requester) || Npc->IsKnockedDown() || bChangingHomeArrangement ||
        HomeState.CarriedProp || HasActiveHomeArrangement() || PendingHomeArrangementId.IsValid() ||
        !DeliveryHandheldTransfer::IsOrdinaryItem(Item) || Item->GetWorld()!=GetWorld()) return false;
    if (Action==EDeliveryNpcAction::ReturnItem) return GetCarriedItem()==Item;
    FVector Point;
    return Action==EDeliveryNpcAction::FetchItem && !GetCarriedItem() && CanSeeWorldItem(Item,Point) &&
        Item->GetItemComponent()->CanTransferTo(Npc);
}

void UDeliveryCompanionComponent::ExtendItemDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const
{
    Request.RequestedCapabilityVersion=7;
    const auto* Npc=GetCompanion();
    if (!Npc || !Npc->Agent) return;
    auto* Held=GetCarriedItem();
    if (DeliveryHandheldTransfer::IsOrdinaryItem(Held))
    {
        Request.CompanionHeldItemId=Npc->Agent->FindKnownSubjectId(Held);
        Request.CompanionHeldItemName=PublicItemName(Held);
        if (!Request.CompanionHeldItemId.IsValid()) Request.CompanionHeldItemName.Reset();
    }
    if (!Request.bPlayerRequest || !CanCommand(Requester)) return;
    const auto Add=[&](ADeliveryHandheldItem* Item,const FGuid& EvidenceId,const FVector& Point,double SeenAt,bool bReturning)
    {
        const auto Action=bReturning?EDeliveryNpcAction::ReturnItem:EDeliveryNpcAction::FetchItem;
        if (!CanArrangeItem(Action,Item,Requester)) return;
        const FGuid SubjectId=Npc->Agent->FindKnownSubjectId(Item);
        if (!SubjectId.IsValid() || !Request.Subjects.ContainsByPredicate([&](const auto& Subject) { return Subject.SubjectId==SubjectId; }) ||
            !Request.Evidence.ContainsByPredicate([&](const auto& Fact) { return Fact.EventId==EvidenceId && Fact.SubjectId==SubjectId; })) return;
        FDeliveryNpcCandidate Candidate; Candidate.Action=Action; Candidate.TargetId=SubjectId; Candidate.EvidenceIds.Add(EvidenceId);
        Candidate.ItemChoice=MakeShared<FDeliveryCompanionItemChoice>();
        auto& Choice=*Candidate.ItemChoice;
        Choice.Item=Item; Choice.SubjectId=SubjectId; Choice.EvidenceId=EvidenceId; Choice.ObservedLocation=Point;
        Choice.ObservedAt=SeenAt; Choice.bReturnItem=bReturning;
        Request.AllowedActions.AddUnique(Action); Request.Candidates.Add(MoveTemp(Candidate));
    };
    if (DeliveryHandheldTransfer::IsOrdinaryItem(Held)) Add(Held,HeldItemEventId,FVector::ZeroVector,HeldItemObservedAt,true);
    else for (const auto& Seen:SeenItems) if (Seen.bVisible) Add(Seen.Item.Get(),Seen.EventId,Seen.Location,Seen.SeenAt,false);
}

bool UDeliveryCompanionComponent::CanApplyItemChoice(const FDeliveryCompanionItemChoice& Choice,APlayerState* Requester,int32 ExpectedRevision) const
{
    const auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority() || ExpectedRevision!=CommandRevision || !CanCommand(Requester) ||
        Choice.Requester.Get()!=Requester || !Choice.CandidateId.IsValid() || !Choice.RequestId.IsValid() ||
        !Choice.SubjectId.IsValid() || !Choice.EvidenceId.IsValid() || !Choice.Item.IsValid() ||
        Npc->Agent->FindKnownSubjectId(Choice.Item.Get())!=Choice.SubjectId || !Npc->Agent->HasObservationEvent(Choice.EvidenceId)) return false;
    if (!CanArrangeItem(Choice.bReturnItem?EDeliveryNpcAction::ReturnItem:EDeliveryNpcAction::FetchItem,Choice.Item.Get(),Requester)) return false;
    return Choice.bReturnItem || (!Choice.ObservedLocation.ContainsNaN() && FMath::IsFinite(Choice.ObservedAt) &&
        Choice.ObservedAt>=0 && Choice.ObservedAt<=GetWorld()->GetTimeSeconds() && GetWorld()->GetTimeSeconds()-Choice.ObservedAt<=60);
}
