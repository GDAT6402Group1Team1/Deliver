#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Characters/DeliveryNpcAIController.h"
#include "AbilitySystem/Abilities/GA_CompanionItemTransfer.h"
#include "AbilitySystemComponent.h"
#include "Animation/AnimSequence.h"
#include "AIController.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Inventory/DeliveryCaterpillarItem.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"

EDeliveryNpcAction FDeliveryCompanionItemChoice::GetAction() const
{
    return Action!=EDeliveryNpcAction::None?Action:
        bReturnItem?EDeliveryNpcAction::ReturnItem:EDeliveryNpcAction::FetchItem;
}

namespace
{
    constexpr float ItemSearchPointAngles[]={0.f,-75.f,75.f,180.f};
    constexpr float ItemSearchLookSeconds=1.1f;
    constexpr double ItemSearchSightLossGraceSeconds=.65;

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

    struct FGroundPickupGeometry
    {
        FVector Grip;
        FVector Palm;
        float Reach=0.f;
        float Tolerance=0.f;
    };

    bool GetGroundPickupGeometry(const ADeliveryCompanionCharacter& Npc,const ADeliveryHandheldItem& Item,
        const UDeliveryCompanionTuning& Tuning,FGroundPickupGeometry& Geometry)
    {
        const auto* Mesh=Npc.GetMesh();
        const auto* MeshAsset=Mesh?Mesh->GetSkeletalMeshAsset():nullptr;
        const auto* ItemMesh=Item.GetItemMesh();
        const auto* Animation=Tuning.GroundPickupAnimation.Get();
        const auto* Movement=Npc.GetCharacterMovement();
        if (!Item.bUseNpcGroundPickupPose || !ItemMesh || ItemMesh!=Item.GetRootComponent() ||
            Item.NpcGroundPickupGrip.ContainsNaN() || Item.NpcHandSocket!=TEXT("RightHand") || !MeshAsset ||
            !Animation || Animation->GetSkeleton()!=MeshAsset->GetSkeleton() || Animation->HasRootMotion() ||
            !FMath::IsFinite(Animation->GetPlayLength()) || !FMath::IsFinite(Tuning.GroundPickupContactTime) ||
            Tuning.GroundPickupContactTime<=0 || Tuning.GroundPickupContactTime>=Animation->GetPlayLength() ||
            Tuning.GroundPickupPalmPosition.ContainsNaN() || !FMath::IsFinite(Tuning.GroundPickupContactTolerance) ||
            !Movement || !Movement->IsMovingOnGround() || !Movement->CurrentFloor.IsWalkableFloor() ||
            Movement->CurrentFloor.HitResult.ImpactNormal.Z<.94f) return false;
        Geometry.Grip=ItemMesh->GetComponentTransform().TransformPosition(Item.NpcGroundPickupGrip);
        Geometry.Palm=Mesh->GetComponentTransform().TransformPosition(Tuning.GroundPickupPalmPosition);
        Geometry.Reach=static_cast<float>(FVector::Dist2D(Geometry.Palm,Npc.GetActorLocation()));
        Geometry.Tolerance=FMath::Clamp(Tuning.GroundPickupContactTolerance,5.f,30.f);
        return !Geometry.Grip.ContainsNaN() && !Geometry.Palm.ContainsNaN() && Geometry.Reach>=35.f && Geometry.Reach<=100.f &&
            FMath::Abs(Geometry.Palm.Z-Geometry.Grip.Z)<=Geometry.Tolerance;
    }

    bool IsGroundPickupContactValid(ADeliveryCompanionCharacter* Npc,ADeliveryHandheldItem* Item,const FGroundPickupGeometry& Geometry)
    {
        return Npc->GetVelocity().SizeSquared2D()<=FMath::Square(20.f) && Item->GetVelocity().SizeSquared()<=FMath::Square(25.f) &&
            FVector::DistSquared(Geometry.Palm,Geometry.Grip)<=FMath::Square(Geometry.Tolerance) &&
            ClearItemApproach(Npc,Item,Item,Geometry.Grip);
    }
}

bool UDeliveryCompanionComponent::CanWalkForNearbyItem(const FDeliveryCompanionItemChoice& Choice) const
{
    auto* Npc=GetCompanion();
    auto* Item=Choice.Item.Get();
    auto* Receiver=Cast<ADeliveryCharacter>(GetOwnerPawn());
    if (!IsValid(Npc) || !IsValid(Item) || !IsValid(Receiver) || Receiver->IsActorBeingDestroyed()) return false;
    if (Choice.bSearchLead) return !Choice.bReturnItem;
    constexpr double LocalItemDistance=240.;
    const FVector Origin=Npc->GetActorLocation();
    const FVector ReceiverPosition=Receiver->GetActorLocation();
    if (FVector::DistSquared(Origin,ReceiverPosition)>FMath::Square(LocalItemDistance) ||
        !ClearItemApproach(Npc,Receiver,Item,ReceiverPosition)) return false;
    if (Choice.bReturnItem) return true;
    if (Choice.IsOwnerInventorySource()) return Choice.SourceCarrier.Get()==Receiver && CanTakeOwnerItem(Item);
    FVector ItemPosition;
    return CanSeeWorldItem(Item,ItemPosition) &&
        FVector::DistSquared(Origin,ItemPosition)<=FMath::Square(LocalItemDistance);
}

bool UDeliveryCompanionComponent::CanKeepItemErrand(const FDeliveryCompanionItemChoice& Choice,APlayerState* Requester,
    int32 ExpectedRevision,EDeliveryCompanionTravelMode RequestedTravelMode) const
{
    const auto Action=Choice.GetAction();
    const auto CurrentTravelMode=Errand.bLocalItemTransfer?EDeliveryCompanionTravelMode::Walk:TravelMode;
    if (!HasActiveErrand() || Errand.CommandRevision!=CommandRevision || Errand.Action!=Action || Errand.Item!=Choice.Item ||
        (RequestedTravelMode!=EDeliveryCompanionTravelMode::Unspecified && RequestedTravelMode!=CurrentTravelMode)) return false;
    if (!CanApplyItemChoice(Choice,Requester,ExpectedRevision)) return false;
    if (Choice.bReturnItem) return true;
    if (Action==EDeliveryNpcAction::SearchNearOwner)
        return Errand.ItemSourceCarrier==Choice.SourceCarrier &&
            FVector::DistSquared(ItemLastSeenLocation,Choice.ObservedLocation)<=FMath::Square(70.);
    if (Action==EDeliveryNpcAction::TakeItem) return Errand.ItemSourceCarrier==Choice.SourceCarrier;
    if (Action==EDeliveryNpcAction::InsertCaterpillar)
    {
        if (Errand.ItemUsePackage!=Choice.UsePackage || Errand.bInsertIntoHeldPackage!=Choice.bInsertIntoHeldPackage) return false;
        if (GetCarriedItem()==Choice.Item.Get()) return true;
        if (Choice.IsOwnerInventorySource() || !Errand.PrankInventorySource.IsNone())
            return Errand.ItemSourceCarrier==Choice.SourceCarrier && Errand.PrankInventorySource==Choice.Source;
    }

    // 同一实物的新线索可能指向另一处。拿取用本轮真实视线，找物用当轮签发的新线索
    FVector Location=Choice.ObservedLocation;
    if (!Choice.bSearchLead && !CanSeeWorldItem(Choice.Item.Get(),Location)) return false;
    constexpr double LocationTolerance=30.;
    return !ItemLastSeenLocation.ContainsNaN() && !Location.ContainsNaN() &&
        FVector::DistSquared(Location,ItemLastSeenLocation)<=FMath::Square(LocationTolerance);
}

bool UDeliveryCompanionComponent::ApplyItemChoice(const FDeliveryCompanionItemChoice& Choice,APlayerState* Requester,int32 ExpectedRevision,
    EDeliveryCompanionTravelMode RequestedTravelMode)
{
    // 重复确认仍在做的同一件事，保留导航、找物阶段与正在进行的交接
    if (CanKeepItemErrand(Choice,Requester,ExpectedRevision,RequestedTravelMode)) return true;
    if (!CanApplyItemChoice(Choice,Requester,ExpectedRevision)) return false;
    const bool bAreaSearch=Choice.GetAction()==EDeliveryNpcAction::SearchNearOwner;
    if (bAreaSearch && RequestedTravelMode!=EDeliveryCompanionTravelMode::Unspecified) return false;
    const bool bWalkLocally=bAreaSearch ||
        (RequestedTravelMode==EDeliveryCompanionTravelMode::Unspecified && CanWalkForNearbyItem(Choice));
    if (RequestedTravelMode==EDeliveryCompanionTravelMode::Unspecified && TravelMode==EDeliveryCompanionTravelMode::Unspecified && !bWalkLocally)
        return false;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryHandheldItem> Target=Choice.Item;
    const bool bHadErrand=HasActiveErrand();
    if (bHadErrand) FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("ReplacedByOwner"));
    if (!WeakThis.IsValid() || (!bAreaSearch && !Target.IsValid()) || !CanCommand(Requester) ||
        CommandRevision!=ExpectedRevision+(bHadErrand?1:0) || HasActiveErrand()) return false;
    const auto StillCurrent=[&]() { return WeakThis.IsValid() && (bAreaSearch || Target.IsValid()) && CanCommand(Requester) &&
        CommandRevision==ExpectedRevision+(bHadErrand?1:0) && !HasActiveErrand(); };
    StopFollowMovement();
    if (!StillCurrent()) return false;
    StopErrandMovement();
    if (!StillCurrent()) return false;
    CancelAssist(TEXT("OwnerChangedArrangement"));
    if (!StillCurrent() || !CanApplyItemChoice(Choice,Requester,ExpectedRevision+(bHadErrand?1:0))) return false;
    FVector Seen;
    const bool bFromOwner=Choice.IsOwnerInventorySource();
    const bool bAlreadyHeld=Choice.GetAction()==EDeliveryNpcAction::InsertCaterpillar && GetCarriedItem()==Target.Get();
    const bool bDirectPickup=!bAreaSearch && !Choice.bReturnItem && !Choice.bSearchLead && !bFromOwner && !bAlreadyHeld;
    if (bDirectPickup && !CanSeeWorldItem(Target.Get(),Seen)) return false;
    if (bWalkLocally && !bAreaSearch && !CanWalkForNearbyItem(Choice)) return false;
    ++CommandRevision;
    ApplyTravelInstruction(RequestedTravelMode);
    Errand={}; Errand.Action=Choice.GetAction();
    Errand.Status=EDeliveryNpcActionStatus::Running;
    Errand.Phase=Choice.bReturnItem?EDeliveryCompanionErrandPhase::ReturnToOwner:
        (Choice.bSearchLead || bAreaSearch)?EDeliveryCompanionErrandPhase::SearchNearby:EDeliveryCompanionErrandPhase::GoToPickup;
    Errand.Item=Target; Errand.CommandRevision=CommandRevision;
    Errand.ItemSourceCarrier=Choice.SourceCarrier; Errand.ItemUsePackage=Choice.UsePackage;
    Errand.bInsertIntoHeldPackage=Choice.bInsertIntoHeldPackage;
    if (Errand.Action==EDeliveryNpcAction::InsertCaterpillar && bFromOwner) Errand.PrankInventorySource=Choice.Source;
    if (bAlreadyHeld) Errand.Phase=EDeliveryCompanionErrandPhase::GoToDelivery;
    Errand.bLocalItemTransfer=bWalkLocally;
    ItemLastSeenLocation=bDirectPickup?Seen:Choice.ObservedLocation;
    ItemLastSeenAt=bDirectPickup?GetWorld()->GetTimeSeconds():Choice.ObservedAt;
    ErrandActiveSeconds=0; ErrandTimeLimitSeconds=GetTuning().ErrandTimeoutSeconds;
    bItemRecoverySearchUsed=false;
    ResetItemSearchRoute(GetCompanion()->GetActorLocation(),false);
    bErrandTravelBudgetAdded=false; bErrandVehicleApproachBudgetAdded=false;
    ErrandInteractionSeconds=0; bHasErrandDestination=false; NextErrandNavigationAt=0;
    NextItemOwnerRetryAt=0; LastErrandPause=NAME_None; SetSuspension(NAME_None);
    bCaterpillarOffered=false; CaterpillarOpportunityId.Invalidate(); bAnnouncedCaterpillarOpportunity=true;
    GetOwner()->ForceNetUpdate();
    return true;
}

void UDeliveryCompanionComponent::ResetItemSearchRoute(const FVector& Origin,bool bAlreadyOnFoot)
{
    ItemSearchPointIndex=0;
    AreaSearchSeenNames.Reset();
    bAreaSearchReachedStand=false;
    ItemSearchPointStartedAt=ErrandActiveSeconds;
    ItemSearchLookStartedAt=-1;
    bItemSearchOnFoot=bAlreadyOnFoot;
    ItemSearchSide=(Origin-ItemLastSeenLocation).GetSafeNormal2D();
    if (ItemSearchSide.IsNearlyZero()) ItemSearchSide=FVector::ForwardVector;
}

void UDeliveryCompanionComponent::SetItemSearchFocus(const FVector& Point,float Angle)
{
    const auto* Npc=GetCompanion();
    auto* AI=Npc?Cast<AAIController>(Npc->GetController()):nullptr;
    if (!AI) return;
    const FVector Eye=Npc->GetPawnViewLocation();
    FVector Toward=(Point-Eye).GetSafeNormal2D();
    // 线索在脚下时也要有水平环顾方向，身体的低头姿势由动画处理
    if (Toward.IsNearlyZero()) Toward=-ItemSearchSide;
    ItemSearchFocus=Eye+Toward.RotateAngleAxis(Angle,FVector::UpVector)*250.f;
    AI->SetFocalPoint(ItemSearchFocus.GetValue(),EAIFocusPriority::Gameplay);
}

bool UDeliveryCompanionComponent::IsCurrentItemUse(FGuid UseId,int32 Revision,ADeliveryHandheldItem* Item) const
{
    const auto* Npc=GetCompanion();
    return UseId.IsValid() && ItemUseId==UseId && HasActiveErrand() && Errand.CommandRevision==Revision &&
        CommandRevision==Revision && Errand.Item.Get()==Item && IsValid(Item) && Item->GetWorld()==GetWorld() &&
        Npc && Npc->HasAuthority() && !Npc->IsActorBeingDestroyed() && CanCommand(OwnerPlayerState) &&
        (Errand.Action!=EDeliveryNpcAction::InsertCaterpillar ||
            IsCurrentPrankPackage(Errand.ItemUsePackage.Get(),OwnerPlayerState,Errand.bInsertIntoHeldPackage)) &&
        !Npc->IsKnockedDown() && !IsUsingErrandVehicle() && !HasActiveHomeArrangement() && !HomeState.CarriedProp &&
        !Npc->Agent->ShouldPauseForConversation() &&
        (!Npc->Agent->HasActiveAction() || Npc->Agent->CanContinueCompanionMovement()) &&
        (!Npc->Combat || (!Npc->Combat->IsReactingToHit() && !Npc->Combat->IsRecovering() && !Npc->Combat->IsStriking()));
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
    if (Errand.Action==EDeliveryNpcAction::TakeItem || !Errand.PrankInventorySource.IsNone())
    {
        auto* Source=GetOwnerItemCarrier();
        if (Errand.Phase!=EDeliveryCompanionErrandPhase::GoToPickup || Errand.ItemSourceCarrier.Get()!=Source || !CanTakeOwnerItem(Item) ||
            (!Errand.PrankInventorySource.IsNone() && GetCurrentOwnerItemSource(Item)!=Errand.PrankInventorySource))
        { Reason=TEXT("OwnerItemChanged"); return false; }
        return DeliveryHandheldTransfer::TryTransfer(Source,Npc,Item,Reason,StillCurrent);
    }
    FVector Seen;
    if (Errand.Phase!=EDeliveryCompanionErrandPhase::GoToPickup || !CanSeeWorldItem(Item,Seen))
    { Reason=TEXT("ItemNotVisibleAtPickup"); return false; }
    if (Errand.GroundPickupServerTime>=0 || Errand.bInsertIntoHeldPackage)
    {
        // GAS Commit 期间实物仍可能被移动，交接前最后复核同一触物点
        const auto& PickupTuning=GetTuning();
        const auto AtContact=[&]()
        {
            FGroundPickupGeometry Geometry;
            return StillCurrent() && Errand.GroundPickupServerTime>=0 &&
                GetHomeServerTime()-Errand.GroundPickupServerTime>=PickupTuning.GroundPickupContactTime &&
                CanSeeWorldItem(Item,Seen) && GetGroundPickupGeometry(*Npc,*Item,PickupTuning,Geometry) &&
                IsGroundPickupContactValid(Npc,Item,Geometry);
        };
        if (!AtContact())
        { Reason=TEXT("GroundPickupContactChanged"); return false; }
        if (Errand.bInsertIntoHeldPackage)
        {
            auto* Package=Errand.ItemUsePackage.Get();
            const bool bInserted=Package && Package->TryInsertGroundCaterpillarIntoHeldPackage(
                Npc,Cast<ADeliveryCaterpillarItem>(Item),OwnerPlayerState,AtContact);
            Reason=bInserted?FName(TEXT("CaterpillarInserted")):FName(TEXT("PrankInsertionRejectedItemPreserved"));
            return bInserted;
        }
    }
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

UDeliveryCompanionComponent::EGroundPickupProgress UDeliveryCompanionComponent::UpdateGroundPickup(
    ADeliveryHandheldItem* Item,bool bInReach,TFunctionRef<bool()> IsCurrent)
{
    const bool bPosing=Errand.GroundPickupServerTime>=0;
    const bool bAligning=GroundPickupAlignmentStartedAt>=0;
    if (!bInReach && !bAligning && !bPosing) return EGroundPickupProgress::Unavailable;
    auto* Npc=GetCompanion();
    const auto& PickupTuning=GetTuning();
    const auto Abort=[&]()
    {
        if (bAligning || bPosing) StopErrandMovement();
        return bPosing?EGroundPickupProgress::Preparing:EGroundPickupProgress::Unavailable;
    };
    FGroundPickupGeometry Geometry;
    if (!GetGroundPickupGeometry(*Npc,*Item,PickupTuning,Geometry)) return Abort();
    const FVector& Grip=Geometry.Grip;
    const FVector& Palm=Geometry.Palm;
    const float Reach=Geometry.Reach;
    const FVector Origin=Npc->GetActorLocation();
    const bool bContactValid=bInReach && IsGroundPickupContactValid(Npc,Item,Geometry);
    if (bPosing)
    {
        // 取物点随待拾取的实物变化，接触帧前离开手掌范围就撤销本次动作
        if (!bContactValid) return Abort();
        return GetHomeServerTime()-Errand.GroundPickupServerTime>=PickupTuning.GroundPickupContactTime?
            EGroundPickupProgress::AtContact:EGroundPickupProgress::Preparing;
    }

    const double Now=GetWorld()->GetTimeSeconds();
    if (bAligning && Now-GroundPickupAlignmentStartedAt>=2.5) return Abort();
    if (!bAligning || (bErrandOwnsNavigation && FMath::Abs(FVector::Dist2D(Origin,Grip)-Reach)<=8.f))
    {
        const double Started=bAligning?GroundPickupAlignmentStartedAt:Now;
        StopErrandMovement();
        if (!IsCurrent() || !IsValid(Item) || Errand.Phase!=EDeliveryCompanionErrandPhase::GoToPickup || Errand.Item.Get()!=Item)
            return EGroundPickupProgress::Preparing;
        bHasErrandDestination=false;
        NextErrandNavigationAt=0;
        GroundPickupAlignmentStartedAt=Started;
    }
    if (FMath::Abs(FVector::Dist2D(Origin,Grip)-Reach)>8.f)
    {
        MoveForErrand(Grip,Reach+35.f,Item,Reach);
        return EGroundPickupProgress::Preparing;
    }
    const double PalmAngle=FMath::FindDeltaAngleDegrees(Npc->GetActorRotation().Yaw,(Palm-Origin).Rotation().Yaw);
    SetItemSearchFocus(Grip,static_cast<float>(-PalmAngle));
    const double FacingError=FMath::Abs(FMath::FindDeltaAngleDegrees((Palm-Origin).Rotation().Yaw,(Grip-Origin).Rotation().Yaw));
    if (!bContactValid || FacingError>8. || !IsCurrent()) return EGroundPickupProgress::Preparing;
    Errand.GroundPickupServerTime=GetHomeServerTime();
    Npc->ForceNetUpdate();
    return EGroundPickupProgress::Preparing;
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
    if (Errand.bPrankInsertionCommitted)
    {
        const auto* Pickup=GetTuning().GroundPickupAnimation.Get();
        if (!Npc->IsKnockedDown() && Pickup && Errand.GroundPickupServerTime>=0 &&
            GetHomeServerTime()-Errand.GroundPickupServerTime<Pickup->GetPlayLength()) return;
        FinishErrand(EDeliveryNpcActionStatus::Succeeded,TEXT("CaterpillarInserted"));
        return;
    }
    if (!CanCommand(OwnerPlayerState)) { FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("OwnerLost")); return; }
    if (Npc->IsKnockedDown() || Npc->Agent->ShouldPauseForConversation() ||
        (Npc->Agent->HasActiveAction() && !Npc->Agent->CanContinueCompanionMovement()) ||
        (Npc->Combat && (Npc->Combat->IsReactingToHit() || Npc->Combat->IsRecovering() || Npc->Combat->IsStriking())))
    { StopErrandMovement(); if (Current()) SetSuspension(Npc->IsKnockedDown()?TEXT("Down"):TEXT("OtherBehavior")); return; }
    if (HasActiveHomeArrangement() || HomeState.CarriedProp)
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("HandsOccupiedByHomeUse")); return; }
    if (!CapturePendingWaitLocation() || !Current())
    {
        if (Current()) LastErrandUpdateAt=0;
        return;
    }
    const bool bAreaSearch=Errand.Action==EDeliveryNpcAction::SearchNearOwner;
    if (bAreaSearch)
    {
        const auto* OwnerBody=Cast<ADeliveryCharacter>(GetOwnerPawn());
        if (Errand.Phase!=EDeliveryCompanionErrandPhase::SearchNearby || !IsValid(OwnerBody) || OwnerBody->IsActorBeingDestroyed() ||
            Errand.ItemSourceCarrier.Get()!=OwnerBody || OwnerBody->GetTaskPlayerState()!=OwnerPlayerState || OwnerBody->IsRidingVehicle())
        { FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("ItemSearchOwnerChanged")); return; }
        const FVector OwnerFeet=OwnerBody->GetActorLocation()-FVector(0,0,OwnerBody->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
        if (FVector::DistSquared2D(OwnerFeet,ItemLastSeenLocation)>FMath::Square(DeliveryCompanionItemSearch::Radius) ||
            FMath::Abs(OwnerFeet.Z-ItemLastSeenLocation.Z)>110.)
        { FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("ItemSearchOwnerLeftArea")); return; }
    }
    auto* Item=Errand.Item.Get();
    if (!bAreaSearch && (!DeliveryHandheldTransfer::IsOrdinaryItem(Item) || Item->GetWorld()!=GetWorld()))
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("ItemLostOrNoLongerOrdinary")); return; }
    if (Errand.Action==EDeliveryNpcAction::InsertCaterpillar &&
        !IsCurrentPrankPackage(Errand.ItemUsePackage.Get(),OwnerPlayerState,Errand.bInsertIntoHeldPackage))
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("PrankPackageUnavailableItemPreserved")); return; }
    if (Errand.Action==EDeliveryNpcAction::InsertCaterpillar && GetCarriedItem()==Item)
    {
        UpdateCaterpillarInsertion();
        return;
    }
    if (Errand.Action==EDeliveryNpcAction::CollectCaterpillar && GetCarriedItem()==Item)
    {
        const auto* Animation=GetTuning().GroundPickupAnimation.Get();
        if (Errand.GroundPickupServerTime>=0 && Animation &&
            GetHomeServerTime()-Errand.GroundPickupServerTime<Animation->GetPlayLength()) return;
        FinishErrand(EDeliveryNpcActionStatus::Succeeded,TEXT("ItemTakenIntoHand"));
        return;
    }
    const bool bSearching=HasNearbyItemSearch();
    const bool bReturning=!bSearching && Errand.Phase!=EDeliveryCompanionErrandPhase::GoToPickup;
    const auto* Held=GetCarriedItem();
    if (bReturning && Held!=Item)
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("HeldItemChanged")); return; }
    if (!bReturning && Held && !Errand.bInsertIntoHeldPackage)
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("HandsOccupied")); return; }
    // 越障保留原步行编号，落地后再看目标、切换寻找阶段或交还实物
    if (const auto* NpcAI=Cast<ADeliveryNpcAIController>(Npc->GetController()); NpcAI && NpcAI->IsTraversingObstacle(ErrandMoveId))
    {
        LastErrandUpdateAt=0;
        return;
    }
    const double Now=GetWorld()->GetTimeSeconds();
    ADeliveryCharacter* Player=nullptr;
    if (bReturning)
    {
        if (Errand.GroundPickupServerTime>=0)
        {
            const auto* Animation=GetTuning().GroundPickupAnimation.Get();
            if (Animation && GetHomeServerTime()-Errand.GroundPickupServerTime<Animation->GetPlayLength()) return;
            StopErrandMovement();
            if (!Current()) return;
        }
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
    if (!Current()) return;
    const float Dt=LastErrandUpdateAt>0?FMath::Clamp(float(Now-LastErrandUpdateAt),0.f,1.f):0;
    LastErrandUpdateAt=Now; ErrandActiveSeconds+=Dt;
    if (ErrandActiveSeconds>=ErrandTimeLimitSeconds)
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("ErrandTimedOutItemPreserved")); return; }
    if (bSearching)
    {
        const FVector Hint=ItemLastSeenLocation;
        FVector Observed;
        if (!bAreaSearch && CanSeeWorldItem(Item,Observed) &&
            FVector::DistSquared(Observed,Hint)<=FMath::Square(DeliveryCompanionItemSearch::Radius) &&
            Item->GetItemComponent()->CanTransferTo(Npc))
        {
            ItemLastSeenLocation=Observed; ItemLastSeenAt=Now;
            SetErrandPhase(EDeliveryCompanionErrandPhase::GoToPickup);
            return;
        }
        if (!bItemSearchOnFoot)
        {
            if (!UpdateVehicleTravel(Hint,200.f,EDeliveryCompanionTravelPurpose::Errand) || !Current()) return;
            bItemSearchOnFoot=true;
        }
        const FVector Point=ItemSearchPointIndex==0?Hint:
            Hint+ItemSearchSide.RotateAngleAxis(ItemSearchPointAngles[ItemSearchPointIndex],FVector::UpVector)*180.f;
        if (ItemSearchLookStartedAt<0 && MoveForErrand(Point,180.f,nullptr))
        {
            if (!Current()) return;
            StopErrandMovement();
            if (!Current()) return;
            ItemSearchLookStartedAt=ErrandActiveSeconds;
            if (bAreaSearch) bAreaSearchReachedStand=true;
        }
        if (!Current()) return;
        if (ItemSearchLookStartedAt>=0)
        {
            const float Elapsed=ErrandActiveSeconds-ItemSearchLookStartedAt;
            const auto* Movement=Npc->GetCharacterMovement();
            const double HeightAboveFeet=Hint.Z-Npc->GetNavAgentLocation().Z;
            // 平缓地面停稳后才低身，较高线索和坡边沿用站姿
            if (Errand.LowItemInspectionServerTime<0 && Elapsed<=.35f && GetTuning().LowItemInspectionAnimation &&
                Movement && Movement->IsMovingOnGround() && Movement->CurrentFloor.IsWalkableFloor() &&
                Movement->CurrentFloor.HitResult.ImpactNormal.Z>=.94f && Npc->GetVelocity().SizeSquared2D()<=FMath::Square(20.f) &&
                HeightAboveFeet>=-20.f && HeightAboveFeet<=75.f)
            {
                Errand.LowItemInspectionServerTime=GetHomeServerTime();
                Npc->ForceNetUpdate();
            }
            const float Sweep=FMath::Lerp(-45.f,45.f,FMath::SmoothStep(0.f,ItemSearchLookSeconds,Elapsed));
            const FVector Focus=bAreaSearch?Npc->GetActorLocation()-ItemSearchSide.RotateAngleAxis(
                ItemSearchPointAngles[ItemSearchPointIndex],FVector::UpVector)*250.f:Hint;
            SetItemSearchFocus(Focus,Sweep);
            if (!Current() || Elapsed<ItemSearchLookSeconds) return;
        }
        // 初段允许赶到主人身边，后续几处沿近处搜索，每处都有独立时限
        else if (ErrandActiveSeconds-ItemSearchPointStartedAt<(ItemSearchPointIndex==0?22.f:4.f)) return;
        StopErrandMovement();
        if (!Current()) return;
        if (++ItemSearchPointIndex>=UE_ARRAY_COUNT(ItemSearchPointAngles))
        {
            if (bAreaSearch && bAreaSearchReachedStand)
                FinishErrand(EDeliveryNpcActionStatus::Succeeded,TEXT("ItemSearchAreaChecked"));
            else FinishErrand(EDeliveryNpcActionStatus::Failed,bAreaSearch?TEXT("ItemSearchAreaUnreachable"):TEXT("ItemNotFoundInSearchArea"));
            return;
        }
        ItemSearchPointStartedAt=ErrandActiveSeconds;
        bHasErrandDestination=false;
        NextErrandNavigationAt=0;
        return;
    }
    FVector Goal;
    float Radius=220.f;
    UDeliveryInteractableComponent* Interaction=nullptr;
    bool bVisible=false;
    const bool bTakingOwnerItem=Errand.Action==EDeliveryNpcAction::TakeItem || !Errand.PrankInventorySource.IsNone();
    if (bReturning) Goal=Player->GetActorLocation();
    else
    {
        FVector Point;
        if (bTakingOwnerItem)
        {
            auto* Source=GetOwnerItemCarrier();
            if (!Source || Errand.ItemSourceCarrier.Get()!=Source || !CanTakeOwnerItem(Item) ||
                (!Errand.PrankInventorySource.IsNone() && GetCurrentOwnerItemSource(Item)!=Errand.PrankInventorySource))
            { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("OwnerItemChanged")); return; }
            Point=Source->GetActorLocation();
            bVisible=true;
        }
        else bVisible=CanSeeWorldItem(Item,Point);
        if (bVisible) { ItemLastSeenLocation=Point; ItemLastSeenAt=Now; }
        // 没看见时只走向保存的实见点，绝不读取不可见实物当前坐标导航
        Goal=ItemLastSeenLocation;
        Interaction=UDeliveryInteractableComponent::FindOn(Item,EDeliveryInteractionKey::PickupE);
        if (bTakingOwnerItem) Radius=180.f;
        else if (bVisible && Interaction) Radius=FMath::Clamp(Interaction->InteractRadius,30.f,200.f);
        else Radius=100.f;
    }
    if (Goal.ContainsNaN()) { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("InvalidObservedLocation")); return; }
    const bool bLostPickup=!bReturning && !bVisible;
    const bool bConfirmedSightLoss=bLostPickup && Now-ItemLastSeenAt>=ItemSearchSightLossGraceSeconds;
    if (bLostPickup && (GroundPickupAlignmentStartedAt>=0 || Errand.GroundPickupServerTime>=0))
    {
        StopErrandMovement();
        return;
    }
    if (ItemSearchLookStartedAt>=0)
    {
        if (bLostPickup)
        {
            if (ErrandActiveSeconds-ItemSearchLookStartedAt>=ItemSearchLookSeconds)
                FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("ItemNotFoundAtLastSeenPoint"));
            return;
        }
        StopErrandMovement();
        if (!Current()) return;
    }
    const bool bPickupAlreadyOnFoot=GroundPickupAlignmentStartedAt>=0;
    const bool bOnFoot=bPickupAlreadyOnFoot?!IsUsingErrandVehicle():UpdateVehicleTravel(Goal,Radius,EDeliveryCompanionTravelPurpose::Errand);
    if (!Current()) return;
    if (!bOnFoot)
    {
        if (bPickupAlreadyOnFoot) StopErrandMovement();
        if (Current()) LastErrandUpdateAt=Now;
        return;
    }
    if (bConfirmedSightLoss && !bItemRecoverySearchUsed &&
        FVector::DistSquared(Npc->GetNavAgentLocation(),Goal)<=FMath::Square(DeliveryCompanionItemSearch::Radius))
    {
        // 近处失视时沿同一委托实见的位置换角度找，只给一次复查机会
        SetErrandPhase(EDeliveryCompanionErrandPhase::SearchNearby);
        if (!Current()) return;
        bItemRecoverySearchUsed=true;
        ResetItemSearchRoute(Npc->GetActorLocation(),true);
        return;
    }
    bool bInReach=bReturning || bTakingOwnerItem?FVector::DistSquared(Npc->GetActorLocation(),Goal)<=FMath::Square(bTakingOwnerItem?180.f:220.f):
        bVisible && Interaction && Interaction->CanInteract(Npc);
    AActor* ApproachTarget=bTakingOwnerItem?Errand.ItemSourceCarrier.Get():bReturning?static_cast<AActor*>(Player):Item;
    bInReach=bInReach && ClearItemApproach(Npc,ApproachTarget,Item,Goal);
    const auto GroundPickup=bVisible && !bTakingOwnerItem?UpdateGroundPickup(Item,bInReach,Current):EGroundPickupProgress::Unavailable;
    if (!Current() || GroundPickup==EGroundPickupProgress::Preparing) return;
    if (bInReach && Errand.bInsertIntoHeldPackage && GroundPickup!=EGroundPickupProgress::AtContact)
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("GroundPickupContactChanged")); return; }
    const bool bAtDestination=bInReach || MoveForErrand(Goal,Radius,bVisible || bReturning?ApproachTarget:nullptr);
    if (!Current()) return;
    LastErrandUpdateAt=Now;
    if (!bAtDestination) return;
    if (bLostPickup)
    {
        if (!bItemRecoverySearchUsed || !bConfirmedSightLoss) return;
        // 换角度重见后也可能在接近时转出视野，到点停稳看一次再判找不到
        StopErrandMovement();
        if (!Current()) return;
        ItemSearchLookStartedAt=ErrandActiveSeconds;
        SetItemSearchFocus(Goal,0.f);
        return;
    }
    const double PickupStart=Errand.GroundPickupServerTime;
    if (PickupStart<0)
    {
        StopErrandMovement();
        if (!Current()) return;
    }
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
    if (Errand.bInsertIntoHeldPackage)
    {
        Errand.bPrankInsertionCommitted=true;
        RecordMemory(FGuid::NewGuid(),TEXT("XiaolvInserted"),TEXT("按主人安排把同一只小绿放进手里的本单原包裹，当前仍需交付"),0);
        return;
    }
    if (GetCarriedItem()!=Item)
    { FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("ItemChangedAfterPickup")); return; }
    if (Errand.Action==EDeliveryNpcAction::TakeItem)
    { FinishErrand(EDeliveryNpcActionStatus::Succeeded,TEXT("ItemTakenIntoHand")); return; }
    if (Errand.Action==EDeliveryNpcAction::InsertCaterpillar || Errand.Action==EDeliveryNpcAction::CollectCaterpillar)
    {
        SetErrandPhase(EDeliveryCompanionErrandPhase::GoToDelivery);
        // 实物已在手中，沿原拾取动画起身后再接近包裹
        if (Current() && PickupStart>=0 && GetCarriedItem()==Item)
        {
            Errand.GroundPickupServerTime=PickupStart;
            Npc->ForceNetUpdate();
        }
        return;
    }
    SetErrandPhase(EDeliveryCompanionErrandPhase::ReturnToOwner);
    if (!Current()) return;
    // 持件归属已经完成，起身表现可继续，打断后只会恢复返还
    if (PickupStart>=0 && Errand.Phase==EDeliveryCompanionErrandPhase::ReturnToOwner && GetCarriedItem()==Item)
    {
        Errand.GroundPickupServerTime=PickupStart;
        Npc->ForceNetUpdate();
    }
    NextItemObservationAt=0; RefreshItemObservations();
    if (!Current()) return;
    Npc->ObserveFact(TEXT("CompanionItemPickedUp"),Item,
        TEXT("游戏确认二狗拿到了选中的同一件普通道具，正在带回给主人；还没有完成交还。"),EDeliveryNpcEvidenceSource::ActionResult);
}
