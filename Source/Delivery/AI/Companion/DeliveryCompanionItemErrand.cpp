#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AbilitySystem/Abilities/GA_CompanionItemTransfer.h"
#include "AbilitySystemComponent.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"

namespace
{
    bool ClearItemApproach(ADeliveryCompanionCharacter* Npc,AActor* Target,ADeliveryHandheldItem* Item,const FVector& Goal)
    {
        FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionItemApproach),false,Npc);
        if (Target) Query.AddIgnoredActor(Target);
        if (Item) Query.AddIgnoredActor(Item);
        FCollisionObjectQueryParams Objects;
        for (const auto Channel:{ECC_WorldStatic,ECC_WorldDynamic,ECC_Pawn,ECC_Vehicle,ECC_PhysicsBody}) Objects.AddObjectTypesToQuery(Channel);
        FHitResult Hit;
        return !Npc->GetWorld()->LineTraceSingleByObjectType(Hit,Npc->GetActorLocation(),Goal,Objects,Query);
    }
}

bool UDeliveryCompanionComponent::CanWalkForNearbyItem(const FDeliveryCompanionItemChoice& Choice) const
{
    auto* Npc=GetCompanion();
    auto* Item=Choice.Item.Get();
    auto* Receiver=Cast<ADeliveryCharacter>(GetOwnerPawn());
    if (!IsValid(Npc) || !IsValid(Item) || !IsValid(Receiver) || Receiver->IsActorBeingDestroyed()) return false;
    constexpr double LocalItemDistance=240.;
    const FVector Origin=Npc->GetActorLocation();
    const FVector ReceiverPosition=Receiver->GetActorLocation();
    if (FVector::DistSquared(Origin,ReceiverPosition)>FMath::Square(LocalItemDistance) ||
        !ClearItemApproach(Npc,Receiver,Item,ReceiverPosition)) return false;
    if (Choice.bReturnItem) return true;
    FVector ItemPosition;
    return CanSeeWorldItem(Item,ItemPosition) &&
        FVector::DistSquared(Origin,ItemPosition)<=FMath::Square(LocalItemDistance);
}

bool UDeliveryCompanionComponent::ApplyItemChoice(const FDeliveryCompanionItemChoice& Choice,APlayerState* Requester,int32 ExpectedRevision,
    EDeliveryCompanionTravelMode RequestedTravelMode)
{
    if (!CanApplyItemChoice(Choice,Requester,ExpectedRevision)) return false;
    const bool bWalkLocally=RequestedTravelMode==EDeliveryCompanionTravelMode::Unspecified && CanWalkForNearbyItem(Choice);
    if (RequestedTravelMode==EDeliveryCompanionTravelMode::Unspecified && TravelMode==EDeliveryCompanionTravelMode::Unspecified && !bWalkLocally)
        return false;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryHandheldItem> Target=Choice.Item;
    const bool bHadErrand=HasActiveErrand();
    if (bHadErrand) FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("ReplacedByOwner"));
    if (!WeakThis.IsValid() || !Target.IsValid() || !CanCommand(Requester) ||
        CommandRevision!=ExpectedRevision+(bHadErrand?1:0) || HasActiveErrand()) return false;
    const auto StillCurrent=[&]() { return WeakThis.IsValid() && Target.IsValid() && CanCommand(Requester) &&
        CommandRevision==ExpectedRevision+(bHadErrand?1:0) && !HasActiveErrand(); };
    StopFollowMovement();
    if (!StillCurrent()) return false;
    StopErrandMovement();
    if (!StillCurrent()) return false;
    CancelAssist(TEXT("OwnerChangedArrangement"));
    if (!StillCurrent() || !CanArrangeItem(Choice.bReturnItem?EDeliveryNpcAction::ReturnItem:EDeliveryNpcAction::FetchItem,Target.Get(),Requester)) return false;
    if (bWalkLocally && !CanWalkForNearbyItem(Choice)) return false;
    ++CommandRevision;
    ApplyTravelInstruction(RequestedTravelMode);
    Errand={}; Errand.Action=Choice.bReturnItem?EDeliveryNpcAction::ReturnItem:EDeliveryNpcAction::FetchItem;
    Errand.Status=EDeliveryNpcActionStatus::Running;
    Errand.Phase=Choice.bReturnItem?EDeliveryCompanionErrandPhase::ReturnToOwner:EDeliveryCompanionErrandPhase::GoToPickup;
    Errand.Item=Target; Errand.CommandRevision=CommandRevision;
    Errand.bLocalItemTransfer=bWalkLocally;
    ItemLastSeenLocation=Choice.ObservedLocation; ItemLastSeenAt=Choice.ObservedAt;
    ErrandActiveSeconds=0; ErrandTimeLimitSeconds=GetTuning().ErrandTimeoutSeconds;
    bErrandTravelBudgetAdded=false; bErrandVehicleApproachBudgetAdded=false;
    ErrandInteractionSeconds=0; bHasErrandDestination=false; NextErrandNavigationAt=0;
    NextItemOwnerRetryAt=0; LastErrandPause=NAME_None; SetSuspension(NAME_None);
    bCaterpillarOffered=false; CaterpillarOpportunityId.Invalidate(); bAnnouncedCaterpillarOpportunity=true;
    GetOwner()->ForceNetUpdate();
    return true;
}

bool UDeliveryCompanionComponent::IsCurrentItemUse(FGuid UseId,int32 Revision,ADeliveryHandheldItem* Item) const
{
    const auto* Npc=GetCompanion();
    return UseId.IsValid() && ItemUseId==UseId && HasActiveErrand() && Errand.CommandRevision==Revision &&
        CommandRevision==Revision && Errand.Item.Get()==Item && IsValid(Item) && Item->GetWorld()==GetWorld() &&
        Npc && Npc->HasAuthority() && !Npc->IsActorBeingDestroyed() && CanCommand(OwnerPlayerState) &&
        !Npc->IsKnockedDown() && !IsUsingErrandVehicle() && !HasActiveHomeArrangement() && !HomeState.CarriedProp &&
        !Npc->Agent->ShouldPauseForConversation() &&
        (!Npc->Agent->HasActiveAction() || Npc->Agent->CanContinueCompanionMovement()) &&
        (!Npc->Combat || !Npc->Combat->IsReactingToHit());
}

bool UDeliveryCompanionComponent::ExecuteHandheldUse(const UDeliveryHandheldUseRequest& Request,FName& Reason)
{
    auto* Npc=GetCompanion();
    auto* Item=Request.Item.Get();
    if (Request.GetOuter()!=Npc || !IsCurrentItemUse(Request.UseId,Request.CommandRevision,Item))
    { Reason=TEXT("ItemArrangementChanged"); return false; }
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryHandheldItem> WeakItem(Item);
    const FGuid Id=Request.UseId;
    const int32 Revision=Request.CommandRevision;
    const TFunction<bool()> StillCurrent=[WeakThis,WeakItem,Id,Revision]()
        { return WeakThis.IsValid() && WeakItem.IsValid() && WeakThis->IsCurrentItemUse(Id,Revision,WeakItem.Get()); };
    if (Request.bReturnToOwner)
    {
        if (Errand.Phase!=EDeliveryCompanionErrandPhase::ReturnToOwner || GetCarriedItem()!=Item)
        { Reason=TEXT("HeldItemChanged"); return false; }
        auto* Player=Cast<ADeliveryCharacter>(GetOwnerPawn());
        if (!Player || Player->GetTaskPlayerState()!=OwnerPlayerState)
        { Reason=TEXT("OwnerUnavailable"); return false; }
        return DeliveryHandheldTransfer::TryTransfer(Npc,Player,Item,Reason,StillCurrent);
    }
    FVector Seen;
    if (Errand.Phase!=EDeliveryCompanionErrandPhase::GoToPickup || !CanSeeWorldItem(Item,Seen))
    { Reason=TEXT("ItemNotVisibleAtPickup"); return false; }
    return DeliveryHandheldTransfer::TryPickupWorldItem(Npc,Item,Reason,StillCurrent);
}

void UDeliveryCompanionComponent::FinishHandheldUse(FGuid UseId,bool bSucceeded,FName Reason)
{
    if (!UseId.IsValid() || UseId!=ItemUseId || bItemUseResolved) return;
    bItemUseResolved=true; bItemUseSucceeded=bSucceeded; ItemUseReason=Reason;
}

void UDeliveryCompanionComponent::CancelItemUse()
{
    const bool bWasUsing=ItemUseId.IsValid();
    ItemUseId.Invalidate(); bItemUseResolved=false; bItemUseSucceeded=false; ItemUseReason=NAME_None;
    if (bWasUsing && ItemAbilityHandle.IsValid())
        if (auto* ASC=ItemAbilitySystem.Get()) ASC->CancelAbilityHandle(ItemAbilityHandle);
}

void UDeliveryCompanionComponent::ShutdownItemExecution()
{
    CancelItemUse();
    const auto Handle=ItemAbilityHandle; ItemAbilityHandle={};
    auto* ASC=ItemAbilitySystem.Get(); ItemAbilitySystem.Reset();
    if (ASC && Handle.IsValid()) ASC->ClearAbility(Handle);
    SeenItems.Reset(); HeldItemEventId.Invalidate(); ObservedHeldItem.Reset(); NextItemObservationAt=0;
}

bool UDeliveryCompanionComponent::RunItemTransfer(bool bReturnToOwner,FName& Reason)
{
    auto* Npc=GetCompanion();
    auto* ASC=Npc?Npc->GetAbilitySystemComponent():nullptr;
    if (!Npc || !Npc->HasAuthority() || !ASC || ItemUseId.IsValid()) { Reason=TEXT("ItemAbilityUnavailable"); return false; }
    if (!ItemAbilityHandle.IsValid())
    {
        const auto Class=GetTuning().ItemTransferAbility?GetTuning().ItemTransferAbility.Get():UGA_CompanionItemTransfer::StaticClass();
        ItemAbilitySystem=ASC;
        ItemAbilityHandle=ASC->GiveAbility(FGameplayAbilitySpec(Class,1,INDEX_NONE,this));
    }
    if (!ItemAbilityHandle.IsValid()) { Reason=TEXT("ItemAbilityUnavailable"); return false; }
    auto* Request=NewObject<UDeliveryHandheldUseRequest>(Npc);
    Request->UseId=FGuid::NewGuid(); Request->CommandRevision=Errand.CommandRevision;
    Request->Item=Errand.Item; Request->bReturnToOwner=bReturnToOwner;
    ItemUseId=Request->UseId; bItemUseResolved=false; bItemUseSucceeded=false; ItemUseReason=NAME_None;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const FGuid Id=ItemUseId;
    FGameplayEventData Event; Event.EventTag=TAG_Event_Companion_ItemTransfer; Event.Instigator=Npc;
    Event.Target=Request->Item.Get(); Event.OptionalObject=Request;
    ASC->HandleGameplayEvent(Event.EventTag,&Event);
    if (!WeakThis.IsValid() || ItemUseId!=Id) { Reason=TEXT("ItemArrangementChanged"); return false; }
    const bool bSucceeded=bItemUseResolved && bItemUseSucceeded;
    Reason=bItemUseResolved?ItemUseReason:FName(TEXT("ItemAbilityRejected"));
    CancelItemUse();
    return bSucceeded;
}

void UDeliveryCompanionComponent::UpdateItemErrand()
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority() || !HasActiveErrand() || !DeliveryNpc::IsCompanionItemErrand(Errand.Action)) return;
    const int32 Revision=Errand.CommandRevision;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const auto Current=[WeakThis,WeakNpc,Revision]() { return WeakThis.IsValid() && WeakNpc.IsValid() &&
        !WeakNpc->IsActorBeingDestroyed() && WeakNpc->HasAuthority() && WeakThis->GetOwner()==WeakNpc.Get() &&
        WeakThis->HasActiveErrand() && WeakThis->Errand.CommandRevision==Revision && WeakThis->CommandRevision==Revision; };
    if (!CanCommand(OwnerPlayerState)) { FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("OwnerLost")); return; }
    if (Npc->IsKnockedDown() || Npc->Agent->ShouldPauseForConversation() ||
        (Npc->Agent->HasActiveAction() && !Npc->Agent->CanContinueCompanionMovement()) ||
        (Npc->Combat && Npc->Combat->IsReactingToHit()))
    { StopErrandMovement(); if (Current()) SetSuspension(Npc->IsKnockedDown()?TEXT("Down"):TEXT("OtherBehavior")); return; }
    if (HasActiveHomeArrangement() || HomeState.CarriedProp)
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("HandsOccupiedByHomeUse")); return; }
    auto* Item=Errand.Item.Get();
    if (!DeliveryHandheldTransfer::IsOrdinaryItem(Item) || Item->GetWorld()!=GetWorld())
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("ItemLostOrNoLongerOrdinary")); return; }
    const double Now=GetWorld()->GetTimeSeconds();
    const bool bReturning=Errand.Phase!=EDeliveryCompanionErrandPhase::GoToPickup;
    ADeliveryCharacter* Player=nullptr;
    if (bReturning)
    {
        if (GetCarriedItem()!=Item) { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("HeldItemChanged")); return; }
        if (Now<NextItemOwnerRetryAt) return;
        Player=Cast<ADeliveryCharacter>(GetOwnerPawn());
        if (!Player || Player->GetTaskPlayerState()!=OwnerPlayerState || !Player->CanHandleHandheldItem())
        {
            if (Errand.Phase!=EDeliveryCompanionErrandPhase::WaitingForOwner) SetErrandPhase(EDeliveryCompanionErrandPhase::WaitingForOwner);
            if (!Current()) return;
            ReportErrandPause(TEXT("OwnerUnavailableOrRiding"));
            if (Current()) { EnsureOnFoot(); if (Current()) NextItemOwnerRetryAt=Now+.5; }
            return;
        }
        if (Errand.Phase==EDeliveryCompanionErrandPhase::WaitingForCapacity && !Player->CanReceiveHandheldItem(Item) &&
            FVector::DistSquared(Npc->GetActorLocation(),Player->GetActorLocation())<=FMath::Square(220.f))
        { ReportErrandPause(TEXT("OwnerInventoryFullOrBusy")); if (Current()) NextItemOwnerRetryAt=Now+.5; return; }
        if (Errand.Phase!=EDeliveryCompanionErrandPhase::ReturnToOwner) SetErrandPhase(EDeliveryCompanionErrandPhase::ReturnToOwner);
    }
    else if (GetCarriedItem())
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("HandsOccupied")); return; }
    if (!Current()) return;
    const float Dt=LastErrandUpdateAt>0?FMath::Clamp(float(Now-LastErrandUpdateAt),0.f,1.f):0;
    LastErrandUpdateAt=Now; ErrandActiveSeconds+=Dt;
    if (ErrandActiveSeconds>=ErrandTimeLimitSeconds)
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("ErrandTimedOutItemPreserved")); return; }
    FVector Goal;
    float Radius=220.f;
    UDeliveryInteractableComponent* Interaction=nullptr;
    bool bVisible=false;
    if (bReturning) Goal=Player->GetActorLocation();
    else
    {
        FVector Point;
        bVisible=CanSeeWorldItem(Item,Point);
        if (bVisible) { ItemLastSeenLocation=Point; ItemLastSeenAt=Now; }
        // 没看见时只走向保存的实见点，绝不读取不可见实物当前坐标导航
        Goal=ItemLastSeenLocation;
        Interaction=UDeliveryInteractableComponent::FindOn(Item,EDeliveryInteractionKey::PickupE);
        if (bVisible && Interaction) Radius=FMath::Clamp(Interaction->InteractRadius,30.f,200.f);
        else Radius=100.f;
    }
    if (Goal.ContainsNaN()) { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("InvalidObservedLocation")); return; }
    const bool bOnFoot=UpdateVehicleTravel(Goal,Radius,EDeliveryCompanionTravelPurpose::Errand);
    if (!Current()) return;
    if (!bOnFoot) { LastErrandUpdateAt=Now; return; }
    bool bInReach=bReturning?FVector::DistSquared(Npc->GetActorLocation(),Goal)<=FMath::Square(220.f):
        bVisible && Interaction && Interaction->CanInteract(Npc);
    bInReach=bInReach && ClearItemApproach(Npc,bReturning?static_cast<AActor*>(Player):Item,Item,Goal);
    const bool bAtDestination=bInReach || MoveForErrand(Goal,Radius,bReturning?static_cast<AActor*>(Player):bVisible?Item:nullptr);
    if (!Current()) return;
    LastErrandUpdateAt=Now;
    if (!bAtDestination) return;
    if (!bReturning && !bVisible)
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("ItemNotFoundAtLastSeenPoint")); return; }
    StopErrandMovement();
    if (!Current()) return;
    if (bReturning && !Player->CanReceiveHandheldItem(Item))
    {
        SetErrandPhase(EDeliveryCompanionErrandPhase::WaitingForCapacity);
        if (Current()) { ReportErrandPause(TEXT("OwnerInventoryFullOrBusy")); if (Current()) NextItemOwnerRetryAt=Now+.5; }
        return;
    }
    FName Failure;
    const bool bSucceeded=RunItemTransfer(bReturning,Failure);
    if (!Current()) return;
    if (!bSucceeded)
    {
        // 原实物仍在自己手中时可等候重试，失败不自动丢弃或重复取新件
        if (bReturning && GetCarriedItem()==Item)
        { ReportErrandPause(Failure); if (Current()) NextItemOwnerRetryAt=Now+.5; return; }
        FinishErrand(EDeliveryNpcActionStatus::Failed,Failure); return;
    }
    if (bReturning)
    { FinishErrand(EDeliveryNpcActionStatus::Succeeded,TEXT("ItemReturnedToOwner")); return; }
    if (GetCarriedItem()!=Item)
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("ItemChangedAfterPickup")); return; }
    SetErrandPhase(EDeliveryCompanionErrandPhase::ReturnToOwner);
    if (!Current()) return;
    NextItemObservationAt=0; RefreshItemObservations();
    if (!Current()) return;
    Npc->ObserveFact(TEXT("CompanionItemPickedUp"),Item,
        TEXT("游戏确认二狗拿到了选中的同一件普通道具，正在带回给主人；还没有完成交还。"),EDeliveryNpcEvidenceSource::ActionResult);
}
