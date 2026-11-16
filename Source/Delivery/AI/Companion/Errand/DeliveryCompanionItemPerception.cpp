#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionTuning.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Inventory/Items/DeliveryHandheldItem.h"
#include "Inventory/Items/DeliveryPackageItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Interaction/DeliveryInteractionRegistry.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "AIController.h"
#include "AIHelpers.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISense_Sight.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"

bool UDeliveryCompanionComponent::MakeRoomForItemSearchEvidence(FDeliveryNpcDecisionRequest& Request,bool bPrioritizeCurrentItem)
{
    constexpr int32 MaxEvidence=32;
    if (Request.Evidence.Num()<MaxEvidence) return true;
    const auto IsUnreferenced=[&Request](const FDeliveryNpcEvidence& Fact)
    {
        return Fact.EventId!=Request.PlayerMessageId &&
            !Request.Candidates.ContainsByPredicate([&Fact](const FDeliveryNpcCandidate& Candidate)
                { return Candidate.EvidenceIds.Contains(Fact.EventId); });
    };
    int32 Index=Request.Evidence.IndexOfByPredicate([&IsUnreferenced](const FDeliveryNpcEvidence& Fact)
    {
        const FName Kind=Fact.Kind;
        return (Kind==TEXT("WorldItemLostSight") || Kind==TEXT("WorldItemSeen") || Kind==TEXT("LastSeen") ||
            Kind==TEXT("LostSight")) && !Fact.bNeedsReply && IsUnreferenced(Fact);
    });
    if (Index==INDEX_NONE && bPrioritizeCurrentItem)
    {
        // 只从本次快照省略已被后续同类结果覆盖的记录，原事件留在角色记忆中
        for (int32 Older=0;Older<Request.Evidence.Num()-1;++Older)
        {
            const auto& Fact=Request.Evidence[Older];
            const FName Kind=Fact.Kind;
            if (Fact.Source!=EDeliveryNpcEvidenceSource::ActionResult || !IsUnreferenced(Fact) ||
                (Kind!=TEXT("ActionFinished") && Kind!=TEXT("CompanionErrandFinished") &&
                    Kind!=TEXT("CompanionErrandPaused") && Kind!=TEXT("CompanionItemErrandFinished"))) continue;
            if (Request.Evidence.FindLastByPredicate([Kind](const auto& Later)
                { return Later.Kind==Kind && Later.Source==EDeliveryNpcEvidenceSource::ActionResult; })<=Older) continue;
            Index=Older;
            break;
        }
    }
    if (Index==INDEX_NONE) return false;
    Request.Evidence.RemoveAt(Index);
    return true;
}

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

    const UDeliveryInteractableComponent* GetGroundItemInteraction(const ADeliveryHandheldItem* Item,const UWorld* World)
    {
        const auto* Interaction=DeliveryHandheldTransfer::FindWorldPickupInteraction(Item,World);
        return Interaction && !PublicItemName(Item).IsEmpty()?Interaction:nullptr;
    }
}

bool UDeliveryCompanionComponent::CanSeeWorldItem(const ADeliveryHandheldItem* Item,FVector& ObservedPoint) const
{
    const auto* Npc=GetCompanion();
    if (!IsValid(Npc) || !GetWorld() || Npc->GetWorld()!=GetWorld()) return false;
    const auto* Interaction=GetGroundItemInteraction(Item,GetWorld());
    if (!Interaction) return false;
    const auto* Mesh=Item->GetItemMesh();
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

bool UDeliveryCompanionComponent::CanArrangeOwnerAreaSearch(APlayerState* Requester) const
{
    const auto* Npc=GetCompanion();
    const auto* Pawn=Cast<ADeliveryCharacter>(GetOwnerPawn());
    return IsValid(Npc) && Npc->HasAuthority() && !Npc->IsActorBeingDestroyed() && !Npc->IsKnockedDown() &&
        CanCommand(Requester) && IsValid(Pawn) && !Pawn->IsActorBeingDestroyed() && Pawn->GetWorld()==GetWorld() &&
        Pawn->GetTaskPlayerState()==Requester && !Pawn->IsRidingVehicle() && !bChangingHomeArrangement &&
        !HomeState.CarriedProp && !HasActiveHomeArrangement() && !PendingHomeArrangementId.IsValid() && !GetCarriedItem() &&
        FVector::DistSquared(Npc->GetActorLocation(),Pawn->GetActorLocation())<=FMath::Square(1600.);
}

void UDeliveryCompanionComponent::AddOwnerAreaSearchCandidate(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const
{
    if (!Request.bPlayerRequest || !Request.bPendingPlayerMessage || !Request.bRequesterIsCompanionOwner ||
        !Request.PlayerMessageId.IsValid() || Request.RequesterPlayerState.Get()!=Requester || !CanArrangeOwnerAreaSearch(Requester)) return;
    const auto* Pawn=CastChecked<ADeliveryCharacter>(GetOwnerPawn());
    const auto* Speech=Request.Evidence.FindByPredicate([&Request](const FDeliveryNpcEvidence& Fact)
    {
        return Fact.EventId==Request.PlayerMessageId && Fact.Kind==TEXT("PlayerSpeech") && Fact.Source==EDeliveryNpcEvidenceSource::Heard;
    });
    if (!Speech || GetCompanion()->Agent->FindKnownSubjectId(Pawn)!=Speech->SubjectId) return;
    const FVector Feet=Pawn->GetActorLocation()-FVector(0,0,Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
    if (Feet.ContainsNaN()) return;
    FDeliveryNpcCandidate Candidate;
    Candidate.Action=EDeliveryNpcAction::SearchNearOwner;
    Candidate.EvidenceIds.Add(Speech->EventId);
    Candidate.ItemChoice=MakeShared<FDeliveryCompanionItemChoice>();
    auto& Choice=*Candidate.ItemChoice;
    Choice.Action=Candidate.Action;
    Choice.EvidenceId=Speech->EventId;
    Choice.SourceCarrier=GetOwnerPawn();
    Choice.ObservedLocation=Feet;
    Choice.ObservedAt=GetWorld()->GetTimeSeconds();
    Request.Candidates.Add(MoveTemp(Candidate));
    Request.AllowedActions.AddUnique(EDeliveryNpcAction::SearchNearOwner);
}

void UDeliveryCompanionComponent::AddNearbyItemSearchCandidates(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const
{
    auto* Npc=GetCompanion();
    const auto* Pawn=GetOwnerPawn();
    const auto* Registry=GetWorld()?GetWorld()->GetSubsystem<UDeliveryInteractionRegistry>():nullptr;
    if (!Npc || !Npc->HasAuthority() || !Npc->Agent || !CanCommand(Requester) || !IsValid(Pawn) || !Registry ||
        !Pawn->IsA<ADeliveryCharacter>() || Npc->IsKnockedDown() || GetCarriedItem() ||
        !Request.RequestId.IsValid() || !Request.bPendingPlayerMessage || !Request.PlayerMessageId.IsValid() ||
        Request.RequesterPlayerState.Get()!=Requester) return;
    const FVector Origin=Pawn->GetActorLocation();
    constexpr float OwnerRadius=DeliveryCompanionItemSearch::Radius;
    constexpr float CompanionRadius=1600.f;
    if (FVector::DistSquared(Npc->GetActorLocation(),Origin)>FMath::Square(CompanionRadius)) return;
    const double Now=GetWorld()->GetTimeSeconds();
    constexpr int32 MaxLeads=4;
    int32 LeadCount=0;
    const auto AddLead=[&](ADeliveryHandheldItem* Item,const FVector& Point,const TCHAR* Situation)
    {
        if (!CanArrangeItem(EDeliveryNpcAction::FetchItem,Item,Requester,true) ||
            Request.Candidates.ContainsByPredicate([Item](const FDeliveryNpcCandidate& Candidate)
                { return Candidate.ItemChoice.IsValid() && Candidate.ItemChoice->Item.Get()==Item; })) return;
        const FString ItemName=PublicItemName(Item);
        if (ItemName.IsEmpty()) return;
        FGuid SubjectId=Npc->Agent->FindKnownSubjectId(Item);
        const bool bNeedsSubject=!SubjectId.IsValid() || !Request.Subjects.ContainsByPredicate([SubjectId](const FDeliveryNpcSubject& Subject)
            { return Subject.SubjectId==SubjectId; });
        if ((bNeedsSubject && Request.Subjects.Num()>=65) || !MakeRoomForItemSearchEvidence(Request)) return;
        if (!SubjectId.IsValid()) SubjectId=FGuid::NewGuid();
        if (bNeedsSubject) Request.Subjects.Add({SubjectId,ItemName,TEXT("other")});

        // 主人附近的线索随本轮请求发送，不登记为角色长期认识的对象或事实
        FDeliveryNpcEvidence Fact;
        Fact.EventId=FGuid::NewGuid(); Fact.Kind=DeliveryCompanionItemSearch::EvidenceKind; Fact.SubjectId=SubjectId;
        Fact.Source=EDeliveryNpcEvidenceSource::ActionResult; Fact.ObservedAt=Now;
        Fact.ExpiresAt=Now+DeliveryCompanionItemSearch::LeadLifetime; Fact.bNeedsReply=false;
        Fact.Description=FString::Printf(TEXT("%s“%s”。线索离主人约 %.1f 米，离我约 %.1f 米，拿取要到场确认。"),Situation,*ItemName,
            FVector::Dist2D(Point,Origin)/100.,FVector::Dist2D(Point,Npc->GetActorLocation())/100.);
        FDeliveryNpcCandidate Candidate;
        Candidate.Action=EDeliveryNpcAction::FetchItem; Candidate.TargetId=SubjectId; Candidate.EvidenceIds.Add(Fact.EventId);
        Candidate.ItemChoice=MakeShared<FDeliveryCompanionItemChoice>();
        auto& Choice=*Candidate.ItemChoice;
        Choice.Item=Item; Choice.SubjectId=SubjectId; Choice.EvidenceId=Fact.EventId;
        Choice.ObservedLocation=Point; Choice.ObservedAt=Now; Choice.bSearchLead=true;
        Request.Evidence.Add(MoveTemp(Fact));
        Request.AllowedActions.AddUnique(Candidate.Action);
        Request.Candidates.Add(MoveTemp(Candidate));
        ++LeadCount;
    };
    TArray<UDeliveryInteractableComponent*> Nearby;
    Registry->GetRegisteredInRadius(Origin,OwnerRadius,Nearby);
    Nearby.Sort([&Origin](const auto& A,const auto& B)
        { return FVector::DistSquared(A.GetOwner()->GetActorLocation(),Origin)<FVector::DistSquared(B.GetOwner()->GetActorLocation(),Origin); });
    FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionNearbyItemLead),false,Pawn);
    Query.AddIgnoredActor(Npc);
    for (const auto* Registered:Nearby)
    {
        if (LeadCount>=MaxLeads) break;
        auto* Item=Cast<ADeliveryHandheldItem>(Registered->GetOwner());
        const auto* Interaction=GetGroundItemInteraction(Item,GetWorld());
        if (Interaction!=Registered || !Interaction || !Item->GetItemComponent()->CanTransferTo(Npc)) continue;
        const FVector Point=Item->GetItemMesh()->Bounds.Origin;
        if (Point.ContainsNaN() || FVector::DistSquared(Origin,Point)>FMath::Square(OwnerRadius)) continue;
        FVector Seen;
        if (CanSeeWorldItem(Item,Seen))
        {
            // 常规观察仍在限频时，刚进入视野且尚无直接候选的目标也能用于本轮
            AddLead(Item,Seen,TEXT("我刚看见了主人附近的"));
            continue;
        }
        FHitResult Hit;
        // 候选只检查主人近处无遮挡的公共物品，墙门另一侧的道具不进入这轮请求
        if (GetWorld()->LineTraceSingleByChannel(Hit,Origin+FVector(0,0,35),Point,ECC_Visibility,Query) && Hit.GetActor()!=Item) continue;
        AddLead(Item,Point,TEXT("主人附近有可去查看的线索，我还没亲眼看见"));
    }
    for (const auto& Seen:SeenItems)
    {
        if (LeadCount>=MaxLeads) break;
        auto* Item=Seen.Item.Get();
        // 只用自己先前保存的坐标，不用被遮挡物品的当前坐标或持有状态
        if (!DeliveryHandheldTransfer::IsOrdinaryItem(Item) ||
            Now-Seen.SeenAt>DeliveryCompanionItemSearch::MemoryLifetime ||
            FVector::DistSquared(Origin,Seen.Location)>FMath::Square(OwnerRadius)) continue;
        AddLead(Item,Seen.Location,TEXT("我不久前在主人附近见过"));
    }
}

void UDeliveryCompanionComponent::RecordVisibleWorldItem(ADeliveryHandheldItem* Item,const FVector& Point,double Now)
{
    FSeenItem* Seen=SeenItems.FindByPredicate([Item](const auto& Entry) { return Entry.Item.Get()==Item; });
    if (!Seen) { Seen=&SeenItems.AddDefaulted_GetRef(); Seen->Item=Item; }
    auto* Agent=GetCompanion()->Agent.Get();
    const bool bNeedFact=!Seen->bVisible || !Agent->HasObservationEvent(Seen->EventId) || Now-Seen->FactAt>=30;
    Seen->bVisible=true; Seen->Location=Point; Seen->SeenAt=Now;
    if (!bNeedFact) return;
    FDeliveryNpcObservation Fact;
    Fact.EventId=FGuid::NewGuid(); Fact.Kind=TEXT("WorldItemSeen"); Fact.Source=EDeliveryNpcEvidenceSource::Seen;
    Fact.Subject=Item; Fact.bHasObservedLocation=true; Fact.ObservedLocation=Point; Fact.Lifetime=60; Fact.bNeedsReply=false;
    Fact.Description=FString::Printf(TEXT("在这条观察发生时，我看见地上的公共道具“%s”；这条记录不能证明它现在仍在那里，能否取回以本次候选和接近后的检查为准。"),*PublicItemName(Item));
    if (Agent->ObserveEvent(Fact)) { Seen->EventId=Fact.EventId; Seen->FactAt=Now; }
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
        if (HasNearbyItemSearch() && Errand.Action==EDeliveryNpcAction::SearchNearOwner && ItemSearchLookStartedAt>=0 &&
            AreaSearchSeenNames.Num()<8 && FVector::DistSquared2D(Point,ItemLastSeenLocation)<=FMath::Square(DeliveryCompanionItemSearch::Radius) &&
            FMath::Abs(Point.Z-ItemLastSeenLocation.Z)<=110.)
            AreaSearchSeenNames.AddUnique(PublicItemName(Item));
        RecordVisibleWorldItem(Item,Point,Now);
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

bool UDeliveryCompanionComponent::CanArrangeItem(EDeliveryNpcAction Action,ADeliveryHandheldItem* Item,APlayerState* Requester,bool bSearchLead) const
{
    if (Action==EDeliveryNpcAction::SearchNearOwner) return !Item && !bSearchLead && CanArrangeOwnerAreaSearch(Requester);
    if (Action==EDeliveryNpcAction::CollectCaterpillar || Action==EDeliveryNpcAction::InsertCaterpillar)
        return CanArrangeCaterpillar(Action,Item,Requester,bSearchLead);
    const auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority() || !CanCommand(Requester) || Npc->IsKnockedDown() || bChangingHomeArrangement ||
        HomeState.CarriedProp || HasActiveHomeArrangement() || PendingHomeArrangementId.IsValid() ||
        !DeliveryHandheldTransfer::IsOrdinaryItem(Item) || Item->IsActorBeingDestroyed() || Item->GetWorld()!=GetWorld()) return false;
    if (Action==EDeliveryNpcAction::ReturnItem) return !bSearchLead && GetCarriedItem()==Item;
    if (Action==EDeliveryNpcAction::TakeItem) return !bSearchLead && CanTakeOwnerItem(Item);
    if (Action!=EDeliveryNpcAction::FetchItem || !DeliveryHandheldTransfer::IsFetchableItem(Item) || GetCarriedItem()) return false;
    // 按线索只能安排搜索，拿取要重新通过实见和物件可用状态检查
    if (bSearchLead) return true;
    FVector Point;
    return CanSeeWorldItem(Item,Point) && Item->GetItemComponent()->CanTransferTo(Npc);
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
    AddOwnerInventoryCandidates(Request,Requester);
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
        Choice.Source=bReturning?TEXT("CompanionHand"):TEXT("World");
        Request.AllowedActions.AddUnique(Action); Request.Candidates.Add(MoveTemp(Candidate));
    };
    if (DeliveryHandheldTransfer::IsOrdinaryItem(Held)) Add(Held,HeldItemEventId,FVector::ZeroVector,HeldItemObservedAt,true);
    else
    {
        for (const auto& Seen:SeenItems) if (Seen.bVisible) Add(Seen.Item.Get(),Seen.EventId,Seen.Location,Seen.SeenAt,false);
        AddNearbyItemSearchCandidates(Request,Requester);
        AddOwnerAreaSearchCandidate(Request,Requester);
    }
    AddCaterpillarItemCandidates(Request,Requester);
}

bool UDeliveryCompanionComponent::CanApplyItemChoice(const FDeliveryCompanionItemChoice& Choice,APlayerState* Requester,int32 ExpectedRevision) const
{
    const auto* Npc=GetCompanion();
    if (!Npc || !Npc->Agent || !Npc->HasAuthority() || ExpectedRevision!=CommandRevision || !CanCommand(Requester) ||
        Choice.Requester.Get()!=Requester || !Choice.CandidateId.IsValid() || !Choice.RequestId.IsValid() ||
        !Choice.EvidenceId.IsValid()) return false;
    if (Choice.GetAction()==EDeliveryNpcAction::SearchNearOwner)
    {
        if (Choice.SubjectId.IsValid() || Choice.Item.IsValid() || Choice.bSearchLead || Choice.bReturnItem ||
            Choice.SourceCarrier.Get()!=GetOwnerPawn() || !CanArrangeOwnerAreaSearch(Requester) ||
            Choice.ObservedLocation.ContainsNaN() || !Npc->Agent->HasObservationEvent(Choice.EvidenceId)) return false;
        const auto* Pawn=CastChecked<ADeliveryCharacter>(GetOwnerPawn());
        const FVector Feet=Pawn->GetActorLocation()-FVector(0,0,Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
        const double Age=GetWorld()->GetTimeSeconds()-Choice.ObservedAt;
        return Age>=0. && Age<DeliveryCompanionItemSearch::LeadLifetime && FMath::Abs(Feet.Z-Choice.ObservedLocation.Z)<=110. &&
            FVector::DistSquared2D(Feet,Choice.ObservedLocation)<=FMath::Square(200.);
    }
    if (!Choice.SubjectId.IsValid() || !Choice.Item.IsValid()) return false;
    if (!CanArrangeItem(Choice.GetAction(),
        Choice.Item.Get(),Requester,Choice.bSearchLead)) return false;
    const auto Action=Choice.GetAction();
    if (Action==EDeliveryNpcAction::CollectCaterpillar || Action==EDeliveryNpcAction::InsertCaterpillar)
    {
        if (Action==EDeliveryNpcAction::InsertCaterpillar &&
            !IsCurrentPrankPackage(Choice.UsePackage.Get(),Requester,Choice.bInsertIntoHeldPackage)) return false;
        if (Choice.bInsertIntoHeldPackage && (Action!=EDeliveryNpcAction::InsertCaterpillar || !Choice.bSearchLead ||
            Choice.Source!=TEXT("World") || Choice.Item->GetOwner())) return false;
        if (Action==EDeliveryNpcAction::InsertCaterpillar && Choice.IsOwnerInventorySource())
            return !Choice.bSearchLead && Choice.SourceCarrier.Get()==GetOwnerItemCarrier() &&
                GetCurrentOwnerItemSource(Choice.Item.Get())==Choice.Source &&
                FMath::IsFinite(Choice.ObservedAt) && Choice.ObservedAt>=0 &&
                GetWorld()->GetTimeSeconds()>=Choice.ObservedAt && GetWorld()->GetTimeSeconds()-Choice.ObservedAt<15.;
        const bool bHeld=GetCarriedItem()==Choice.Item.Get() && Choice.Item->GetOwner()==Npc;
        if ((!bHeld && Choice.Item->GetOwner()) || Choice.Source!=(bHeld?TEXT("CompanionHand"):TEXT("World"))) return false;
    }
    if (Choice.GetAction()==EDeliveryNpcAction::TakeItem)
        return Choice.SourceCarrier.Get()==GetOwnerItemCarrier() &&
            (Choice.Source==TEXT("OwnerHand") || Choice.Source==TEXT("OwnerBackpack")) &&
            FMath::IsFinite(Choice.ObservedAt) && Choice.ObservedAt>=0 &&
            GetWorld()->GetTimeSeconds()>=Choice.ObservedAt && GetWorld()->GetTimeSeconds()-Choice.ObservedAt<15.;
    // 本轮线索已随候选绑定到原目标，旧的真实实见和持物证据仍从角色记忆核对
    if (!Choice.bSearchLead && (Npc->Agent->FindKnownSubjectId(Choice.Item.Get())!=Choice.SubjectId ||
        !Npc->Agent->HasObservationEvent(Choice.EvidenceId))) return false;
    const double Age=GetWorld()->GetTimeSeconds()-Choice.ObservedAt;
    if (Choice.bSearchLead) return !Choice.ObservedLocation.ContainsNaN() && Choice.ObservedAt>=0. &&
        Age>=0. && Age<DeliveryCompanionItemSearch::LeadLifetime;
    return Choice.bReturnItem || (!Choice.ObservedLocation.ContainsNaN() && FMath::IsFinite(Choice.ObservedAt) &&
        Choice.ObservedAt>=0 && Age>=0. && Age<=60.);
}
