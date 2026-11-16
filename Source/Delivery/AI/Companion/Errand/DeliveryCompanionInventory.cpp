#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Character/DeliveryCharacter.h"
#include "Inventory/Items/DeliveryCaterpillarItem.h"
#include "Inventory/Items/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "Engine/World.h"

ADeliveryCharacter* UDeliveryCompanionComponent::GetOwnerItemCarrier() const
{
    auto* Pawn=GetOwnerPawn();
    auto* Bike=Cast<ADeliveryMotorbike>(Pawn);
    auto* Character=Bike?Bike->GetDriver():Cast<ADeliveryCharacter>(Pawn);
    return IsValid(Character) && Character->GetWorld()==GetWorld() &&
        Character->GetTaskPlayerState()==OwnerPlayerState?Character:nullptr;
}

bool UDeliveryCompanionComponent::CanTakeOwnerItem(const ADeliveryHandheldItem* Item) const
{
    const auto* Player=GetOwnerItemCarrier();
    const auto* Npc=GetCompanion();
    return Player && Npc && CanCommand(OwnerPlayerState) && Player->CanHandleHandheldItem() &&
        DeliveryHandheldTransfer::IsOrdinaryItem(Item) && Item->GetWorld()==GetWorld() &&
        Item->GetOwner()==Player && Player->OwnsHandheldItem(Item) && !GetCarriedItem() &&
        !Npc->IsKnockedDown() && !HasActiveHomeArrangement() && !HomeState.CarriedProp;
}

FName UDeliveryCompanionComponent::GetCurrentOwnerItemSource(const ADeliveryHandheldItem* Item) const
{
    if (!CanTakeOwnerItem(Item)) return NAME_None;
    const auto* Inventory=GetOwnerItemCarrier()->GetInventoryComponent();
    return Inventory->GetHeldItem()==Item?FName(TEXT("OwnerHand")):FName(TEXT("OwnerBackpack"));
}

void UDeliveryCompanionComponent::AddOwnerInventoryCandidates(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const
{
    auto* Player=GetOwnerItemCarrier();
    const auto* Inventory=Player?Player->GetInventoryComponent():nullptr;
    if (!Inventory || !CanCommand(Requester) || !Request.bPlayerRequest || !Request.bPendingPlayerMessage ||
        !Request.PlayerMessageId.IsValid() || Request.RequesterPlayerState.Get()!=Requester) return;
    // 只在主人当前指令中枚举实际五个槽，访客与场景闲聊看不到私人物品
    for (int32 Slot=0;Slot<UDeliveryInventoryComponent::SlotCount;++Slot)
    {
        if (Request.Subjects.Num()>=65 || Request.Candidates.Num()>=64) break;
        auto* Item=Inventory->GetItemInSlot(Slot);
        if (!CanTakeOwnerItem(Item)) continue;
        const auto* Data=Item->GetItemComponent();
        FString Name=Data?Data->DisplayName.ToString():FString();
        for (TCHAR& Character:Name) if (Character<32) Character=TEXT(' ');
        Name=Name.TrimStartAndEnd().Left(64);
        if (Name.IsEmpty()) continue;
        if (!MakeRoomForItemSearchEvidence(Request,true)) break;
        const FGuid Subject=FGuid::NewGuid();
        Request.Subjects.Add({Subject,Name,TEXT("other")});
        FDeliveryNpcEvidence Fact;
        Fact.EventId=FGuid::NewGuid(); Fact.SubjectId=Subject; Fact.Kind=TEXT("CompanionOwnerItemAvailable");
        Fact.Source=EDeliveryNpcEvidenceSource::ActionResult;
        Fact.ObservedAt=GetWorld()->GetTimeSeconds(); Fact.ExpiresAt=Fact.ObservedAt+15.; Fact.bNeedsReply=false;
        Fact.Description=FString::Printf(TEXT("主人当前授权查询的%s里有同一件“%s”，可按这轮指令接到我手中；不能把背包当作我已持有或已交还。"),
            Slot==0?TEXT("手中"):TEXT("背包"),*Name);
        FDeliveryNpcCandidate Candidate;
        Candidate.Action=EDeliveryNpcAction::TakeItem; Candidate.TargetId=Subject; Candidate.EvidenceIds.Add(Fact.EventId);
        Candidate.ItemChoice=MakeShared<FDeliveryCompanionItemChoice>();
        auto& Choice=*Candidate.ItemChoice;
        Choice.Action=Candidate.Action; Choice.Item=Item; Choice.SourceCarrier=Player;
        Choice.Source=Slot==0?TEXT("OwnerHand"):TEXT("OwnerBackpack");
        Choice.SubjectId=Subject; Choice.EvidenceId=Fact.EventId; Choice.ObservedAt=Fact.ObservedAt;
        Choice.ObservedLocation=Player->GetActorLocation();
        // 同一私有槽位证据可安排接过去，也可把同一只虫放进本单原件
        if (Item->IsA<ADeliveryCaterpillarItem>() && Request.Candidates.Num()<63 && CanWalkForNearbyItem(Choice) &&
            CanArrangeCaterpillar(EDeliveryNpcAction::InsertCaterpillar,Item,Requester) &&
            IsCurrentPrankPackage(Request.CompanionPackage.Get(),Requester))
        {
            auto Prank=Candidate;
            Prank.Action=EDeliveryNpcAction::InsertCaterpillar;
            Prank.ItemChoice=MakeShared<FDeliveryCompanionItemChoice>(Choice);
            Prank.ItemChoice->Action=Prank.Action;
            Prank.ItemChoice->UsePackage=Request.CompanionPackage;
            Request.AllowedActions.AddUnique(Prank.Action);
            Request.Candidates.Add(MoveTemp(Prank));
        }
        Request.Evidence.Add(MoveTemp(Fact));
        Request.AllowedActions.AddUnique(Candidate.Action); Request.Candidates.Add(MoveTemp(Candidate));
    }
}
