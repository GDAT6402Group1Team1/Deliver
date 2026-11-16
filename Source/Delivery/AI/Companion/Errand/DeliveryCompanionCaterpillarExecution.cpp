#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Characters/DeliveryNpcAIController.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionTuning.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Animation/AnimSequence.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "Inventory/Items/DeliveryCaterpillarItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Inventory/Items/DeliveryPackageItem.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/World/DeliveryItemComponent.h"

// ── 小绿执行 ──
// 走过去拾取小绿、把小绿放进包裹的执行过程

/// @brief 主人同意后检查能否捡起小绿（CollectCaterpillar）或把它放进包裹（InsertCaterpillar）
bool UDeliveryCompanionComponent::CanArrangeCaterpillar(EDeliveryNpcAction Action,ADeliveryHandheldItem* Item,
    APlayerState* Requester,bool bSearchLead) const
{
    auto* Worm=Cast<ADeliveryCaterpillarItem>(Item);
    const auto* Npc=GetCompanion();
    if ((Action!=EDeliveryNpcAction::CollectCaterpillar && Action!=EDeliveryNpcAction::InsertCaterpillar) ||
        !IsValid(Npc) || !Npc->HasAuthority() || Npc->IsKnockedDown() || !CanCommand(Requester) || bGivingCaterpillar ||
        !IsValid(Worm) || Worm->IsActorBeingDestroyed() || Worm->GetWorld()!=GetWorld() || bChangingHomeArrangement ||
        HomeState.CarriedProp || HasActiveHomeArrangement() || PendingHomeArrangementId.IsValid() ||
        Worm->GetItemComponent()->IsTransferInProgress()) return false;
    const auto* Held=GetCarriedItem();
    const bool bCompanionHoldsPackage=Action==EDeliveryNpcAction::InsertCaterpillar && GetCarriedTaskPackage()!=nullptr;
    if (Held && !bCompanionHoldsPackage)
    {
        if (Action!=EDeliveryNpcAction::InsertCaterpillar || Held!=Worm || Worm->GetOwner()!=Npc) return false;
    }
    else if (Worm->GetOwner())
    {
        if (bCompanionHoldsPackage || Action!=EDeliveryNpcAction::InsertCaterpillar || !CanTakeOwnerItem(Worm)) return false;
    }
    else
    {
        if (bCompanionHoldsPackage && (!Worm->bUseNpcGroundPickupPose || !GetTuning().GroundPickupAnimation)) return false;
        FVector Point;
        if (!CanSeeWorldItem(Worm,Point) && (!bSearchLead ||
            FindReachableYardCaterpillar(GetOwnerPawn(),DeliveryCompanionItemSearch::Radius)!=Worm)) return false;
    }
    if (Action==EDeliveryNpcAction::CollectCaterpillar) return true;
    return IsCurrentPrankPackage(GetCommandPackage(),Requester,bCompanionHoldsPackage);
}

/// @brief Package 是初遇那一单的原包裹，且包裹此刻能接收二狗放进来的小绿
bool UDeliveryCompanionComponent::IsCurrentPrankPackage(const ADeliveryPackageItem* Package,APlayerState* Requester,
    bool bCompanionHoldsPackage) const
{
    const auto* Npc=GetCompanion();
    if (!IsValid(Npc) || !IsValid(Package) || Package!=GetCommandPackage() || !CanCommand(Requester)) return false;
    const auto* Task=Package->GetDeliveryItemComponent()->OwningTask.Get();
    return IsValid(Task) && Task->TaskId==Npc->EncounterTaskId &&
        Package->CanAcceptCompanionCaterpillar(Npc,Requester,false,bCompanionHoldsPackage);
}

/// @brief 在主人附近找小绿；bSeen 表示二狗能直接看见，看不见时 Point 取小绿的模型中心
ADeliveryCaterpillarItem* UDeliveryCompanionComponent::FindCaterpillarForOwnerRequest(FVector& Point,bool& bSeen) const
{
    constexpr float Radius=DeliveryCompanionItemSearch::Radius;
    auto* Worm=FindReachableYardCaterpillar(GetOwnerPawn(),Radius);
    if (Worm)
    {
        bSeen=CanSeeWorldItem(Worm,Point);
        if (!bSeen) Point=Worm->GetItemMesh()->Bounds.Origin;
        return Worm;
    }
    Worm=FindReachableYardCaterpillar(GetCompanion(),Radius);
    bSeen=Worm && CanSeeWorldItem(Worm,Point);
    return bSeen?Worm:nullptr;
}

/// @brief 主人说话时立即检查刚看见的小绿，能放进包裹就记下这次看见
void UDeliveryCompanionComponent::RefreshCaterpillarObservationForOwnerRequest(APlayerState* Requester)
{
    if (!GetCarriedTaskPackage() || !CanCommand(Requester)) return;
    FVector Point;
    bool bSeen=false;
    auto* Worm=FindCaterpillarForOwnerRequest(Point,bSeen);
    if (bSeen && CanArrangeCaterpillar(EDeliveryNpcAction::InsertCaterpillar,Worm,Requester,true))
    {
        // 当前主人说话时可以确认刚进入视野的小绿，不等下一次通用感知扫描
        RecordVisibleWorldItem(Worm,Point,GetWorld()->GetTimeSeconds());
    }
}

/// @brief 主人这次说话时把捡小绿、放小绿进包裹的候选写进模型请求
void UDeliveryCompanionComponent::AddCaterpillarItemCandidates(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const
{
    auto* Npc=GetCompanion();
    const auto* Pawn=GetOwnerPawn();
    if (!Npc || !Npc->Agent || !IsValid(Pawn) || !CanCommand(Requester) || Request.RequesterPlayerState.Get()!=Requester ||
        !Request.bPlayerRequest || !Request.bPendingPlayerMessage || !Request.PlayerMessageId.IsValid() ||
        Request.Candidates.Num()>=63) return;
    auto* Held=GetCarriedItem();
    auto* Worm=Cast<ADeliveryCaterpillarItem>(Held);
    const bool bCompanionHoldsPackage=GetCarriedTaskPackage()!=nullptr;
    if (Held && !Worm && !bCompanionHoldsPackage) return;
    const bool bWormInHand=Worm!=nullptr;
    FVector Point=Npc->GetActorLocation();
    bool bSeen=false;
    if (!Worm) Worm=FindCaterpillarForOwnerRequest(Point,bSeen);
    const bool bCanCollect=CanArrangeCaterpillar(EDeliveryNpcAction::CollectCaterpillar,Worm,Requester,true);
    const bool bCanInsert=CanArrangeCaterpillar(EDeliveryNpcAction::InsertCaterpillar,Worm,Requester,true);
    if ((!bCanCollect && !bCanInsert) || Point.ContainsNaN()) return;
    FGuid SubjectId=Npc->Agent->FindKnownSubjectId(Worm);
    if (!SubjectId.IsValid()) SubjectId=FGuid::NewGuid();
    const auto* ExistingSubject=Request.Subjects.FindByPredicate([SubjectId](const FDeliveryNpcSubject& Subject)
        { return Subject.SubjectId==SubjectId; });
    if (!ExistingSubject && Request.Subjects.Num()>=65) return;
    const double Now=GetWorld()->GetTimeSeconds();

    // 同一只虫仍实际在手中时复用请求已有的持物事实，满额时也不丢投放候选
    const auto* ExistingFact=bWormInHand && ObservedHeldItem.Get()==Worm && HeldItemEventId.IsValid()?
        Request.Evidence.FindByPredicate([&](const FDeliveryNpcEvidence& Fact)
        {
            return Fact.EventId==HeldItemEventId && Fact.SubjectId==SubjectId && Fact.Kind==TEXT("CompanionHoldingItem") &&
                Fact.Source==EDeliveryNpcEvidenceSource::ActionResult && !Fact.bNeedsReply;
        }):nullptr;
    if (bSeen)
    {
        const auto* Seen=SeenItems.FindByPredicate([Worm](const auto& Entry)
            { return Entry.Item.Get()==Worm && Entry.bVisible && Entry.EventId.IsValid(); });
        const int32 Latest=Request.Evidence.FindLastByPredicate([SubjectId](const FDeliveryNpcEvidence& Fact)
        {
            return Fact.SubjectId==SubjectId && (Fact.Kind==TEXT("WorldItemSeen") || Fact.Kind==TEXT("WorldItemLostSight"));
        });
        if (Seen && Latest!=INDEX_NONE)
        {
            const auto& Fact=Request.Evidence[Latest];
            if (Fact.EventId==Seen->EventId && Fact.Kind==TEXT("WorldItemSeen") &&
                Fact.Source==EDeliveryNpcEvidenceSource::Seen && !Fact.bNeedsReply)
                ExistingFact=&Fact;
        }
    }
    if (ExistingFact && (ExistingFact->ObservedAt>Now || Now-ExistingFact->ObservedAt>=DeliveryCompanionItemSearch::LeadLifetime*.5 ||
        ExistingFact->ExpiresAt<=Now)) ExistingFact=nullptr;
    if (!ExistingFact && !MakeRoomForItemSearchEvidence(Request,bCompanionHoldsPackage || bWormInHand)) return;
    if (!ExistingSubject)
    {
        const int32 Index=Request.Subjects.Add({SubjectId,TEXT("小绿（毛毛虫）"),TEXT("other")});
        ExistingSubject=&Request.Subjects[Index];
    }
    if (bWormInHand)
    {
        Request.CompanionHeldItemId=SubjectId;
        Request.CompanionHeldItemName=ExistingSubject->Name;
    }
    const FGuid EvidenceId=ExistingFact?ExistingFact->EventId:FGuid::NewGuid();
    const double ObservedAt=ExistingFact?ExistingFact->ObservedAt:Now;
    const FVector ObservedPoint=ExistingFact && ExistingFact->bHasObservedLocation?ExistingFact->ObservedLocation:Point;
    if (!ExistingFact)
    {
        FDeliveryNpcEvidence Fact;
        Fact.EventId=EvidenceId;
        Fact.SubjectId=SubjectId;
        Fact.bNeedsReply=false;
        Fact.ObservedAt=Now;
        Fact.ExpiresAt=Now+DeliveryCompanionItemSearch::LeadLifetime;
        Fact.Kind=bWormInHand?FName(TEXT("CompanionHoldingItem")):bSeen?FName(TEXT("WorldItemSeen")):DeliveryCompanionItemSearch::EvidenceKind;
        Fact.Source=bSeen?EDeliveryNpcEvidenceSource::Seen:EDeliveryNpcEvidenceSource::ActionResult;
        Fact.Description=bWormInHand?TEXT("我现在拿着同一只小绿"):
            bSeen?TEXT("我看见了地上的小绿"):TEXT("主人附近有小绿的线索，我要到现场确认");
        Request.Evidence.Add(MoveTemp(Fact));
    }
    const auto Add=[&](EDeliveryNpcAction Action,ADeliveryPackageItem* Package)
    {
        FDeliveryNpcCandidate Candidate;
        Candidate.Action=Action;
        Candidate.TargetId=SubjectId;
        Candidate.EvidenceIds.Add(EvidenceId);
        Candidate.ItemChoice=MakeShared<FDeliveryCompanionItemChoice>();
        auto& Choice=*Candidate.ItemChoice;
        Choice.Action=Action;
        Choice.Item=Worm;
        Choice.UsePackage=Package;
        Choice.Source=bWormInHand?TEXT("CompanionHand"):TEXT("World");
        Choice.bInsertIntoHeldPackage=bCompanionHoldsPackage && Action==EDeliveryNpcAction::InsertCaterpillar;
        Choice.SubjectId=SubjectId;
        Choice.EvidenceId=EvidenceId;
        // 候选复用实见时也沿用这次观察的时间与位置，采纳阶段会用同一份事实校验
        Choice.ObservedLocation=ObservedPoint;
        Choice.ObservedAt=ObservedAt;
        Choice.bSearchLead=true;
        Request.AllowedActions.AddUnique(Action);
        Request.Candidates.Add(MoveTemp(Candidate));
    };
    if (bCanCollect) Add(EDeliveryNpcAction::CollectCaterpillar,nullptr);
    if (bCanInsert) Add(EDeliveryNpcAction::InsertCaterpillar,GetCommandPackage());
}

/// @brief 走到包裹旁并把小绿放进去；返回 true 表示这一步还在进行或已完成
bool UDeliveryCompanionComponent::UpdateCaterpillarInsertion()
{
    auto* Npc=GetCompanion();
    auto* Worm=Cast<ADeliveryCaterpillarItem>(Errand.Item.Get());
    auto* Package=Errand.ItemUsePackage.Get();
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const TWeakObjectPtr<APlayerState> Requester(OwnerPlayerState);
    const TWeakObjectPtr<ADeliveryPackageItem> WeakPackage(Package);
    const int32 Revision=Errand.CommandRevision;
    const auto Current=[WeakThis,WeakNpc,Requester,WeakPackage,Revision]()
    {
        return WeakThis.IsValid() && WeakNpc.IsValid() && !WeakNpc->IsActorBeingDestroyed() &&
            WeakThis->GetOwner()==WeakNpc.Get() && WeakThis->HasActiveErrand() && WeakThis->CommandRevision==Revision &&
            WeakThis->Errand.CommandRevision==Revision && WeakThis->Errand.Action==EDeliveryNpcAction::InsertCaterpillar &&
            WeakThis->CanCommand(Requester.Get()) && WeakPackage.IsValid() && WeakThis->GetCommandPackage()==WeakPackage.Get();
    };
    if (!IsValid(Npc) || !IsValid(Worm) || !Current() || !Package->CanAcceptCompanionCaterpillar(Npc,Requester.Get(),false))
    {
        FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("PrankPackageUnavailableItemPreserved"));
        return false;
    }
    // 越障期间保留原路线，落地后才继续接近或投放
    if (const auto* AI=Cast<ADeliveryNpcAIController>(Npc->GetController()); AI && AI->IsTraversingObstacle(ErrandMoveId))
    {
        LastErrandUpdateAt=0;
        return false;
    }
    if (Errand.GroundPickupServerTime>=0)
    {
        const auto* Pickup=GetTuning().GroundPickupAnimation.Get();
        if (Pickup && GetHomeServerTime()-Errand.GroundPickupServerTime<Pickup->GetPlayLength()) return false;
        StopErrandMovement();
        if (!Current()) return false;
    }
    const double Now=GetWorld()->GetTimeSeconds();
    const float Dt=LastErrandUpdateAt>0?FMath::Clamp(float(Now-LastErrandUpdateAt),0.f,1.f):0.f;
    LastErrandUpdateAt=Now;
    ErrandActiveSeconds+=Dt;
    if (ErrandActiveSeconds>=ErrandTimeLimitSeconds)
    {
        FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("ErrandTimedOutItemPreserved"));
        return false;
    }
    if (!EnsureOnFoot() || !Current()) return false;
    if (!Package->CanAcceptCompanionCaterpillar(Npc,Requester.Get(),true))
    {
        auto* Carrier=Package->GetOwner();
        const FVector Goal=Carrier?Carrier->GetActorLocation():Package->GetActorLocation();
        MoveForErrand(Goal,85.f,Carrier?Carrier:Package);
        return false;
    }
    StopErrandMovement();
    if (!Current()) return false;
    const bool bInserted=Package->TryInsertCompanionCaterpillar(Npc,Worm,Requester.Get(),Current);
    if (!Current()) return bInserted;
    if (!bInserted)
    {
        FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("PrankInsertionRejectedItemPreserved"));
        return false;
    }
    RecordMemory(FGuid::NewGuid(),TEXT("XiaolvInserted"),TEXT("按主人安排把同一只小绿放进本单原包裹，当前仍需交付"),0);
    if (Current()) FinishErrand(EDeliveryNpcActionStatus::Succeeded,TEXT("CaterpillarInserted"));
    return true;
}
