#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "World/DeliveryHome.h"
#include "World/DeliveryHomeObjectComponent.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "Components/PrimitiveComponent.h"
#include "AIController.h"
#include "AIHelpers.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISense_Sight.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"

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

    bool FitsPlacement(const UDeliveryHomeObjectComponent* Source,const FDeliveryHomeUseCandidate& Placement)
    {
        const auto* Definition=Placement.Definition.Get();
        const auto* Prop=Source?Source->GetOwner():nullptr;
        const auto* Body=Prop?Cast<UPrimitiveComponent>(Prop->GetRootComponent()):nullptr;
        if (!Definition || Placement.Operation!=EDeliveryHomeOperation::PlaceProp || !Prop || !Body ||
            !Source->AllowedUses.Contains(EDeliveryHomeOperation::TakeProp) || Prop->GetOwner() ||
            Body->Mobility!=EComponentMobility::Movable || !Prop->GetActorEnableCollision() ||
            !Prop->FindComponentByClass<UDeliveryGrabbableComponent>()) return false;
        if (!Definition->RequiredPropTags.IsEmpty() && !Definition->RequiredPropTags.Matches(Source->ObjectTags)) return false;
        // 当前搬运接口控制水平朝向，不能向模型提供需要倾斜摆放的槽
        if (FVector::DotProduct(Placement.UseTransform.GetUnitAxis(EAxis::Z),FVector::UpVector)<.9998f) return false;
        // 拿在手里缩小只影响搬运，摆放候选始终按原尺寸在物件自身坐标中检查
        const FBox Bounds=Source->GetOriginalSizeBounds(FTransform::Identity);
        if (!Bounds.IsValid || Bounds.Min.ContainsNaN() || Bounds.Max.ContainsNaN()) return false;
        const FVector Extent=Bounds.GetExtent();
        return Extent.GetMin()>0 && Extent.X<=Definition->MaximumPropHalfExtent.X &&
            Extent.Y<=Definition->MaximumPropHalfExtent.Y && Extent.Z<=Definition->MaximumPropHalfExtent.Z;
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
    auto* Npc=GetCompanion();
    auto* Home=ADeliveryHome::FindSharedHome(this);
    if (!Npc || !Npc->HasAuthority() || !Home) return;
    TArray<UDeliveryHomeObjectComponent*> Objects;
    Home->GetFurnitureObjects(Objects);
    TArray<FPerceivedHomeObject> Perceived;
    const bool bCanArrange=Request.bPlayerRequest && CanCommand(Requester) && Home->IsClaimed() &&
        !Npc->IsKnockedDown() && !IsUsingErrandVehicle() && !GetCarriedItem();
    for (auto* Object:Objects)
    {
        if (Request.HomeObjects.Num()>=24) break;
        if (!CanSeeObject(Npc,Object) || SnapshotText(Object->DisplayName.ToString(),64).IsEmpty()) continue;
        FPerceivedHomeObject Seen;
        Seen.Object=Object;
        Seen.ViewIndex=Request.HomeObjects.Num();
        if (bCanArrange) Object->FindAvailableUses(Npc,Requester,Seen.Uses);
        Seen.Uses.RemoveAll([](const auto& Use)
        { return !IsProtocolSlot(Use.SlotId) || !Use.Definition.IsValid() || SnapshotText(Use.Definition->DisplayName.ToString(),64).IsEmpty(); });
        FDeliveryNpcHomeObjectView View;
        View.ObjectId=Object->ObjectId;
        View.Name=SnapshotText(Object->DisplayName.ToString(),64);
        View.State=Home->IsClaimed()?TEXT("当前看见，属于共用房屋"):TEXT("当前看见，爷爷尚未搬离，不能安排使用");
        const FString Description=SnapshotText(Object->Description.ToString(),100);
        if (!Description.IsEmpty()) View.State+=TEXT("；")+Description;
        if (Object->AllowedUses.Contains(EDeliveryHomeOperation::PlayAudio))
            View.State+=Object->IsAudioPlaying()?TEXT("；正在播放声音"):TEXT("；声音已关闭");
        if (const auto* Prop=Object->GetOwner()->FindComponentByClass<UDeliveryGrabbableComponent>())
            if (Prop->HasAnyCarrier()) View.State+=OwnsHomeSource(Object)?TEXT("；正在我手里"):TEXT("；正在被别人搬运");
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
        const bool bAlreadyCarried=OwnsHomeSource(Source.Object);
        if ((!Take && !bAlreadyCarried) || (GetHomeState().CarriedProp && !bAlreadyCarried)) continue;
        for (const auto& Destination:Perceived)
        {
            if (Source.Object==Destination.Object) continue;
            for (const auto& Place:Destination.Uses)
            {
                if (!FitsPlacement(Source.Object,Place)) continue;
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
        Npc->IsKnockedDown() || IsUsingErrandVehicle() || GetCarriedItem() || !Home || !Home->IsClaimed() || !Source ||
        Source->Home!=Home || !Home->ContainsFurniture(Source) || Source->ObjectId!=Choice.SourceId ||
        Source->GetRevision()!=Choice.SourceRevision || !CanSeeObject(Npc,Source)) return false;
    if (Choice.Operation==EDeliveryHomeOperation::PlaceProp)
    {
        auto* Destination=Choice.DestinationObject.Get();
        if (!Destination || Destination==Source || Destination->Home!=Home || !Home->ContainsFurniture(Destination) ||
            Destination->ObjectId!=Choice.DestinationId || Destination->GetRevision()!=Choice.DestinationRevision ||
            !CanSeeObject(Npc,Destination) || Choice.Use.Object.Get()!=Destination || Choice.Use.Operation!=Choice.Operation ||
            Choice.Use.SlotId!=Choice.DestinationSlotId || !FitsPlacement(Source,Choice.Use) ||
            !StillOffered(Choice.Use,Npc,Requester)) return false;
        if (OwnsHomeSource(Source)) return GetHomeState().CarriedProp==Source->GetOwner();
        return !GetHomeState().CarriedProp && Choice.TakeUse.Object.Get()==Source &&
            Choice.TakeUse.Operation==EDeliveryHomeOperation::TakeProp && StillOffered(Choice.TakeUse,Npc,Requester);
    }
    if (Choice.DestinationObject.IsValid() || Choice.DestinationId.IsValid() || !Choice.DestinationSlotId.IsNone() ||
        Choice.Use.Object.Get()!=Source || Choice.Use.Operation!=Choice.Operation ||
        Choice.Operation==EDeliveryHomeOperation::None) return false;
    if (GetHomeState().CarriedProp && (Choice.Operation==EDeliveryHomeOperation::Sit || Choice.Operation==EDeliveryHomeOperation::TakeProp)) return false;
    return StillOffered(Choice.Use,Npc,Requester);
}
