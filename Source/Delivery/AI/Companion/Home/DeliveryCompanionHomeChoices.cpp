#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "World/Home/DeliveryHome.h"
#include "World/Home/DeliveryHomeObjectComponent.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "AIController.h"
#include "AIHelpers.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISense_Sight.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"

// 试玩排查期间默认打开，能直接看到每个摆放槽为什么没给候选；查清后改回 Log
DEFINE_LOG_CATEGORY_STATIC(LogDeliveryCompanionHomeChoice,Verbose,All);

namespace
{
    FString SnapshotText(FString Text,int32 Limit)
    {
        for (TCHAR& Character:Text) if (Character<32) Character=TEXT(' ');
        return Text.TrimStartAndEnd().Left(Limit);
    }

    bool IsProtocolSlot(FName Id)
    {
        const FString Text=Id.ToString();
        return !Id.IsNone() && Text.Len()<=64 && SnapshotText(Text,64)==Text;
    }

    bool CanSeeObject(const ADeliveryCompanionCharacter* Npc,const UDeliveryHomeObjectComponent* Object)
    {
        const auto* Actor=IsValid(Object)?Object->GetOwner():nullptr;
        const auto* Home=IsValid(Object)?Object->Home.Get():nullptr;
        if (!IsValid(Npc) || !Npc->HasAuthority() || !Npc->GetWorld() || Npc->IsActorBeingDestroyed() ||
            Npc->IsHidden() || Npc->IsKnockedDown() || !IsValid(Actor) || !IsValid(Home) ||
            Actor->IsActorBeingDestroyed() || Actor->IsHidden() || Object->GetWorld()!=Npc->GetWorld() ||
            Actor->GetWorld()!=Npc->GetWorld() || Home->GetWorld()!=Npc->GetWorld() ||
            !Object->IsRegistered() || !Home->ContainsFurniture(Object)) return false;
        const auto* Controller=Cast<AAIController>(Npc->GetController());
        const auto* Perception=IsValid(Controller)?Controller->GetAIPerceptionComponent():nullptr;
        const auto* Sight=IsValid(Perception)?Perception->GetSenseConfig<UAISenseConfig_Sight>():nullptr;
        // 家具也沿当前视觉观察，停用视觉或背对物件不能继续声称亲眼看见
        if (!Sight || Controller->GetPawn()!=Npc || Controller->GetWorld()!=Npc->GetWorld() ||
            Perception->GetWorld()!=Npc->GetWorld() || !Perception->IsRegistered() ||
            !Perception->IsSenseEnabled(UAISense_Sight::StaticClass()) ||
            !FMath::IsFinite(Sight->SightRadius) || Sight->SightRadius<=0 ||
            !FMath::IsFinite(Sight->PeripheralVisionAngleDegrees) ||
            !FMath::IsFinite(Sight->PointOfViewBackwardOffset) || Sight->PointOfViewBackwardOffset<0 || Sight->PointOfViewBackwardOffset>3000 ||
            !FMath::IsFinite(Sight->NearClippingRadius) || Sight->NearClippingRadius<0 || Sight->NearClippingRadius>6000) return false;
        const FBox Bounds=Actor->GetComponentsBoundingBox(true);
        if (!Bounds.IsValid || Bounds.Min.ContainsNaN() || Bounds.Max.ContainsNaN() || Bounds.GetExtent().GetMax()<=0 ||
            !FMath::IsFinite(Home->ObjectSightRadius)) return false;
        FVector Eye=FVector::ZeroVector,Direction=FVector::ZeroVector;
        Perception->GetLocationAndDirection(Eye,Direction);
        if (Eye.ContainsNaN() || Direction.ContainsNaN() || !Direction.Normalize()) return false;
        const float Radius=FMath::Min(Sight->SightRadius,FMath::Clamp(Home->ObjectSightRadius,100.f,3000.f));
        if (Bounds.ComputeSquaredDistanceToPoint(Eye)>FMath::Square(Radius)) return false;
        const float PeripheralCos=FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Sight->PeripheralVisionAngleDegrees,0.f,180.f)));
        const FVector Center=Bounds.GetCenter();
        const FVector Extent=Bounds.GetExtent()*.7f;
        FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionHomeSight),false,Npc);
        // 多取几个实物表面附近的点，避免柜子原点落在墙内时永远认不出来
        for (const FVector Offset:{FVector::ZeroVector,FVector(Extent.X,0,0),FVector(-Extent.X,0,0),
            FVector(0,Extent.Y,0),FVector(0,-Extent.Y,0),FVector(0,0,Extent.Z)})
        {
            const FVector Point=Center+Offset;
            if (FVector::DistSquared(Eye,Point)>FMath::Square(Radius) ||
                !FAISystem::CheckIsTargetInSightCone(Eye,Direction,PeripheralCos,Sight->PointOfViewBackwardOffset,
                    FMath::Square(Sight->NearClippingRadius),FMath::Square(Radius+Sight->PointOfViewBackwardOffset),Point)) continue;
            FHitResult Hit;
            if (!Npc->GetWorld()->LineTraceSingleByChannel(Hit,Eye,Point,ECC_Visibility,Params) || Hit.GetActor()==Actor)
                return true;
        }
        return false;
    }

    bool SameTransform(const FTransform& A,const FTransform& B)
    {
        return !A.ContainsNaN() && !B.ContainsNaN() && FVector::DistSquared(A.GetLocation(),B.GetLocation())<=4.f &&
            FMath::RadiansToDegrees(A.GetRotation().AngularDistance(B.GetRotation()))<=2.f;
    }

    bool StillOffered(const FDeliveryHomeUseCandidate& Expected,ADeliveryCompanionCharacter* Npc,APlayerState* Requester)
    {
        auto* Object=Expected.Object.Get();
        if (!Object || Object->ObjectId!=Expected.ObjectId || Object->GetRevision()!=Expected.Revision) return false;
        TArray<FDeliveryHomeUseCandidate> Current;
        Object->FindAvailableUses(Npc,Requester,Current);
        return Current.ContainsByPredicate([&](const auto& Use)
        {
            return Use.Slot==Expected.Slot && Use.SlotId==Expected.SlotId && Use.UseId==Expected.UseId &&
                Use.Operation==Expected.Operation && Use.Definition==Expected.Definition &&
                SameTransform(Use.EntranceTransform,Expected.EntranceTransform) &&
                SameTransform(Use.UseTransform,Expected.UseTransform) && SameTransform(Use.ExitTransform,Expected.ExitTransform);
        });
    }

    bool FitsPlacement(const ADeliveryCompanionCharacter& Npc,const UDeliveryHomeObjectComponent* Source,
        const FDeliveryHomeUseCandidate& Placement)
    {
        const auto* Definition=Placement.Definition.Get();
        const auto* Prop=Source?Source->GetOwner():nullptr;
        const auto* Body=Prop?Cast<UPrimitiveComponent>(Prop->GetRootComponent()):nullptr;
        const auto* Grab=Prop?Prop->FindComponentByClass<UDeliveryGrabbableComponent>():nullptr;
        const auto* Capsule=Npc.GetCapsuleComponent();
        // 打开 LogDeliveryCompanionHomeChoice Verbose 可看到某件道具为什么没有得到某个摆放槽的候选
        const auto Reject=[&](const TCHAR* Why)
        {
            UE_LOG(LogDeliveryCompanionHomeChoice,Verbose,TEXT("HomePlacementNotOffered prop=%s destination=%s slot=%s reason=%s"),
                *GetNameSafe(Prop),*GetNameSafe(Placement.Object.IsValid()?Placement.Object->GetOwner():nullptr),
                *Placement.SlotId.ToString(),Why);
            return false;
        };
        // 已经拿在二狗自己手里的东西也能换个地方放，这时它归二狗持有、碰撞暂时关着
        const bool bHeldByNpc=Prop && Prop->GetOwner()==&Npc;
        if (!Definition || Placement.Operation!=EDeliveryHomeOperation::PlaceProp || !Prop || !Body || !Grab || !Capsule ||
            !Source->AllowedUses.Contains(EDeliveryHomeOperation::TakeProp) || (Prop->GetOwner() && !bHeldByNpc) ||
            Body->Mobility!=EComponentMobility::Movable || (!Prop->GetActorEnableCollision() && !bHeldByNpc)) return Reject(TEXT("Configuration"));
        if (!Definition->RequiredPropTags.IsEmpty() && !Definition->RequiredPropTags.Matches(Source->ObjectTags)) return Reject(TEXT("PropTags"));
        // 当前搬运接口控制水平朝向，不能向模型提供需要倾斜摆放的槽
        if (FVector::DotProduct(Placement.UseTransform.GetUnitAxis(EAxis::Z),FVector::UpVector)<.9998f) return Reject(TEXT("Tilted"));
        // 拿在手里缩小只影响搬运，摆放候选始终按原尺寸在物件自身坐标中检查
        const FBox Bounds=Source->GetOriginalSizeBounds(FTransform::Identity);
        const FBox Footprint=Source->GetPlacementFootprintBounds(FTransform::Identity);
        if (!Bounds.IsValid || Bounds.Min.ContainsNaN() || Bounds.Max.ContainsNaN() || !Footprint.IsValid) return Reject(TEXT("Bounds"));
        const FVector Extent(Footprint.GetExtent().X,Footprint.GetExtent().Y,Bounds.GetExtent().Z);
        if (Extent.GetMin()<=0 || Extent.X>Definition->MaximumPropHalfExtent.X ||
            Extent.Y>Definition->MaximumPropHalfExtent.Y || Extent.Z>Definition->MaximumPropHalfExtent.Z) return Reject(TEXT("TooLarge"));
        const FQuat Rotation=Placement.UseTransform.GetRotation();
        const FBox Rotated=Source->GetOriginalSizeBounds(FTransform(Rotation));
        const FBox RotatedFootprint=Source->GetPlacementFootprintBounds(FTransform(Rotation));
        if (!Rotated.IsValid || !RotatedFootprint.IsValid) return Reject(TEXT("RotatedBounds"));
        const FVector Offset(RotatedFootprint.GetCenter().X,RotatedFootprint.GetCenter().Y,Rotated.Min.Z);
        const FTransform Target(Rotation,Placement.UseTransform.GetLocation()-Offset,Source->GetOriginalActorScale());
        FVector SmallTarget;
        const FVector Stand=Placement.EntranceTransform.GetLocation()+FVector(0,0,Capsule->GetScaledCapsuleHalfHeight());
        if (!Source->GetCarryPlacementLocation(Target,SmallTarget,true) ||
            !Grab->CanNpcReachPlacement(Npc,Stand,SmallTarget,SmallTarget,SmallTarget)) return Reject(TEXT("HandReach"));
        if (Definition->bUsePropTransferPoint)
        {
            // 中转点与 Use 都由同一实时槽位解释，偏移不继承家具缩放
            const FVector PointFromUse=Definition->UseOffset.InverseTransformPositionNoScale(Definition->PropTransferOffset);
            const FVector Transfer=Placement.UseTransform.TransformPositionNoScale(PointFromUse);
            if (!Grab->CanNpcReachPlacement(Npc,Stand,Transfer,Transfer,Transfer)) return Reject(TEXT("TransferReach"));
        }
        FName Reason;
        if (!Source->CanPlaceAtOriginalSize(Target,Reason,Placement.Object.Get())) return Reject(*Reason.ToString());
        return true;
    }

    struct FPerceivedHomeObject
    {
        UDeliveryHomeObjectComponent* Object=nullptr;
        int32 ViewIndex=INDEX_NONE;
        TArray<FDeliveryHomeUseCandidate> Uses;
    };
}

void UDeliveryCompanionComponent::ExtendHomeDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const
{
    Request.RequestedCapabilityVersion=5;
    Request.HomeObjects.Reset();
    Request.RememberedHomeObjects.Reset();
    auto* Npc=GetCompanion();
    auto* Home=ADeliveryHome::FindSharedHome(this);
    if (!Npc || !Npc->HasAuthority() || !Home) return;
    TArray<UDeliveryHomeObjectComponent*> Objects;
    Home->GetFurnitureObjects(Objects);
    // 眼前看得见的排在前面，清单或组合数到上限时先保留它们
    TSet<const UDeliveryHomeObjectComponent*> SeenNow;
    for (const auto* Object:Objects) if (CanSeeObject(Npc,Object)) SeenNow.Add(Object);
    Objects.StableSort([&SeenNow](const UDeliveryHomeObjectComponent& A,const UDeliveryHomeObjectComponent& B)
    { return SeenNow.Contains(&A) && !SeenNow.Contains(&B); });
    TArray<FPerceivedHomeObject> Perceived;
    const bool bCanArrange=Request.bPlayerRequest && CanCommand(Requester) && Home->IsClaimed() &&
        !Npc->IsKnockedDown() && !IsUsingErrandVehicle() && !GetCarriedItem();
    const auto* HeldSource=bCanArrange?HomeTakeClaim.Object.Get():nullptr;
    const int32 HeldIndex=HeldSource?Objects.IndexOfByKey(HeldSource):INDEX_NONE;
    if (HeldIndex==INDEX_NONE || !HeldSource->IsRegistered() || !OwnsHomeSource(HeldSource)) HeldSource=nullptr;
    const auto* CurrentSeat=bCanArrange && NeedsHomeExit() && HomeState.Phase==EDeliveryCompanionHomePhase::Using?
        HomeUseClaim.Object.Get():nullptr;
    const int32 SeatIndex=CurrentSeat?Objects.IndexOfByKey(CurrentSeat):INDEX_NONE;
    // 手里的实物先入清单，家具多时也保留这件物体的摆放来源
    if (HeldSource && HeldIndex>0) Objects.Swap(0,HeldIndex);
    else if (SeatIndex>0) Objects.Swap(0,SeatIndex);
    for (auto* Object:Objects)
    {
        if (Request.HomeObjects.Num()>=24) break;
        const bool bInHand=Object==HeldSource;
        const bool bCurrentSeat=Object==CurrentSeat;
        const bool bSeenNow=bInHand || bCurrentSeat || SeenNow.Contains(Object);
        // 自己家里的家具不用正对着也知道，主人说到哪件都有对应候选
        if ((!bSeenNow && !KnowsSharedHomeObject(Object)) || SnapshotText(Object->DisplayName.ToString(),64).IsEmpty()) continue;
        FPerceivedHomeObject Seen;
        Seen.Object=Object;
        Seen.ViewIndex=Request.HomeObjects.Num();
        if (bCanArrange && !bInHand) Object->FindAvailableUses(Npc,Requester,Seen.Uses);
        Seen.Uses.RemoveAll([](const auto& Use)
        { return !IsProtocolSlot(Use.SlotId) || !Use.Definition.IsValid() || SnapshotText(Use.Definition->DisplayName.ToString(),64).IsEmpty(); });
        FDeliveryNpcHomeObjectView View;
        View.ObjectId=Object->ObjectId;
        View.Name=SnapshotText(Object->DisplayName.ToString(),64);
        View.State=bInHand?TEXT("正在我手里，属于共用房屋"):bCurrentSeat?TEXT("我正坐在这个座位上，属于共用房屋"):
            !bSeenNow?TEXT("屋里的物件，现在不在视线里，属于共用房屋"):
            Home->IsClaimed()?TEXT("当前看见，属于共用房屋"):TEXT("当前看见；爷爷还住在这里没搬走，房子还没到手，屋里的东西现在都不能搬动或使用，要等主人把爷爷催走、房屋解锁之后");
        const FString Description=SnapshotText(Object->Description.ToString(),100);
        if (!Description.IsEmpty()) View.State+=TEXT("；")+Description;
        if (Object->AllowedUses.Contains(EDeliveryHomeOperation::PlayAudio))
            View.State+=Object->IsAudioPlaying()?TEXT("；正在播放声音"):TEXT("；声音已关闭");
        if (!bInHand)
            if (const auto* Prop=Object->GetOwner()->FindComponentByClass<UDeliveryGrabbableComponent>(); Prop && Prop->HasAnyCarrier())
                View.State+=OwnsHomeSource(Object)?TEXT("；正在我手里"):TEXT("；正在被别人搬运");
        if (Object->IsCarryScaleActive())
            View.State+=Object->GetCarryScaleState().Phase==EDeliveryHomeCarryScalePhase::WaitingToRestore?
                TEXT("；暂时缩小，等空间安全后恢复原大小"):TEXT("；搬运时缩小，摆放需要按原大小留空间");
        for (const auto& Use:Seen.Uses)
        {
            if (View.Slots.Num()>=16) break;
            FDeliveryNpcHomeSlotView Slot;
            Slot.SlotId=Use.SlotId.ToString();
            Slot.Name=SnapshotText(Use.Definition->DisplayName.ToString(),64);
            Slot.bAvailable=true;
            View.Slots.Add(MoveTemp(Slot));
        }
        for (const auto& State:Object->GetUseStates())
        {
            View.State+=State.Phase==EDeliveryHomeUsePhase::Placed?TEXT("；有道具占着摆放位置"):TEXT("；有使用位置正在被占用");
            if (!IsProtocolSlot(State.SlotId) || View.Slots.Num()>=16 ||
                View.Slots.ContainsByPredicate([&](const auto& Slot){return Slot.SlotId==State.SlotId.ToString();})) continue;
            FDeliveryNpcHomeSlotView Slot;
            Slot.SlotId=State.SlotId.ToString(); Slot.Name=TEXT("已占用的位置");
            View.Slots.Add(MoveTemp(Slot));
        }
        View.State=SnapshotText(View.State,256);
        Request.HomeObjects.Add(MoveTemp(View));
        Perceived.Add(MoveTemp(Seen));
    }
    CopyHomeObjectMemories(*Home,Request);
    if (!bCanArrange) return;
    int32 HomeChoiceCount=0;
    auto AddChoice=[&](const FPerceivedHomeObject& Source,FDeliveryCompanionHomeChoice Choice)
    {
        if (HomeChoiceCount>=64) return;
        Choice.SourceObject=Source.Object; Choice.SourceId=Source.Object->ObjectId;
        Choice.SourceRevision=Source.Object->GetRevision();
        FDeliveryNpcCandidate Candidate;
        Candidate.Action=EDeliveryNpcAction::HomeAction;
        Candidate.HomeChoice=MakeShared<FDeliveryCompanionHomeChoice>(MoveTemp(Choice));
        Request.AllowedActions.AddUnique(Candidate.Action);
        Request.HomeObjects[Source.ViewIndex].Uses.AddUnique(
            StaticEnum<EDeliveryHomeOperation>()->GetNameStringByValue(static_cast<int64>(Candidate.HomeChoice->Operation)));
        Request.Candidates.Add(MoveTemp(Candidate));
        ++HomeChoiceCount;
    };
    for (const auto& Source:Perceived)
    {
        if (Source.Object==CurrentSeat)
        {
            FName Reason;
            if (Source.Object->ValidateClaim(HomeUseClaim,false,Reason))
            {
                FDeliveryCompanionHomeChoice Choice;
                Choice.Operation=EDeliveryHomeOperation::LeaveSeat;
                Choice.SeatArrangementId=HomeState.ArrangementId;
                AddChoice(Source,MoveTemp(Choice));
            }
        }
        TSet<EDeliveryHomeOperation> Offered;
        for (const auto& Use:Source.Uses)
        {
            if (Use.Operation==EDeliveryHomeOperation::PlaceProp || Offered.Contains(Use.Operation)) continue;
            if (GetHomeState().CarriedProp && (Use.Operation==EDeliveryHomeOperation::Sit || Use.Operation==EDeliveryHomeOperation::TakeProp)) continue;
            FDeliveryCompanionHomeChoice Choice;
            Choice.Operation=Use.Operation; Choice.Use=Use;
            AddChoice(Source,MoveTemp(Choice));
            Offered.Add(Use.Operation);
        }
        const auto* Take=Source.Uses.FindByPredicate([](const auto& Use){return Use.Operation==EDeliveryHomeOperation::TakeProp;});
        const bool bAlreadyCarried=Source.Object==HeldSource;
        if ((!Take && !bAlreadyCarried) || (GetHomeState().CarriedProp && !bAlreadyCarried)) continue;
        auto* OwnerPawn=GetOwnerPawn();
        FTransform Entrance,Placement;
        // 能拿的东西都可以请二狗放到主人身边；主人面前暂时摆不下时，执行中会走到主人旁边放在脚边
        if (IsValid(OwnerPawn))
        {
            FDeliveryCompanionHomeChoice Choice;
            Choice.Operation=EDeliveryHomeOperation::PlaceNearOwner;
            Choice.PlacementOwnerPawn=OwnerPawn;
            if (!bAlreadyCarried) Choice.TakeUse=*Take;
            AddChoice(Source,MoveTemp(Choice));
        }
    }
    // 先保留每个物件的拿放动作，再追加有净空的物件对物件组合
    for (const auto& Source:Perceived)
    {
        const auto* Take=Source.Uses.FindByPredicate([](const auto& Use){return Use.Operation==EDeliveryHomeOperation::TakeProp;});
        const bool bAlreadyCarried=Source.Object==HeldSource;
        if ((!Take && !bAlreadyCarried) || (GetHomeState().CarriedProp && !bAlreadyCarried)) continue;
        for (const auto& Destination:Perceived)
        {
            if (HomeChoiceCount>=64) return;
            if (Source.Object==Destination.Object) continue;
            for (const auto& Place:Destination.Uses)
            {
                if (!FitsPlacement(*Npc,Source.Object,Place)) continue;
                // 目的槽必须包含在同一份模型清单里，不能提供被截断的隐藏位置
                if (!Request.HomeObjects[Destination.ViewIndex].Slots.ContainsByPredicate([&](const auto& Slot)
                    {return Slot.bAvailable && Slot.SlotId==Place.SlotId.ToString();})) continue;
                FDeliveryCompanionHomeChoice Choice;
                Choice.Operation=EDeliveryHomeOperation::PlaceProp; Choice.Use=Place;
                if (!bAlreadyCarried) Choice.TakeUse=*Take;
                Choice.DestinationObject=Destination.Object; Choice.DestinationId=Destination.Object->ObjectId;
                Choice.DestinationRevision=Destination.Object->GetRevision(); Choice.DestinationSlotId=Place.SlotId;
                AddChoice(Source,MoveTemp(Choice));
            }
        }
    }
}

bool UDeliveryCompanionComponent::CanApplyHomeChoice(const FDeliveryCompanionHomeChoice& Choice,APlayerState* Requester,int32 ExpectedRevision) const
{
    auto* Npc=GetCompanion();
    auto* Home=ADeliveryHome::FindSharedHome(this);
    auto* Source=Choice.SourceObject.Get();
    if (!Npc || !Npc->HasAuthority() || !CanCommand(Requester) || Choice.Requester.Get()!=Requester ||
        CommandRevision!=ExpectedRevision || !Choice.CandidateId.IsValid() || !Choice.RequestId.IsValid() ||
        Npc->IsKnockedDown() || IsUsingErrandVehicle() || GetCarriedItem() || !Home || !Home->IsClaimed() || !Source || !Source->IsRegistered() ||
        Source->Home!=Home || !Home->ContainsFurniture(Source) || Source->ObjectId!=Choice.SourceId ||
        Source->GetRevision()!=Choice.SourceRevision) return false;
    if (Choice.Operation==EDeliveryHomeOperation::LeaveSeat)
    {
        FName Reason;
        return !Choice.DestinationObject.IsValid() && !Choice.DestinationId.IsValid() && Choice.DestinationSlotId.IsNone() &&
            Choice.SeatArrangementId==HomeState.ArrangementId && HomeState.Phase==EDeliveryCompanionHomePhase::Using &&
            NeedsHomeExit() && HomeUseClaim.Object.Get()==Source && HomeState.Object==Source->GetOwner() &&
            Source->ValidateClaim(HomeUseClaim,false,Reason);
    }
    const bool bPlace=Choice.Operation==EDeliveryHomeOperation::PlaceProp || Choice.Operation==EDeliveryHomeOperation::PlaceNearOwner;
    const bool bPlaceFromHand=bPlace && OwnsHomeSource(Source);
    if (!bPlaceFromHand && !CanSeeObject(Npc,Source) && !KnowsSharedHomeObject(Source)) return false;
    if (Choice.Operation==EDeliveryHomeOperation::PlaceNearOwner)
    {
        FTransform Entrance,Placement;
        if (Choice.DestinationObject.IsValid() || Choice.DestinationId.IsValid() || !Choice.DestinationSlotId.IsNone()) return false;
        // 已经拿在手里的东西总可以请二狗放下，主人面前暂时没有空位时执行中会改放在脚边
        if (bPlaceFromHand) return GetHomeState().CarriedProp==Source->GetOwner() && IsValid(Choice.PlacementOwnerPawn.Get());
        if (!IsValid(Choice.PlacementOwnerPawn.Get())) return false;
        return !GetHomeState().CarriedProp && Choice.TakeUse.Object.Get()==Source &&
            Choice.TakeUse.Operation==EDeliveryHomeOperation::TakeProp && StillOffered(Choice.TakeUse,Npc,Requester);
    }
    if (Choice.Operation==EDeliveryHomeOperation::PlaceProp)
    {
        auto* Destination=Choice.DestinationObject.Get();
        if (!Destination || Destination==Source || Destination->Home!=Home || !Home->ContainsFurniture(Destination) ||
            Destination->ObjectId!=Choice.DestinationId || Destination->GetRevision()!=Choice.DestinationRevision ||
            (!CanSeeObject(Npc,Destination) && !KnowsSharedHomeObject(Destination)) || Choice.Use.Object.Get()!=Destination || Choice.Use.Operation!=Choice.Operation ||
            Choice.Use.SlotId!=Choice.DestinationSlotId || !FitsPlacement(*Npc,Source,Choice.Use) ||
            !StillOffered(Choice.Use,Npc,Requester)) return false;
        if (bPlaceFromHand) return GetHomeState().CarriedProp==Source->GetOwner();
        return !GetHomeState().CarriedProp && Choice.TakeUse.Object.Get()==Source &&
            Choice.TakeUse.Operation==EDeliveryHomeOperation::TakeProp && StillOffered(Choice.TakeUse,Npc,Requester);
    }
    if (Choice.DestinationObject.IsValid() || Choice.DestinationId.IsValid() || !Choice.DestinationSlotId.IsNone() ||
        Choice.Use.Object.Get()!=Source || Choice.Use.Operation!=Choice.Operation ||
        Choice.Operation==EDeliveryHomeOperation::None) return false;
    if (GetHomeState().CarriedProp && (Choice.Operation==EDeliveryHomeOperation::Sit || Choice.Operation==EDeliveryHomeOperation::TakeProp)) return false;
    return StillOffered(Choice.Use,Npc,Requester);
}

bool UDeliveryCompanionComponent::KnowsSharedHomeObject(const UDeliveryHomeObjectComponent* Object) const
{
    const auto* Npc=GetCompanion();
    const auto* Home=ADeliveryHome::FindSharedHome(this);
    const auto* Actor=IsValid(Object)?Object->GetOwner():nullptr;
    FVector Interior;
    if (!IsValid(Npc) || !IsValid(Home) || !Home->IsClaimed() || !IsValid(Actor) || Actor->IsHidden() ||
        Actor->IsActorBeingDestroyed() || !Object->IsRegistered() || Object->Home.Get()!=Home || !Home->ContainsFurniture(Object) ||
        !FMath::IsFinite(Home->ObjectSightRadius) || !Home->TryGetInteriorLocation(Interior)) return false;
    // 人要在屋里或门口附近，走远以后只留名称记忆，不能隔着街安排家里的东西
    return FVector::DistSquared(Npc->GetActorLocation(),Interior)<=
        FMath::Square(FMath::Clamp(Home->ObjectSightRadius,100.f,3000.f)+600.f);
}

void UDeliveryCompanionComponent::RefreshHomeObjectMemories()
{
    constexpr double ObservationInterval=2.0;
    constexpr int32 MaxRememberedObjects=12;
    constexpr int32 MaxVisibilityChecks=24;
    const auto* Npc=GetCompanion();
    const auto* World=GetWorld();
    if (!IsValid(Npc) || !Npc->HasAuthority() || !World || !CanCommand(OwnerPlayerState) ||
        Npc->GetEncounterState().Phase!=EDeliveryCompanionEncounterPhase::Joined) return;
    const double Now=World->GetTimeSeconds();
    if (!FMath::IsFinite(Now) || Now<NextHomeObjectObservationAt) return;
    NextHomeObjectObservationAt=Now+ObservationInterval;

    auto* Home=ADeliveryHome::FindSharedHome(this);
    if (!IsValid(Home) || Home->IsActorBeingDestroyed() || !Home->IsClaimed())
    {
        ObservedHome.Reset();
        RememberedHomeObjects.Reset();
        return;
    }
    if (ObservedHome.Get()!=Home)
    {
        ObservedHome=Home;
        RememberedHomeObjects.Reset();
    }
    FVector Interior;
    if (Npc->IsHidden() || Npc->IsKnockedDown() || !FMath::IsFinite(Home->ObjectSightRadius) ||
        !Home->TryGetInteriorLocation(Interior) ||
        FVector::DistSquared(Npc->GetActorLocation(),Interior)>
        FMath::Square(FMath::Clamp(Home->ObjectSightRadius,100.f,3000.f)+600.f)) return;

    TArray<UDeliveryHomeObjectComponent*> Furniture;
    Home->GetFurnitureObjects(Furniture);
    for (int32 Index=0;Index<FMath::Min(Furniture.Num(),MaxVisibilityChecks);++Index)
    {
        const auto* Object=Furniture[Index];
        if (!CanSeeObject(Npc,Object)) continue;
        const FString Name=SnapshotText(Object->DisplayName.ToString(),64);
        if (Name.IsEmpty()) continue;
        // 只记物件资料，状态变化和操作结果仍依赖本轮实见及服务器确认
        if (RememberedHomeObjects.Num()>=MaxRememberedObjects && !RememberedHomeObjects.Contains(Object->ObjectId))
        {
            FGuid Oldest;
            double OldestTime=TNumericLimits<double>::Max();
            for (const auto& Entry:RememberedHomeObjects)
                if (Entry.Value.ObservedAt<OldestTime)
                { Oldest=Entry.Key; OldestTime=Entry.Value.ObservedAt; }
            RememberedHomeObjects.Remove(Oldest);
        }
        auto& Memory=RememberedHomeObjects.FindOrAdd(Object->ObjectId);
        Memory.View.Name=Name;
        Memory.View.Description=SnapshotText(Object->Description.ToString(),100);
        Memory.ObservedAt=Now;
    }
}

void UDeliveryCompanionComponent::CopyHomeObjectMemories(const ADeliveryHome& Home,FDeliveryNpcDecisionRequest& Request) const
{
    constexpr int32 MaxRequestMemories=8;
    if (ObservedHome.Get()!=&Home || !Home.IsClaimed() || Home.IsActorBeingDestroyed()) return;
    TSet<FGuid> Present;
    for (const auto& Object:Request.HomeObjects) Present.Add(Object.ObjectId);
    TArray<const FRememberedHomeObject*,TInlineAllocator<12>> Earlier;
    for (const auto& Entry:RememberedHomeObjects)
        if (!Present.Contains(Entry.Key)) Earlier.Add(&Entry.Value);
    Earlier.Sort([](const FRememberedHomeObject& A,const FRememberedHomeObject& B)
    {
        if (A.ObservedAt!=B.ObservedAt) return A.ObservedAt>B.ObservedAt;
        return A.View.Name<B.View.Name;
    });
    for (int32 Index=0;Index<FMath::Min(Earlier.Num(),MaxRequestMemories);++Index)
        Request.RememberedHomeObjects.Add(Earlier[Index]->View);
}
