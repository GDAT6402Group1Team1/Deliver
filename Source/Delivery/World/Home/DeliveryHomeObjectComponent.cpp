#include "World/Home/DeliveryHomeObjectComponent.h"
#include "World/Home/DeliveryHome.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "Inventory/Items/DeliveryPackageItem.h"
#include "Components/AudioComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/CapsuleComponent.h"
#include "SmartObjectComponent.h"
#include "SmartObjectDefinition.h"
#include "SmartObjectSubsystem.h"
#include "Sound/SoundBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "UObject/UObjectIterator.h"
#include "Async/Async.h"
#include "Net/UnrealNetwork.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeliveryHomeObject,Log,All);

namespace
{
    bool Fail(FName& OutReason,const TCHAR* Reason) { OutReason=Reason; return false; }

    FCollisionObjectQueryParams ScaleCollisionObjects()
    {
        FCollisionObjectQueryParams Objects;
        for (const ECollisionChannel Channel:{ECC_WorldStatic,ECC_WorldDynamic,ECC_Pawn,ECC_PhysicsBody,ECC_Vehicle,ECC_Destructible})
            Objects.AddObjectTypesToQuery(Channel);
        return Objects;
    }

    bool ValidScale(const FVector& Scale)
    {
        return !Scale.ContainsNaN() && Scale.GetAbs().GetMin()>.001 && Scale.GetAbs().GetMax()<100000;
    }

    bool BlocksScaleExpansion(const UPrimitiveComponent* Other,ECollisionChannel PropChannel)
    {
        return IsValid(Other) && (Other->GetCollisionObjectType()==ECC_Pawn || Other->GetCollisionObjectType()==ECC_Vehicle ||
            Other->IsPhysicsCollisionEnabled() || Other->GetCollisionResponseToChannel(PropChannel)==ECR_Block);
    }

    bool HasClaimToken(const USmartObjectSubsystem* Subsystem,const FSmartObjectSlotHandle& Slot,FGuid Token)
    {
        if (!Subsystem || !Slot.IsValid() || !Token.IsValid()) return false;
        bool bOurs=false;
        Subsystem->ReadSlotData(Slot,[&](FConstSmartObjectSlotView View)
        {
            if (!View.IsValid()) return;
            const auto* Data=View.GetUserData().GetPtr<FDeliveryHomeSlotUserData>();
            bOurs=Data && Data->Token==Token &&
                (View.GetState()==ESmartObjectSlotState::Claimed || View.GetState()==ESmartObjectSlotState::Occupied);
        });
        return bOurs;
    }

    void MakeUseTransforms(const UDeliveryHomeBehaviorDefinition& Definition,const FTransform& Slot,
        FTransform& Entrance,FTransform& Use,FTransform& Exit)
    {
        // 家具自身缩放不传给人物，附加偏移按世界厘米解释
        const FTransform Frame(Slot.GetRotation(),Slot.GetLocation(),FVector::OneVector);
        Entrance=Definition.EntranceOffset*Frame;
        Use=Definition.UseOffset*Frame;
        Exit=Definition.ExitOffset*Frame;
        Entrance.SetScale3D(FVector::OneVector); Use.SetScale3D(FVector::OneVector); Exit.SetScale3D(FVector::OneVector);
    }
}

UDeliveryHomeObjectComponent::UDeliveryHomeObjectComponent()
{
    SetIsReplicatedByDefault(true);
    PrimaryComponentTick.bCanEverTick=true;
    PrimaryComponentTick.TickInterval=.25f;
}

void UDeliveryHomeObjectComponent::BeginPlay()
{
    Super::BeginPlay();
    if (!GetOwner()->HasAuthority()) SetComponentTickEnabled(false);
    OnRep_Audio();
}

USmartObjectComponent* UDeliveryHomeObjectComponent::ResolveSmartObject() const
{
    auto* Component=Cast<USmartObjectComponent>(SmartObjectReference.GetComponent(GetOwner()));
    return IsValid(Component) && Component->GetOwner()==GetOwner() && Component->GetWorld()==GetWorld()?Component:nullptr;
}

UAudioComponent* UDeliveryHomeObjectComponent::ResolveAudio() const
{
    auto* Component=Cast<UAudioComponent>(AudioReference.GetComponent(GetOwner()));
    return IsValid(Component) && Component->GetOwner()==GetOwner() && Component->GetWorld()==GetWorld()?Component:nullptr;
}

bool UDeliveryHomeObjectComponent::ValidateHome(FName& OutReason,bool bRequireRuntimeHome) const
{
    if (!IsInGameThread() || bEndingPlay || !GetWorld() || !IsValid(GetOwner()) || !GetOwner()->HasAuthority() ||
        GetOwner()->IsActorBeingDestroyed() || GetOwner()->IsHidden()) return Fail(OutReason,TEXT("HomeObjectUnavailable"));
    const FBox ObjectBounds=GetOwner()->GetComponentsBoundingBox(true);
    if (!ObjectBounds.IsValid || ObjectBounds.GetExtent().GetMax()<=0) return Fail(OutReason,TEXT("HomeObjectGeometryMissing"));
    if (!IsValid(Home) || Home->IsActorBeingDestroyed() || Home->GetWorld()!=GetWorld() ||
        (bRequireRuntimeHome && (ADeliveryHome::FindSharedHome(this)!=Home || !Home->IsClaimed())))
        return Fail(OutReason,TEXT("HomeNotClaimed"));
    if (!ObjectId.IsValid() || DisplayName.IsEmpty() || !Home->ContainsFurniture(this))
        return Fail(OutReason,TEXT("HomeObjectIdentityMissing"));
    // 只核对同 World 的实例，重复持久 ID 不随机选择其中一个
    for (TObjectIterator<UDeliveryHomeObjectComponent> It;It;++It)
        if (*It!=this && !It->IsTemplate() && It->GetWorld()==GetWorld() && IsValid(It->GetOwner()) &&
            !It->GetOwner()->IsActorBeingDestroyed() && It->ObjectId==ObjectId)
            return Fail(OutReason,TEXT("DuplicateHomeObjectId"));
    OutReason=NAME_None;
    return true;
}

bool UDeliveryHomeObjectComponent::ValidateUser(ADeliveryCompanionCharacter* Companion,APlayerState* Requester,FName& OutReason) const
{
    if (!ValidateHome(OutReason)) return false;
    if (!IsValid(Companion) || Companion->IsActorBeingDestroyed() || Companion->GetWorld()!=GetWorld() ||
        !Companion->HasAuthority() || !Companion->Companion || !IsValid(Requester) || Requester->GetWorld()!=GetWorld() ||
        !Companion->Companion->CanCommand(Requester)) return Fail(OutReason,TEXT("HomeRequesterNotOwner"));
    if (Companion->IsKnockedDown() || Companion->IsHidden() || Companion->Companion->IsUsingErrandVehicle())
        return Fail(OutReason,TEXT("HomeUserUnavailable"));
    return true;
}

bool UDeliveryHomeObjectComponent::IsOperationAvailable(const UDeliveryHomeBehaviorDefinition* Definition) const
{
    if (!Definition || !Definition->IsUsableConfiguration() || !AllowedUses.Contains(Definition->Operation)) return false;
    const bool bTaking=Definition->Operation==EDeliveryHomeOperation::TakeProp;
    for (const auto& Pair:OwnedUses)
    {
        const auto* Active=Pair.Value.Definition.Get();
        if (Active && (Active->Operation==EDeliveryHomeOperation::TakeProp)!=bTaking && IsCurrentSmartObjectClaim(Pair.Value))
            return false;
    }
    const auto* Grabbable=GetOwner()->FindComponentByClass<UDeliveryGrabbableComponent>();
    if (IsCarryScaleActive() || (Grabbable && Grabbable->HasAnyCarrier())) return false;
    if (Definition->Operation==EDeliveryHomeOperation::PlayAudio) return !bAudioPlaying && ResolveAudio()!=nullptr;
    if (Definition->Operation==EDeliveryHomeOperation::StopAudio) return bAudioPlaying && ResolveAudio()!=nullptr;
    if (bTaking)
    {
        const auto* Body=Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent());
        return Grabbable && !GetOwner()->GetOwner() && Body &&
            Body->Mobility==EComponentMobility::Movable && Body->IsSimulatingPhysics();
    }
    if (Definition->Operation==EDeliveryHomeOperation::PlaceProp) return IsStablePlacementSurface();
    return true;
}

bool UDeliveryHomeObjectComponent::ValidateUse(ADeliveryCompanionCharacter* Companion,
    const UDeliveryHomeBehaviorDefinition* Definition,const FSmartObjectSlotHandle& Slot,bool bClose,FName& OutReason,
    const AActor* PlacementProp,bool bHeldByUser) const
{
    auto* Subsystem=GetWorld()?GetWorld()->GetSubsystem<USmartObjectSubsystem>():nullptr;
    auto* Smart=ResolveSmartObject();
    if (!Subsystem || !Smart || !Slot.IsValid() || Slot.GetSmartObjectHandle()!=Smart->GetRegisteredHandle() ||
        !Subsystem->IsSmartObjectSlotValid(Slot) || !Definition || !Definition->IsUsableConfiguration() ||
        !AllowedUses.Contains(Definition->Operation)) return Fail(OutReason,TEXT("HomeSlotUnavailable"));
    if (Definition->Operation!=EDeliveryHomeOperation::TakeProp)
    {
        const auto* Grab=GetOwner()->FindComponentByClass<UDeliveryGrabbableComponent>();
        if (IsCarryScaleActive() || (Grab && Grab->HasAnyCarrier())) return Fail(OutReason,TEXT("HomePropAlreadyCarried"));
    }
    FTransform Entrance,Use,Exit;
    if (!ResolveUseTransforms(Companion,Definition,Slot,Entrance,Use,Exit,OutReason)) return false;
    FVector Point=Use.TransformPositionNoScale(Definition->SightOffset);
    if (bClose && Definition->Operation==EDeliveryHomeOperation::TakeProp)
        if (const auto* Grab=GetOwner()->FindComponentByClass<UDeliveryGrabbableComponent>(); Grab && Grab->bAllowNpcEdgePickup &&
            !Grab->HasAnyCarrier() && !Grab->FindNpcPickupContact(Companion->GetActorLocation(),Companion,Point))
            return Fail(OutReason,TEXT("HomeTakeEntranceTooFar"));
    const float Radius=bClose?Definition->UseRadius:Definition->ApproachRadius;
    // 拿取距离在登记搬运前已按边缘接触点通过；登记后不再改用物件中心重量一次，大件抱枕的中心会超出拿取半径
    if (!(bClose && bHeldByUser) && FVector::DistSquared(Companion->GetActorLocation(),Point)>FMath::Square(Radius))
        return Fail(OutReason,TEXT("HomeObjectTooFar"));
    FCollisionQueryParams Params(SCENE_QUERY_STAT(HomeObjectSight),false,Companion);
    // 原物盖住支撑点后仍需验证家具视线，其余物件和墙继续挡射线
    if (Definition->Operation==EDeliveryHomeOperation::PlaceProp && IsValid(PlacementProp) && !PlacementProp->IsActorBeingDestroyed())
        Params.AddIgnoredActor(PlacementProp);
    FHitResult Hit;
    // 同伴熟悉自己家，隔着家具或背对时也能安排；走到使用点后才要求真的看得见、够得着
    if (bClose && GetWorld()->LineTraceSingleByChannel(Hit,Companion->GetPawnViewLocation(),Point,ECC_Visibility,Params) &&
        Hit.GetActor()!=GetOwner()) return Fail(OutReason,TEXT("HomeObjectOccluded"));
    if (!Definition->bAllowWithTaskPackage && Companion->Companion->GetCarriedTaskPackage())
        return Fail(OutReason,TEXT("HomeUserCarryingPackage"));
    OutReason=NAME_None;
    return true;
}

bool UDeliveryHomeObjectComponent::ResolveTakeStandingPoint(ADeliveryCompanionCharacter* Companion,
    const UDeliveryHomeBehaviorDefinition& Definition,const FTransform& Authored,const FTransform& Use,
    bool bProjectNearby,FTransform& OutStanding,FName& OutReason) const
{
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    const auto* Capsule=Companion?Companion->GetCapsuleComponent():nullptr;
    const auto* Movement=Companion?Companion->GetCharacterMovement():nullptr;
    FVector Interior;
    if (!Nav || !Capsule || !Movement || !IsValid(Home) || !IsValid(Home->ExistingHouse) ||
        Home->ExistingHouse->IsActorBeingDestroyed() || Home->ExistingHouse->GetWorld()!=GetWorld() ||
        !Home->TryGetInteriorLocation(Interior) || Authored.ContainsNaN() || Use.ContainsNaN())
        return Fail(OutReason,TEXT("HomeTakeGroundUnavailable"));
    const auto& Properties=Companion->GetNavAgentPropertiesRef();
    const FVector Start=Companion->GetNavAgentLocation();
    const auto* NavData=Nav->GetNavDataForProps(Properties,Start);
    if (!NavData || Start.ContainsNaN()) return Fail(OutReason,TEXT("HomeTakeNavigationUnavailable"));
    FVector Seed=Authored.GetLocation();
    // 只在作者入口附近找本屋同层地面，不围绕实物搜索其他房间或整张地图
    if (bProjectNearby) Seed.Z=Interior.Z;
    FNavLocation Projected;
    if (!Nav->ProjectPointToNavigation(Seed,Projected,bProjectNearby?FVector(35,35,100):FVector(15,15,30),NavData) ||
        FVector::Dist2D(Projected.Location,Authored.GetLocation())>(bProjectNearby?35.:15.) ||
        FMath::Abs(Projected.Location.Z-Seed.Z)>(bProjectNearby?100.:15.))
        return Fail(OutReason,TEXT("HomeTakeEntranceOffNavigation"));
    // 已标定入口保留其 XY，投影只用于确认导航；无摆放记录时才采用附近投影点
    const FVector Probe=bProjectNearby?Projected.Location:Authored.GetLocation();
    FCollisionQueryParams GroundQuery(SCENE_QUERY_STAT(HomeTakeGround),false,Companion);
    FHitResult Ground;
    if (!GetWorld()->LineTraceSingleByChannel(Ground,Probe+FVector(0,0,50),Probe-FVector(0,0,100),ECC_Visibility,GroundQuery) ||
        Ground.bStartPenetrating || Ground.GetActor()!=Home->ExistingHouse || !Movement->IsWalkable(Ground) ||
        !Ground.GetComponent() || Ground.GetComponent()->IsSimulatingPhysics() ||
        FMath::Abs(Ground.ImpactPoint.Z-Projected.Location.Z)>15.)
        return Fail(OutReason,TEXT("HomeTakeEntranceUnsupported"));
    const FVector Feet=Ground.ImpactPoint;
    const float HalfHeight=Capsule->GetScaledCapsuleHalfHeight();
    const float Radius=Capsule->GetScaledCapsuleRadius();
    if (!FMath::IsFinite(HalfHeight) || !FMath::IsFinite(Radius) || Radius<=0 || HalfHeight<Radius)
        return Fail(OutReason,TEXT("HomeTakeCapsuleUnavailable"));
    const FVector Center=Feet+FVector(0,0,HalfHeight);
    FVector UsePoint=Use.TransformPositionNoScale(Definition.SightOffset);
    const auto* Grab=GetOwner()->FindComponentByClass<UDeliveryGrabbableComponent>();
    const bool bEdge=Grab && Grab->bAllowNpcEdgePickup;
    if ((bEdge && !Grab->FindNpcPickupContact(Center,Companion,UsePoint)) ||
        (!bEdge && FVector::DistSquared(Center,GetOwner()->GetActorLocation())>FMath::Square(200.)) ||
        FVector::DistSquared(Center,UsePoint)>FMath::Square(FMath::Min(200.,static_cast<double>(Definition.UseRadius))))
        return Fail(OutReason,TEXT("HomeTakeEntranceTooFar"));
    TArray<FOverlapResult> Overlaps;
    GetWorld()->OverlapMultiByObjectType(Overlaps,Center+FVector(0,0,2),FQuat::Identity,ScaleCollisionObjects(),
        FCollisionShape::MakeCapsule(Radius,HalfHeight),GroundQuery);
    for (const auto& Overlap:Overlaps)
        if (BlocksScaleExpansion(Overlap.GetComponent(),ECC_Pawn))
            return Fail(OutReason,TEXT("HomeTakeEntranceBlocked"));
    const FVector Eye=Center+(Companion->GetPawnViewLocation()-Companion->GetActorLocation());
    FHitResult Sight;
    if (GetWorld()->LineTraceSingleByChannel(Sight,Eye,UsePoint,ECC_Visibility,GroundQuery) && Sight.GetActor()!=GetOwner())
        return Fail(OutReason,TEXT("HomeTakeEntranceOccluded"));
    // 搬运登记还会检查人物中心到物件的真实连线，提前排除隔桌面或墙可见却够不到的入口
    FCollisionQueryParams ReachQuery=GroundQuery;
    ReachQuery.AddIgnoredActor(GetOwner());
    const FBox Bounds=GetOwner()->GetComponentsBoundingBox(true);
    if (!Bounds.IsValid || (!bEdge &&
        (GetWorld()->LineTraceTestByChannel(Center,Bounds.GetCenter(),ECC_Visibility,ReachQuery) ||
         GetWorld()->LineTraceTestByChannel(Center,GetOwner()->GetActorLocation(),ECC_Visibility,ReachQuery))))
        return Fail(OutReason,TEXT("HomeTakeEntranceOccluded"));
    // 已站在同一入口只表示无需移动；仍先通过地面、导航、胶囊与伸手检查，不伪造零长度路径成功
    const bool bAlreadyAtEntrance=FVector::Dist2D(Start,Feet)<=1. && FMath::Abs(Start.Z-Feet.Z)<=15.;
    if (!bAlreadyAtEntrance)
    {
        FPathFindingQuery PathQuery(Companion,*NavData,Start,Feet);
        PathQuery.SetAllowPartialPaths(false);
        const auto Path=Nav->FindPathSync(Properties,PathQuery);
        // 这一刻没有完整路线不再直接判失败：二狗可能正站在门槛外或坡上，出发后走不通会直接到站位上
        if (!Path.IsSuccessful() || !Path.Path.IsValid() || Path.Path->IsPartial())
            UE_LOG(LogTemp,Log,TEXT("HomeTakeEntranceNoPath object=%s from=%s to=%s"),*GetNameSafe(GetOwner()),*Start.ToCompactString(),*Feet.ToCompactString());
    }
    OutStanding=FTransform(FRotator(0,Authored.Rotator().Yaw,0),Feet,FVector::OneVector);
    OutReason=NAME_None;
    return true;
}

bool UDeliveryHomeObjectComponent::FindCommittedPlacement(FDeliveryHomeUseClaim& OutClaim,FName& OutReason) const
{
    OutClaim={};
    if (!ValidateHome(OutReason)) return false;
    const auto* Subsystem=GetWorld()->GetSubsystem<USmartObjectSubsystem>();
    if (!Subsystem) return Fail(OutReason,TEXT("HomeSlotUnavailable"));
    TArray<UDeliveryHomeObjectComponent*> Furniture;
    Home->GetFurnitureObjects(Furniture);
    for (const auto* Object:Furniture)
    {
        if (Object==this || !IsValid(Object) || Object->bEndingPlay || Object->Home!=Home) continue;
        for (const auto& Pair:Object->OwnedUses)
        {
            const auto& Placement=Pair.Value;
            FName Reason;
            if (!Placement.bPlacementCommitted || Placement.PlacementProp.Get()!=GetOwner() ||
                !Object->IsCurrentSmartObjectClaim(Placement) || !Object->ValidateHome(Reason) ||
                !Object->IsPlacementValid(Placement,GetOwner(),Reason)) continue;
            bool bOccupied=false;
            Subsystem->ReadSlotData(Placement.Handle.SlotHandle,[&](FConstSmartObjectSlotView View)
            { bOccupied=View.IsValid() && View.IsEnabled() && View.GetState()==ESmartObjectSlotState::Occupied; });
            if (!bOccupied) continue;
            if (OutClaim.IsValid()) return Fail(OutReason,TEXT("HomeTakePlacementAmbiguous"));
            OutClaim=Placement.Ticket;
        }
    }
    OutReason=NAME_None;
    return true;
}

TOptional<FTransform> UDeliveryHomeObjectComponent::ReadLiveSlotTransform(const FSmartObjectSlotHandle& Slot) const
{
    const auto* Subsystem=GetWorld()?GetWorld()->GetSubsystem<USmartObjectSubsystem>():nullptr;
    const auto* Smart=ResolveSmartObject();
    const auto* Definition=Smart?Smart->GetDefinition():nullptr;
    if (!Subsystem || !Definition || !Slot.IsValid() || Slot.GetSmartObjectHandle()!=Smart->GetRegisteredHandle() ||
        !Subsystem->IsSmartObjectSlotValid(Slot) || !Definition->GetSlots().IsValidIndex(Slot.GetSlotIndex())) return {};
    // 占槽仍归 Smart Objects，已搬过的家具按实时组件位置解析同一真实槽
    return Definition->GetSlotWorldTransform(Slot.GetSlotIndex(),Smart->GetComponentTransform());
}

bool UDeliveryHomeObjectComponent::ResolveUseTransforms(ADeliveryCompanionCharacter* Companion,
    const UDeliveryHomeBehaviorDefinition* Definition,const FSmartObjectSlotHandle& Slot,
    FTransform& OutEntrance,FTransform& OutUse,FTransform& OutExit,FName& OutReason) const
{
    const auto SlotTransform=ReadLiveSlotTransform(Slot);
    if (!Definition || !SlotTransform.IsSet()) return Fail(OutReason,TEXT("HomeSlotUnavailable"));
    MakeUseTransforms(*Definition,SlotTransform.GetValue(),OutEntrance,OutUse,OutExit);
    OutReason=NAME_None;
    if (Definition->Operation!=EDeliveryHomeOperation::TakeProp) return true;
    if (!IsValid(Companion) || Companion->GetWorld()!=GetWorld() || !ValidateHome(OutReason))
        return Fail(OutReason,TEXT("HomeTakeUserUnavailable"));
    // 已实际搬运后入口不再驱动行走，不能因重投地面失败丢掉本次持件
    const auto* Grab=GetOwner()->FindComponentByClass<UDeliveryGrabbableComponent>();
    for (const auto& Pair:OwnedUses)
    {
        const auto& Owned=Pair.Value;
        if (!Owned.bPlacementCommitted && Owned.Companion.Get()==Companion && Owned.Handle.SlotHandle==Slot &&
            Owned.Definition.Get()==Definition && IsCurrentSmartObjectClaim(Owned) && Grab &&
            Grab->IsCarriedByProvider(Companion->Companion,Owned.Ticket.ActionId)) return true;
    }
    if (!Grab || Grab->HasAnyCarrier() || GetOwner()->GetOwner() || GetOwner()->GetAttachParentActor())
        return Fail(OutReason,TEXT("HomePropAlreadyCarried"));
    FTransform GroundEntrance=OutEntrance,GroundExit=OutExit;
    FDeliveryHomeUseClaim Placement;
    if (!FindCommittedPlacement(Placement,OutReason)) return false;
    if (Placement.IsValid())
    {
        FTransform Support;
        if (!Placement.Object->GetUseTransforms(Placement,GroundEntrance,Support,GroundExit))
            return Fail(OutReason,TEXT("HomeTakePlacementChanged"));
    }
    const bool bProjectNearby=!Placement.IsValid();
    FTransform StandingEntrance,StandingExit;
    if (!ResolveTakeStandingPoint(Companion,*Definition,GroundEntrance,OutUse,bProjectNearby,StandingEntrance,OutReason))
    {
        if (!bProjectNearby) return false;
        const FBox Bounds=GetOriginalSizeBounds(GetOwner()->GetActorTransform());
        if (!Bounds.IsValid) return false;
        const FVector Extent=Bounds.GetExtent();
        const FVector Center=Bounds.GetCenter();
        FVector Toward=(Companion->GetNavAgentLocation()-Center).GetSafeNormal2D();
        if (Toward.IsNearlyZero()) Toward=FVector::ForwardVector;
        // 家具被搬到新位置后，旧入口可能贴墙；只在这件原物周围重查可落脚处
        for (const float Angle:{0.f,45.f,-45.f,90.f,-90.f,180.f})
        {
            const FVector Side=Toward.RotateAngleAxis(Angle,FVector::UpVector);
            const double Distance=FMath::Abs(Side.X)*Extent.X+FMath::Abs(Side.Y)*Extent.Y+Companion->GetSimpleCollisionRadius()+14.;
            if (!Grab->bAllowNpcEdgePickup && Distance>190.) continue;
            const FTransform Alternative((-Side).Rotation(),Center+Side*FMath::Max(65.,Distance));
            if (ResolveTakeStandingPoint(Companion,*Definition,Alternative,OutUse,true,OutEntrance,OutReason))
            { OutExit=OutEntrance; return true; }
        }
        return false;
    }
    if (GroundExit.Equals(GroundEntrance,.001)) StandingExit=StandingEntrance;
    else if (!ResolveTakeStandingPoint(Companion,*Definition,GroundExit,OutUse,bProjectNearby,StandingExit,OutReason)) return false;
    OutEntrance=StandingEntrance;
    OutExit=StandingExit;
    return true;
}

bool UDeliveryHomeObjectComponent::CanReachPlacementEntrance(const ADeliveryCompanionCharacter& Companion,
    const FDeliveryHomeUseCandidate& Candidate) const
{
    const auto* Definition=Candidate.Definition.Get();
    const auto* Capsule=Companion.GetCapsuleComponent();
    const auto* Movement=Companion.GetCharacterMovement();
    if (!Definition || !Capsule || !Movement) return false;
    const FVector Entrance=Candidate.EntranceTransform.GetLocation();
    const FVector Center=Entrance+FVector(0,0,Capsule->GetScaledCapsuleHalfHeight());
    const FVector Use=Candidate.UseTransform.TransformPositionNoScale(Definition->SightOffset);
    // 候选检查作者站位的距离、导航面与脚下空间，完整路线在行动开始时查询
    if (Entrance.ContainsNaN() || Use.ContainsNaN() || FVector::DistSquared(Center,Use)>FMath::Square(Definition->UseRadius)) return false;
    auto* World=GetWorld();
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
    const auto& Properties=Companion.GetNavAgentPropertiesRef();
    const FVector Start=Companion.GetNavAgentLocation();
    const auto* NavData=Nav?Nav->GetNavDataForProps(Properties,Start):nullptr;
    FNavLocation Projection;
    if (!World || !NavData || Start.ContainsNaN() ||
        !Nav->ProjectPointToNavigation(Entrance,Projection,FVector(18,18,30),NavData) ||
        FVector::Dist2D(Projection.Location,Entrance)>12. || FMath::Abs(Projection.Location.Z-Entrance.Z)>15.) return false;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(HomePlacementEntrance),false,&Companion);
    if (const auto* Component=Companion.Companion.Get()) Query.AddIgnoredActor(Component->GetHomeState().CarriedProp.Get());
    FHitResult Floor;
    if (!World->LineTraceSingleByChannel(Floor,Entrance+FVector(0,0,30),Entrance-FVector(0,0,35),ECC_Visibility,Query) ||
        Floor.bStartPenetrating || !Floor.GetComponent() || Floor.GetComponent()->IsSimulatingPhysics() ||
        !Movement->IsWalkable(Floor) || FMath::Abs(Floor.ImpactPoint.Z-Entrance.Z)>15.) return false;
    TArray<FOverlapResult> Overlaps;
    World->OverlapMultiByObjectType(Overlaps,Center+FVector(0,0,2),FQuat::Identity,ScaleCollisionObjects(),
        FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(),Capsule->GetScaledCapsuleHalfHeight()),Query);
    for (const auto& Overlap:Overlaps)
        if (BlocksScaleExpansion(Overlap.GetComponent(),ECC_Pawn)) return false;
    return true;
}

void UDeliveryHomeObjectComponent::FindAvailableUses(ADeliveryCompanionCharacter* Companion,APlayerState* Requester,
    TArray<FDeliveryHomeUseCandidate>& OutUses) const
{
    OutUses.Reset();
    FName Reason;
    if (!ValidateUser(Companion,Requester,Reason)) return;
    auto* Smart=ResolveSmartObject();
    auto* Subsystem=GetWorld()->GetSubsystem<USmartObjectSubsystem>();
    if (!Smart || !Subsystem || !Smart->GetRegisteredHandle().IsValid()) return;
    FSmartObjectRequestFilter Filter;
    Filter.BehaviorDefinitionClasses.Add(UDeliveryHomeBehaviorDefinition::StaticClass());
    Filter.ClaimPriority=ESmartObjectClaimPriority::Normal;
    Filter.bShouldIncludeClaimedSlots=false;
    Filter.bShouldIncludeDisabledSlots=false;
    Filter.bShouldEvaluateConditions=true;
    TArray<FSmartObjectSlotHandle> Slots;
    Subsystem->FindSlots(Smart->GetRegisteredHandle(),Filter,Slots,FConstStructView::Make(FSmartObjectActorUserData(Companion)));
    TSet<FName> SeenSlotIds;
    TSet<FName> DuplicateSlotIds;
    TArray<FSmartObjectSlotHandle> AllSlots;
    Subsystem->GetAllSlots(Smart->GetRegisteredHandle(),AllSlots);
    for (const auto& Slot:AllSlots)
    {
        const auto* Definition=Subsystem->GetBehaviorDefinition<UDeliveryHomeBehaviorDefinition>(FSmartObjectRequestResult(Smart->GetRegisteredHandle(),Slot));
        if (!Definition) continue;
        if (SeenSlotIds.Contains(Definition->SlotId)) DuplicateSlotIds.Add(Definition->SlotId);
        SeenSlotIds.Add(Definition->SlotId);
    }
    for (const auto& Slot:Slots)
    {
        FSmartObjectRequestResult Result(Smart->GetRegisteredHandle(),Slot);
        const auto* Definition=Subsystem->GetBehaviorDefinition<UDeliveryHomeBehaviorDefinition>(Result);
        if (!IsOperationAvailable(Definition) || !ValidateUse(Companion,Definition,Slot,false,Reason)) continue;
        FDeliveryHomeUseCandidate Candidate;
        Candidate.Object=const_cast<UDeliveryHomeObjectComponent*>(this);
        Candidate.Definition=Definition;
        Candidate.ObjectId=ObjectId; Candidate.SlotId=Definition->SlotId; Candidate.UseId=Definition->UseId;
        Candidate.Operation=Definition->Operation; Candidate.Revision=Revision; Candidate.Slot=Result;
        if (!ResolveUseTransforms(Companion,Definition,Slot,
            Candidate.EntranceTransform,Candidate.UseTransform,Candidate.ExitTransform,Reason)) continue;
        if (Candidate.Operation==EDeliveryHomeOperation::PlaceProp && !CanReachPlacementEntrance(*Companion,Candidate)) continue;
        OutUses.Add(MoveTemp(Candidate));
    }
    OutUses.RemoveAll([&](const auto& Candidate){return DuplicateSlotIds.Contains(Candidate.SlotId);});
}

bool UDeliveryHomeObjectComponent::TryClaimUse(ADeliveryCompanionCharacter* Companion,APlayerState* Requester,FGuid ActionId,
    const FDeliveryHomeUseCandidate& Candidate,FDeliveryHomeUseClaim& OutClaim,FName& OutReason,
    const UDeliveryHomeObjectComponent* PlacementSource)
{
    OutClaim={};
    if (!ActionId.IsValid()) return Fail(OutReason,TEXT("HomeActionIdMissing"));
    if (!ValidateUser(Companion,Requester,OutReason)) return false;
    if (Candidate.Object.Get()!=this || Candidate.ObjectId!=ObjectId) return Fail(OutReason,TEXT("HomeCandidateExpired"));
    TWeakObjectPtr<AActor> PlacementProp;
    if (Candidate.Operation==EDeliveryHomeOperation::PlaceProp)
    {
        auto* Prop=IsValid(PlacementSource)?PlacementSource->GetOwner():nullptr;
        // 正拿在这位同伴手里的道具可以直接改放到别处
        if (!IsValid(Prop) || PlacementSource==this || PlacementSource->Home!=Home || Prop==GetOwner() ||
            (Prop->GetOwner() && Prop->GetOwner()!=Companion) || Prop->IsA<APawn>() || Prop->IsA<ADeliveryPackageItem>() ||
            !PlacementSource->AllowedUses.Contains(EDeliveryHomeOperation::TakeProp))
            return Fail(OutReason,TEXT("HomePropUnavailable"));
        if (!PlacementSource->ValidateHome(OutReason)) return false;
        PlacementProp=Prop;
    }
    for (const auto& Pair:OwnedUses)
        if (Pair.Value.Ticket.ActionId==ActionId)
        {
            const auto& Existing=Pair.Value;
            if (!Existing.bPlacementCommitted && Existing.Companion==Companion && Existing.Requester==Requester &&
                Existing.PlacementProp.HasSameIndexAndSerialNumber(PlacementProp) &&
                Existing.Handle.SlotHandle==Candidate.Slot.SlotHandle && ValidateClaim(Existing.Ticket,false,OutReason))
            { OutClaim=Existing.Ticket; return true; }
            return Fail(OutReason,TEXT("HomeActionAlreadyClaimed"));
        }
    if (Candidate.Object.Get()!=this || Candidate.ObjectId!=ObjectId || Candidate.Revision!=Revision)
        return Fail(OutReason,TEXT("HomeCandidateExpired"));
    TArray<FDeliveryHomeUseCandidate> Current;
    FindAvailableUses(Companion,Requester,Current);
    const auto* Offered=Current.FindByPredicate([&](const auto& Item)
    { return Item.Slot==Candidate.Slot && Item.SlotId==Candidate.SlotId && Item.UseId==Candidate.UseId && Item.Operation==Candidate.Operation; });
    if (!Offered) return Fail(OutReason,TEXT("HomeUseNoLongerAvailable"));
    auto* Subsystem=GetWorld()->GetSubsystem<USmartObjectSubsystem>();
    const FGuid Token=FGuid::NewGuid();
    const TWeakObjectPtr<UDeliveryHomeObjectComponent> WeakThis(this);
    const auto Handle=Subsystem->MarkSlotAsClaimed(Candidate.Slot.SlotHandle,ESmartObjectClaimPriority::Normal,
        FConstStructView::Make(FDeliveryHomeSlotUserData(Companion,Token)));
    if (!Handle.IsValid()) return Fail(OutReason,TEXT("HomeSlotClaimRejected"));
    // SO 通知可能同步改变拥有者，领取后再次核验再交出凭据
    if (!WeakThis.IsValid() || !IsValid(Subsystem) || !ValidateUser(Companion,Requester,OutReason) ||
        !IsOperationAvailable(Offered->Definition.Get()) ||
        !HasClaimToken(Subsystem,Handle.SlotHandle,Token))
    {
        if (IsValid(Subsystem) && HasClaimToken(Subsystem,Handle.SlotHandle,Token)) Subsystem->MarkSlotAsFree(Handle);
        if (OutReason.IsNone()) OutReason=TEXT("HomeClaimExpired");
        return false;
    }
    FOwnedUse Use;
    Use.Ticket.Object=this; Use.Ticket.Token=Token; Use.Ticket.ActionId=ActionId;
    Use.Handle=Handle; Use.Companion=Companion; Use.Requester=Requester; Use.Definition=Offered->Definition;
    Use.PlacementProp=PlacementProp;
    OwnedUses.Add(Token,Use);
    Subsystem->RegisterSlotInvalidationCallback(Handle,FOnSlotInvalidated::CreateLambda(
        [WeakThis](const FSmartObjectClaimHandle& InvalidHandle,ESmartObjectSlotState)
        {
            const auto Notify=[WeakThis,InvalidHandle]()
            { if (auto* Self=WeakThis.Get()) Self->HandleSlotInvalidated(InvalidHandle); };
            if (IsInGameThread()) Notify(); else AsyncTask(ENamedThreads::GameThread,Notify);
        }));
    if (!ValidateClaim(Use.Ticket,false,OutReason)) { ReleaseOwned(Token,OutReason); return false; }
    OutClaim=Use.Ticket;
    RefreshReplicatedState();
    OutReason=NAME_None;
    return true;
}

const UDeliveryHomeObjectComponent::FOwnedUse* UDeliveryHomeObjectComponent::FindOwned(const FDeliveryHomeUseClaim& Claim) const
{
    const auto* Use=Claim.Object.Get()==this?OwnedUses.Find(Claim.Token):nullptr;
    return Use && Use->Ticket.ActionId==Claim.ActionId?Use:nullptr;
}

UDeliveryHomeObjectComponent::FOwnedUse* UDeliveryHomeObjectComponent::FindOwned(const FDeliveryHomeUseClaim& Claim)
{
    return const_cast<FOwnedUse*>(static_cast<const UDeliveryHomeObjectComponent*>(this)->FindOwned(Claim));
}

bool UDeliveryHomeObjectComponent::IsCurrentSmartObjectClaim(const FOwnedUse& Use) const
{
    const auto* Subsystem=GetWorld()?GetWorld()->GetSubsystem<USmartObjectSubsystem>():nullptr;
    return HasClaimToken(Subsystem,Use.Handle.SlotHandle,Use.Ticket.Token);
}

bool UDeliveryHomeObjectComponent::ValidateClaim(const FDeliveryHomeUseClaim& Claim,bool bRequireInteractionRange,FName& OutReason) const
{
    const auto* Use=FindOwned(Claim);
    if (!Use || Use->bPlacementCommitted || !IsCurrentSmartObjectClaim(*Use)) return Fail(OutReason,TEXT("HomeClaimExpired"));
    if (!ValidateUser(Use->Companion.Get(),Use->Requester.Get(),OutReason)) return false;
    auto* Subsystem=GetWorld()->GetSubsystem<USmartObjectSubsystem>();
    bool bEnabled=false;
    Subsystem->ReadSlotData(Use->Handle.SlotHandle,[&](FConstSmartObjectSlotView View){ bEnabled=View.IsValid() && View.IsEnabled(); });
    if (!bEnabled || !Subsystem->EvaluateSelectionConditions(Use->Handle.SlotHandle,
        FConstStructView::Make(FSmartObjectActorUserData(Use->Companion.Get())))) return Fail(OutReason,TEXT("HomeSlotConditionChanged"));
    const auto* Definition=Use->Definition.Get();
    if (CarryScaleState.Phase==EDeliveryHomeCarryScalePhase::CarriedSmall && Definition && Definition->Operation==EDeliveryHomeOperation::TakeProp)
    {
        const auto* Companion=Use->Companion.Get();
        const auto* Grabbable=GetOwner()->FindComponentByClass<UDeliveryGrabbableComponent>();
        if (Companion && Grabbable && Grabbable->IsCarriedByProvider(Companion->Companion,Claim.ActionId))
        {
            // 已拿起的实物由搬运凭据确认，转头和桌沿遮挡不应撤销原拿取凭据
            const auto* Smart=ResolveSmartObject();
            if (!IsRegistered() || !Smart || Use->Handle.SlotHandle.GetSmartObjectHandle()!=Smart->GetRegisteredHandle() ||
                Subsystem->GetSlotState(Use->Handle.SlotHandle)!=ESmartObjectSlotState::Occupied ||
                !Definition->IsUsableConfiguration() || !AllowedUses.Contains(EDeliveryHomeOperation::TakeProp))
                return Fail(OutReason,TEXT("HomeSlotUnavailable"));
            return ValidateScaleObject(OutReason);
        }
    }
    const auto* PlacementProp=Use->PlacementProp.Get();
    if (Definition && Definition->Operation==EDeliveryHomeOperation::PlaceProp &&
        (!IsValid(PlacementProp) || PlacementProp->IsActorBeingDestroyed()))
        return Fail(OutReason,TEXT("HomePropUnavailable"));
    // 同一份拿取凭据已由这名同伴登记搬运时，近距离复核沿用登记前的接触结果
    const auto* TakenProp=GetOwner()->FindComponentByClass<UDeliveryGrabbableComponent>();
    auto* User=Use->Companion.Get();
    const bool bHeldByUser=Definition && Definition->Operation==EDeliveryHomeOperation::TakeProp && TakenProp && User &&
        TakenProp->bAllowNpcEdgePickup && TakenProp->IsCarriedByProvider(User->Companion,Claim.ActionId);
    return ValidateUse(User,Definition,Use->Handle.SlotHandle,bRequireInteractionRange,OutReason,PlacementProp,bHeldByUser);
}

bool UDeliveryHomeObjectComponent::GetUseTransforms(const FDeliveryHomeUseClaim& Claim,FTransform& OutEntrance,FTransform& OutUse,FTransform& OutExit) const
{
    const auto* Use=FindOwned(Claim);
    if (!Use || !Use->Definition.IsValid() || !IsCurrentSmartObjectClaim(*Use)) return false;
    FName Reason;
    return ResolveUseTransforms(Use->Companion.Get(),Use->Definition.Get(),Use->Handle.SlotHandle,OutEntrance,OutUse,OutExit,Reason);
}

bool UDeliveryHomeObjectComponent::GetPlacementTransform(const FDeliveryHomeUseClaim& Claim,
    const UDeliveryHomeObjectComponent* Source,FTransform& OutTransform,FName& OutReason) const
{
    OutTransform=FTransform::Identity;
    if (!ValidateHome(OutReason)) return false;
    const auto* Use=FindOwned(Claim);
    const auto* Definition=Use?Use->Definition.Get():nullptr;
    if (!Definition || Definition->Operation!=EDeliveryHomeOperation::PlaceProp || !IsCurrentSmartObjectClaim(*Use))
        return Fail(OutReason,TEXT("HomePlacementClaimExpired"));
    const auto* Prop=IsValid(Source)?Source->GetOwner():nullptr;
    if (!IsValid(Source) || Source==this || Source->GetWorld()!=GetWorld() || Source->Home!=Home || Use->PlacementProp.Get()!=Prop ||
        !IsValid(Prop) || Prop==GetOwner() || Prop->IsActorBeingDestroyed() || Prop->IsA<APawn>() ||
        Prop->IsA<ADeliveryPackageItem>() || !Source->AllowedUses.Contains(EDeliveryHomeOperation::TakeProp))
        return Fail(OutReason,TEXT("HomePropUnavailable"));
    if (!Source->ValidateHome(OutReason)) return false;
    FTransform Entrance,Support,Exit;
    // 已提交的实物继续占槽，查询不依赖已经清掉的 Companion 和 Requester
    if (!GetUseTransforms(Claim,Entrance,Support,Exit) || Support.ContainsNaN() || !Support.GetRotation().IsNormalized())
        return Fail(OutReason,TEXT("HomeSlotUnavailable"));
    // 以最终朝向和原尺度计算几何偏心，物件还在手里缩小时也得到相同落位目标
    const FBox Bounds=Source->GetOriginalSizeBounds(FTransform(Support.GetRotation(),FVector::ZeroVector,FVector::OneVector));
    if (!Bounds.IsValid || Bounds.Min.ContainsNaN() || Bounds.Max.ContainsNaN() || Bounds.GetExtent().GetMin()<=0)
        return Fail(OutReason,TEXT("HomePropBoundsMissing"));
    const FBox Footprint=Source->GetPlacementFootprintBounds(FTransform(Support.GetRotation()));
    if (!Footprint.IsValid) return Fail(OutReason,TEXT("HomePropBoundsMissing"));
    const FVector Offset(Footprint.GetCenter().X,Footprint.GetCenter().Y,Bounds.Min.Z);
    OutTransform=FTransform(Support.GetRotation(),Support.GetLocation()-Offset,FVector::OneVector);
    OutReason=NAME_None;
    return true;
}

bool UDeliveryHomeObjectComponent::ReadPropTransferPoint(const FOwnedUse& Use,
    FDeliveryHomePropTransferPoint& OutPoint,FName& OutReason) const
{
    OutPoint={};
    const auto* Definition=Use.Definition.Get();
    auto* Smart=ResolveSmartObject();
    const auto* Subsystem=GetWorld()?GetWorld()->GetSubsystem<USmartObjectSubsystem>():nullptr;
    if (!ValidateHome(OutReason)) return false;
    if (!Smart || !Subsystem || !IsCurrentSmartObjectClaim(Use) || !Definition ||
        !Definition->IsUsableConfiguration() || Definition->Operation!=EDeliveryHomeOperation::PlaceProp ||
        !AllowedUses.Contains(EDeliveryHomeOperation::PlaceProp))
        return Fail(OutReason,TEXT("HomePropTransferUnavailable"));
    if (!Definition->bUsePropTransferPoint) { OutReason=NAME_None; return true; }
    const auto Slot=ReadLiveSlotTransform(Use.Handle.SlotHandle);
    if (!Slot.IsSet() || Slot->ContainsNaN() || !Slot->GetRotation().IsNormalized())
        return Fail(OutReason,TEXT("HomePropTransferUnavailable"));
    OutPoint.bEnabled=true;
    OutPoint.Object=const_cast<UDeliveryHomeObjectComponent*>(this);
    OutPoint.Home=Home.Get();
    OutPoint.Definition=Definition;
    OutPoint.Slot=Use.Handle.SlotHandle;
    OutPoint.ActorTransform=GetOwner()->GetActorTransform();
    OutPoint.SmartObjectTransform=Smart->GetComponentTransform();
    OutPoint.SlotTransform=Slot.GetValue();
    OutPoint.UseOffset=Definition->UseOffset;
    OutPoint.LocalOffset=Definition->PropTransferOffset;
    // 只继承槽位的位置和朝向，家具 scale 不会再次放大厘米偏移
    const FTransform Frame(Slot->GetRotation(),Slot->GetLocation(),FVector::OneVector);
    OutPoint.WorldPoint=Frame.TransformPositionNoScale(Definition->PropTransferOffset);
    return ValidatePropTransferPoint(OutPoint,OutReason);
}

bool UDeliveryHomeObjectComponent::CapturePropTransferPoint(const FDeliveryHomeUseClaim& Claim,
    FDeliveryHomePropTransferPoint& OutPoint,FName& OutReason) const
{
    OutPoint={};
    if (!ValidateClaim(Claim,true,OutReason)) return false;
    const auto* Use=FindOwned(Claim);
    const auto* Definition=Use?Use->Definition.Get():nullptr;
    if (!Definition) return Fail(OutReason,TEXT("HomePropTransferUnavailable"));
    if (Definition->Operation==EDeliveryHomeOperation::PlaceProp) return ReadPropTransferPoint(*Use,OutPoint,OutReason);
    if (Definition->Operation!=EDeliveryHomeOperation::TakeProp)
        return Fail(OutReason,TEXT("HomePropTransferUnavailable"));
    const auto* Grab=GetOwner()->FindComponentByClass<UDeliveryGrabbableComponent>();
    if (!Grab || Grab->HasAnyCarrier() || GetOwner()->GetOwner() || GetOwner()->GetAttachParentActor())
        return Fail(OutReason,TEXT("HomePropAlreadyCarried"));
    FDeliveryHomeUseClaim Placement;
    if (!FindCommittedPlacement(Placement,OutReason)) return false;
    if (!Placement.IsValid()) { OutReason=NAME_None; return true; }
    const auto* Object=Placement.Object.Get();
    const auto* Placed=Object?Object->FindOwned(Placement):nullptr;
    // 这里只读取真实摆放记录，之后实物被拿起、旧槽释放不会清掉这份路线快照
    return Placed?Object->ReadPropTransferPoint(*Placed,OutPoint,OutReason):Fail(OutReason,TEXT("HomeTakePlacementChanged"));
}

bool UDeliveryHomeObjectComponent::ValidatePropTransferPoint(const FDeliveryHomePropTransferPoint& Point,FName& OutReason) const
{
    const auto* Definition=Point.Definition.Get();
    auto* Smart=ResolveSmartObject();
    auto* Subsystem=GetWorld()?GetWorld()->GetSubsystem<USmartObjectSubsystem>():nullptr;
    if (!ValidateHome(OutReason)) return false;
    if (!Point.bEnabled || Point.Object.Get()!=this || Point.Home.Get()!=Home.Get() || !Definition || !Smart || !Subsystem ||
        Point.Slot.GetSmartObjectHandle()!=Smart->GetRegisteredHandle() || !Subsystem->IsSmartObjectSlotValid(Point.Slot) ||
        !Definition->IsUsableConfiguration() || Definition->Operation!=EDeliveryHomeOperation::PlaceProp ||
        !Definition->bUsePropTransferPoint || !AllowedUses.Contains(EDeliveryHomeOperation::PlaceProp) ||
        !Definition->PropTransferOffset.Equals(Point.LocalOffset,.01) || !Definition->UseOffset.Equals(Point.UseOffset,.01) ||
        !GetOwner()->GetActorTransform().Equals(Point.ActorTransform,.01) ||
        !Smart->GetComponentTransform().Equals(Point.SmartObjectTransform,.01) ||
        Subsystem->GetBehaviorDefinition<UDeliveryHomeBehaviorDefinition>(
            FSmartObjectRequestResult(Smart->GetRegisteredHandle(),Point.Slot))!=Definition)
        return Fail(OutReason,TEXT("HomePropTransferChanged"));
    bool bEnabled=false;
    Subsystem->ReadSlotData(Point.Slot,[&](FConstSmartObjectSlotView View){ bEnabled=View.IsValid() && View.IsEnabled(); });
    const auto Slot=ReadLiveSlotTransform(Point.Slot);
    if (!bEnabled || !Slot.IsSet() || !Slot->Equals(Point.SlotTransform,.01) || Point.WorldPoint.ContainsNaN())
        return Fail(OutReason,TEXT("HomePropTransferChanged"));
    const FTransform Frame(Slot->GetRotation(),Slot->GetLocation(),FVector::OneVector);
    if (!Frame.TransformPositionNoScale(Definition->PropTransferOffset).Equals(Point.WorldPoint,.01))
        return Fail(OutReason,TEXT("HomePropTransferChanged"));
    OutReason=NAME_None;
    return true;
}

const UDeliveryHomeBehaviorDefinition* UDeliveryHomeObjectComponent::GetUseDefinition(const FDeliveryHomeUseClaim& Claim) const
{
    const auto* Use=FindOwned(Claim);
    return Use && IsCurrentSmartObjectClaim(*Use)?Use->Definition.Get():nullptr;
}

bool UDeliveryHomeObjectComponent::BeginUse(const FDeliveryHomeUseClaim& Claim,FName& OutReason)
{
    if (!ValidateClaim(Claim,true,OutReason)) return false;
    auto* Use=FindOwned(Claim);
    auto* Subsystem=GetWorld()->GetSubsystem<USmartObjectSubsystem>();
    if (Subsystem->GetSlotState(Use->Handle.SlotHandle)==ESmartObjectSlotState::Occupied) return true;
    if (!IsOperationAvailable(Use->Definition.Get())) return Fail(OutReason,TEXT("HomeUseNoLongerAvailable"));
    const auto Handle=Use->Handle;
    const TWeakObjectPtr<UDeliveryHomeObjectComponent> WeakThis(this);
    if (!Subsystem->MarkSlotAsOccupied<UDeliveryHomeBehaviorDefinition>(Handle)) return Fail(OutReason,TEXT("HomeOccupyRejected"));
    if (!WeakThis.IsValid()) return Fail(OutReason,TEXT("HomeObjectUnavailable"));
    if (!ValidateClaim(Claim,true,OutReason))
    {
        ReleaseOwned(Claim.Token,OutReason);
        return false;
    }
    RefreshReplicatedState();
    return true;
}

bool UDeliveryHomeObjectComponent::CommitAudioState(const FDeliveryHomeUseClaim& Claim,FName& OutReason)
{
    if (!ValidateClaim(Claim,true,OutReason)) return false;
    const auto* Use=FindOwned(Claim);
    const auto* Definition=Use->Definition.Get();
    auto* Subsystem=GetWorld()->GetSubsystem<USmartObjectSubsystem>();
    if (Subsystem->GetSlotState(Use->Handle.SlotHandle)!=ESmartObjectSlotState::Occupied ||
        (Definition->Operation!=EDeliveryHomeOperation::PlayAudio && Definition->Operation!=EDeliveryHomeOperation::StopAudio) ||
        !IsOperationAvailable(Definition)) return Fail(OutReason,TEXT("HomeAudioStateChanged"));
    bAudioPlaying=Definition->Operation==EDeliveryHomeOperation::PlayAudio;
    ActiveSound=bAudioPlaying?Definition->Sound.Get():nullptr;
    AudioStartedServerTime=GetServerTime();
    const int32 Generation=++AudioGeneration;
    GetWorld()->GetTimerManager().ClearTimer(AudioFinishTimer);
    if (bAudioPlaying && ActiveSound && !ActiveSound->IsLooping())
    {
        const float Duration=ActiveSound->GetDuration();
        if (FMath::IsFinite(Duration) && Duration>0)
            GetWorld()->GetTimerManager().SetTimer(AudioFinishTimer,
                FTimerDelegate::CreateUObject(this,&UDeliveryHomeObjectComponent::StopFinishedAudio,Generation),Duration,false);
    }
    // 在声音组件回调前先移除本次执行凭据，重入不能释放下一轮使用
    const FGuid ActionId=Claim.ActionId;
    auto* MutableUse=FindOwned(Claim);
    MutableUse->bExecutionEnded=true;
    const TWeakObjectPtr<UDeliveryHomeObjectComponent> WeakThis(this);
    ReleaseOwned(Claim.Token,TEXT("HomeAudioCommitted"));
    if (!WeakThis.IsValid()) return true;
    OnRep_Audio();
    if (WeakThis.IsValid()) OnUseEnded.Broadcast(ActionId,TEXT("HomeAudioCommitted"));
    return true;
}

bool UDeliveryHomeObjectComponent::IsPlacementValid(const FOwnedUse& Use,AActor* Prop,FName& OutReason) const
{
    const auto* Definition=Use.Definition.Get();
    if (!Definition || Definition->Operation!=EDeliveryHomeOperation::PlaceProp || !IsValid(Prop) ||
        Prop==GetOwner() || Prop->IsActorBeingDestroyed() || Prop->GetWorld()!=GetWorld() || Prop->GetOwner())
        return Fail(OutReason,TEXT("HomePropUnavailable"));
    if (!IsStablePlacementSurface()) return Fail(OutReason,TEXT("HomePlacementUnsupported"));
    const auto* PropMetadata=Prop->FindComponentByClass<UDeliveryHomeObjectComponent>();
    const auto* Grabbable=Prop->FindComponentByClass<UDeliveryGrabbableComponent>();
    const auto* Body=Cast<UPrimitiveComponent>(Prop->GetRootComponent());
    if (!PropMetadata || PropMetadata->Home!=Home || !PropMetadata->ValidateHome(OutReason) ||
        !Grabbable || Grabbable->HasAnyCarrier() || !Body || Body->Mobility!=EComponentMobility::Movable ||
        !Prop->GetActorEnableCollision() || !Body->IsQueryCollisionEnabled() || !Body->IsSimulatingPhysics())
        return Fail(OutReason,TEXT("HomePropStillCarried"));
    if (PropMetadata->IsCarryScaleActive()) return Fail(OutReason,TEXT("HomePropStillSmall"));
    if (!Definition->RequiredPropTags.IsEmpty() && !Definition->RequiredPropTags.Matches(PropMetadata->ObjectTags))
        return Fail(OutReason,TEXT("HomePropDoesNotFitSlot"));
    FTransform Placement;
    if (!GetPlacementTransform(Use.Ticket,PropMetadata,Placement,OutReason)) return false;
    if (FVector::Dist(Prop->GetActorLocation(),Placement.GetLocation())>Definition->PlacementTolerance ||
        FMath::RadiansToDegrees(Prop->GetActorQuat().AngularDistance(Placement.GetRotation()))>Definition->PlacementAngleTolerance)
        return Fail(OutReason,TEXT("HomePropNotAtPlacement"));
    // 直接把原尺寸几何转进槽位轴向，世界 AABB 再反转会在非零 yaw 时二次膨胀
    const FQuat SlotRelativeRotation=Placement.GetRotation().Inverse()*Prop->GetActorQuat();
    const FBox SlotBounds=PropMetadata->GetOriginalSizeBounds(FTransform(SlotRelativeRotation,FVector::ZeroVector,FVector::OneVector));
    if (!SlotBounds.IsValid || SlotBounds.Min.ContainsNaN() || SlotBounds.Max.ContainsNaN() || SlotBounds.GetExtent().GetMin()<=0)
        return Fail(OutReason,TEXT("HomePropBoundsMissing"));
    // 平移只影响中心，不影响半尺寸；世界位置另由到位、碰撞和支撑查询核验
    const FBox SlotFootprint=PropMetadata->GetPlacementFootprintBounds(FTransform(SlotRelativeRotation));
    if (!SlotFootprint.IsValid) return Fail(OutReason,TEXT("HomePropBoundsMissing"));
    const FVector Extent(SlotFootprint.GetExtent().X, SlotFootprint.GetExtent().Y, SlotBounds.GetExtent().Z);
    if (Extent.X>Definition->MaximumPropHalfExtent.X || Extent.Y>Definition->MaximumPropHalfExtent.Y || Extent.Z>Definition->MaximumPropHalfExtent.Z)
        return Fail(OutReason,TEXT("HomePropTooLarge"));
    const FBox Footprint=PropMetadata->GetPlacementFootprintBounds(Prop->GetActorTransform());
    if (!Footprint.IsValid) return Fail(OutReason,TEXT("HomePropBoundsMissing"));
    const FVector Center=Footprint.GetCenter(), FootExtent=Footprint.GetExtent();
    if (PropMetadata->HasPlacementFootprint())
    {
        const FVector MassCenter=Body->GetCenterOfMass();
        if (MassCenter.ContainsNaN() || FMath::Abs(MassCenter.X-Center.X)>FootExtent.X*.7 ||
            FMath::Abs(MassCenter.Y-Center.Y)>FootExtent.Y*.7) return Fail(OutReason,TEXT("HomePlacementUnsupported"));
    }
    return PropMetadata->HasRestoreSpaceAndSupport(Prop->GetActorTransform(),false,OutReason,this,
        Definition->MaximumSupportGap,Definition->MinimumSupportNormalZ);
}

bool UDeliveryHomeObjectComponent::CanCommitPlacement(const FDeliveryHomeUseClaim& Claim,AActor* Prop,FName& OutReason) const
{
    if (!ValidateClaim(Claim,true,OutReason)) return false;
    const auto* Use=FindOwned(Claim);
    if (GetWorld()->GetSubsystem<USmartObjectSubsystem>()->GetSlotState(Use->Handle.SlotHandle)!=ESmartObjectSlotState::Occupied)
        return Fail(OutReason,TEXT("HomeUseNotStarted"));
    return IsPlacementValid(*Use,Prop,OutReason);
}

bool UDeliveryHomeObjectComponent::CommitPlacement(const FDeliveryHomeUseClaim& Claim,AActor* Prop,FName& OutReason)
{
    if (!ValidateHome(OutReason)) return false;
    auto* Use=FindOwned(Claim);
    if (Use && Use->bPlacementCommitted && Use->PlacementProp==Prop && IsCurrentSmartObjectClaim(*Use))
        return IsPlacementValid(*Use,Prop,OutReason);
    if (!CanCommitPlacement(Claim,Prop,OutReason)) return false;
    Use=FindOwned(Claim);
    Use->bPlacementCommitted=true; Use->bExecutionEnded=true;
    // SO 中原使用者仅为弱引用，占用从这里起由实物持续保持
    Use->Companion.Reset(); Use->Requester.Reset();
    RefreshReplicatedState();
    OnUseEnded.Broadcast(Claim.ActionId,TEXT("HomePlacementCommitted"));
    return true;
}

bool UDeliveryHomeObjectComponent::ValidateScaleObject(FName& OutReason) const
{
    const auto* Actor=GetOwner();
    const auto* Body=IsValid(Actor)?Cast<UPrimitiveComponent>(Actor->GetRootComponent()):nullptr;
    if (!IsInGameThread() || bEndingPlay || !GetWorld() || !IsValid(Actor) || !Actor->HasAuthority() ||
        Actor->IsActorBeingDestroyed() || Actor->IsA<APawn>() || Actor->IsA<ADeliveryPackageItem>() ||
        (!IsCarryScaleActive() && !AllowedUses.Contains(EDeliveryHomeOperation::TakeProp)) ||
        !Body || !Body->IsRegistered() || Body->Mobility!=EComponentMobility::Movable || !Body->GetIsReplicated() ||
        !Actor->GetIsReplicated() || !Actor->IsReplicatingMovement() || !Body->IsQueryCollisionEnabled() ||
        !Actor->GetActorEnableCollision() || !Actor->FindComponentByClass<UDeliveryGrabbableComponent>() ||
        (IsCarryScaleActive() && ScaleRoot.Get()!=Body) || !ValidScale(GetOriginalActorScale()))
        return Fail(OutReason,TEXT("HomePropScaleUnavailable"));
    OutReason=NAME_None;
    return true;
}

FVector UDeliveryHomeObjectComponent::GetOriginalActorScale() const
{
    return IsCarryScaleActive()?CarryScaleState.OriginalActorScale:GetOwner()?GetOwner()->GetActorScale3D():FVector::OneVector;
}

FBox UDeliveryHomeObjectComponent::CalculateSizeBounds(const FTransform& ActorTransform) const
{
    FBox Bounds(ForceInit);
    if (!IsValid(GetOwner()) || ActorTransform.ContainsNaN() || !ValidScale(ActorTransform.GetScale3D())) return Bounds;
    const FTransform Current=GetOwner()->GetActorTransform();
    if (Current.ContainsNaN() || !ValidScale(Current.GetScale3D())) return Bounds;
    TInlineComponentArray<UPrimitiveComponent*> Primitives(GetOwner());
    for (const auto* Primitive:Primitives)
        if (IsValid(Primitive) && Primitive->IsRegistered())
        {
            // 独立绝对变换的子网格不会跟随 Actor 缩放，不能用相对变换猜它的展开空间
            if (Primitive!=GetOwner()->GetRootComponent() && (Primitive->IsUsingAbsoluteLocation() ||
                Primitive->IsUsingAbsoluteRotation() || Primitive->IsUsingAbsoluteScale())) return FBox(ForceInit);
            const FTransform Relative=Primitive->GetComponentTransform().GetRelativeTransform(Current);
            const FBox Part=Primitive->CalcBounds(Relative*ActorTransform).GetBox();
            if (!Part.IsValid || Part.Min.ContainsNaN() || Part.Max.ContainsNaN()) return FBox(ForceInit);
            Bounds+=Part;
        }
    return Bounds;
}

FBox UDeliveryHomeObjectComponent::GetOriginalSizeBounds(const FTransform& ActorTransform) const
{
    FTransform Original=ActorTransform;
    Original.SetScale3D(GetOriginalActorScale());
    return CalculateSizeBounds(Original);
}

bool UDeliveryHomeObjectComponent::HasPlacementFootprint() const
{
    return !PlacementFootprintCenter.ContainsNaN() && !PlacementFootprintHalfExtent.ContainsNaN() &&
        PlacementFootprintHalfExtent.X>0. && PlacementFootprintHalfExtent.Y>0.;
}

FBox UDeliveryHomeObjectComponent::GetPlacementFootprintBounds(const FTransform& ActorTransform) const
{
    if (!HasPlacementFootprint()) return GetOriginalSizeBounds(ActorTransform);
    FTransform Original=ActorTransform;
    Original.SetScale3D(GetOriginalActorScale());
    const FVector Extent(PlacementFootprintHalfExtent.X,PlacementFootprintHalfExtent.Y,0);
    return FBox(PlacementFootprintCenter-Extent,PlacementFootprintCenter+Extent).TransformBy(Original);
}

bool UDeliveryHomeObjectComponent::GetCarryPlacementLocation(const FTransform& FullSizeGoal,FVector& OutLocation,bool bPredictCarryScale) const
{
    OutLocation=FullSizeGoal.GetLocation();
    if ((!IsCarryScaleActive() && !bPredictCarryScale) || !bRestoreScaleFromSupportBottom) return !OutLocation.ContainsNaN();
    const FVector SmallScale=IsCarryScaleActive()?GetOwner()->GetActorScale3D():GetOriginalActorScale()*CarryScaleMultiplier;
    if (!ValidScale(SmallScale)) return false;
    const FBox Original=GetOriginalSizeBounds(FullSizeGoal);
    FTransform CarryGoal=FullSizeGoal;
    CarryGoal.SetScale3D(SmallScale);
    const FBox Small=CalculateSizeBounds(CarryGoal);
    if (!Original.IsValid || !Small.IsValid) return false;
    // 小件先接触实际支撑面，展开时才按同一底部接触点恢复高度
    OutLocation.Z+=Original.Min.Z-Small.Min.Z;
    return !OutLocation.ContainsNaN();
}

void UDeliveryHomeObjectComponent::SetCarryScalePhase(EDeliveryHomeCarryScalePhase Phase)
{
    if (CarryScaleState.Phase==Phase) return;
    CarryScaleState.Phase=Phase;
    NextScaleRestoreOffsetAt=-1;
    ++ScaleGeneration;
    RefreshReplicatedState();
}

bool UDeliveryHomeObjectComponent::BeginCarryScale(const FDeliveryHomeUseClaim& TakeClaim,UActorComponent* Provider,FGuid CarryId,FName& OutReason)
{
    if (bChangingCarryScale) return Fail(OutReason,TEXT("HomeScaleChangeInProgress"));
    if (!ValidateScaleObject(OutReason) || !ValidateClaim(TakeClaim,true,OutReason)) return false;
    const auto* Use=FindOwned(TakeClaim);
    auto* Actor=GetOwner();
    auto* Grabbable=Actor->FindComponentByClass<UDeliveryGrabbableComponent>();
    if (!IsValid(Provider) || Provider->GetWorld()!=GetWorld() || !CarryId.IsValid() || TakeClaim.ActionId!=CarryId ||
        !Use || !Use->Definition.IsValid() || Use->Definition->Operation!=EDeliveryHomeOperation::TakeProp ||
        !Use->Companion.IsValid() || Use->Companion->Companion!=Provider || Actor->GetOwner() || Actor->GetAttachParentActor() ||
        !Grabbable->IsCarriedByProvider(Provider,CarryId) ||
        GetWorld()->GetSubsystem<USmartObjectSubsystem>()->GetSlotState(Use->Handle.SlotHandle)!=ESmartObjectSlotState::Occupied ||
        !FMath::IsFinite(CarryScaleMultiplier) || CarryScaleMultiplier<.1f || CarryScaleMultiplier>1.f)
        return Fail(OutReason,TEXT("HomePropCarryScaleRejected"));
    const bool bWasSmall=IsCarryScaleActive();
    if (!bWasSmall)
    {
        CarryScaleState.OriginalActorScale=Actor->GetActorScale3D();
        ScaleRoot=Cast<UPrimitiveComponent>(Actor->GetRootComponent());
    }
    SetCarryScalePhase(EDeliveryHomeCarryScalePhase::CarriedSmall);
    const uint32 Generation=ScaleGeneration;
    const TWeakObjectPtr<UDeliveryHomeObjectComponent> WeakThis(this);
    const TWeakObjectPtr<UDeliveryGrabbableComponent> WeakGrab(Grabbable);
    const TWeakObjectPtr<UActorComponent> WeakProvider(Provider);
    TGuardValue<bool> ChangeGuard(bChangingCarryScale,true);
    // 重拿仍在等待展开的小物件时只换阶段，保存尺度与当前小尺度都不重算
    if (!bWasSmall) Actor->SetActorScale3D(CarryScaleState.OriginalActorScale*CarryScaleMultiplier);
    if (!WeakThis.IsValid()) return false;
    if (bEndingPlay || Generation!=ScaleGeneration || !WeakGrab.IsValid() || !WeakProvider.IsValid() ||
        !WeakGrab->IsCarriedByProvider(WeakProvider.Get(),CarryId) || !ValidateClaim(TakeClaim,false,OutReason))
    {
        QueueScaleRestore();
        if (OutReason.IsNone()) OutReason=TEXT("HomePropCarryChanged");
        return false;
    }
    Actor->ForceNetUpdate();
    OutReason=NAME_None;
    return true;
}

bool UDeliveryHomeObjectComponent::IsStablePlacementSurface() const
{
    const auto* Actor=GetOwner();
    const auto* Body=IsValid(Actor)?Cast<UPrimitiveComponent>(Actor->GetRootComponent()):nullptr;
    const auto* Grab=Actor?Actor->FindComponentByClass<UDeliveryGrabbableComponent>():nullptr;
    FName Reason;
    bool bRequireRuntimeHome=true;
#if WITH_EDITOR
    // 普通编辑器可预检几何。所有游戏世界继续要求已认领，不能用预览生成候选或占槽
    if (const auto* World=GetWorld(); World && World->WorldType==EWorldType::Editor && !World->HasBegunPlay())
        bRequireRuntimeHome=false;
#endif
    return Body && ValidateHome(Reason,bRequireRuntimeHome) && !IsCarryScaleActive() && !Actor->GetOwner() && !Actor->GetAttachParentActor() &&
        (!Grab || !Grab->HasAnyCarrier()) && (!Body->IsSimulatingPhysics() ||
            (Body->GetPhysicsLinearVelocity().SizeSquared()<=FMath::Square(5.f) &&
             Body->GetPhysicsAngularVelocityInDegrees().SizeSquared()<=FMath::Square(5.f)));
}

bool UDeliveryHomeObjectComponent::HasRestoreSpaceAndSupport(const FTransform& Target,bool bCheckExpansion,FName& OutReason,
    const UDeliveryHomeObjectComponent* PlacementSupport,float SupportGap,float MinimumSupportNormalZ,bool bAllowRestingFurniture) const
{
    const auto* Actor=GetOwner();
    const auto* Body=Cast<UPrimitiveComponent>(Actor->GetRootComponent());
    if (PlacementSupport && (PlacementSupport==this || PlacementSupport->Home!=Home ||
        !PlacementSupport->IsStablePlacementSurface())) return Fail(OutReason,TEXT("HomePlacementUnsupported"));
    const FBox Full=GetOriginalSizeBounds(Target);
    if (!Full.IsValid || Full.GetExtent().GetMin()<=.1) return Fail(OutReason,TEXT("HomePropBoundsMissing"));
    FBox Envelope=Full;
    if (bCheckExpansion)
    {
        const FBox Current=CalculateSizeBounds(Actor->GetActorTransform());
        if (!Current.IsValid) return Fail(OutReason,TEXT("HomePropBoundsMissing"));
        // 正倍率缩放与竖直底面补偿的每个中间点都在两个完整包围盒的并集中
        Envelope+=Current;
    }
    FCollisionQueryParams Query(SCENE_QUERY_STAT(HomePropScaleRestore),false,Actor);
    const FCollisionObjectQueryParams Objects=ScaleCollisionObjects();
    // 只留半厘米底面接触余量，侧面和顶部不缩小，搬运临时忽略 Pawn 的响应不能影响此查询
    Envelope.Min.Z+=.5;
    TArray<FOverlapResult> Overlaps;
    GetWorld()->OverlapMultiByObjectType(Overlaps,Envelope.GetCenter(),FQuat::Identity,Objects,
        FCollisionShape::MakeBox(Envelope.GetExtent()),Query);
    for (const auto& Overlap:Overlaps)
        if (const auto* Other=Overlap.GetComponent(); IsValid(Other) && Other->GetOwner()!=Actor &&
            BlocksScaleExpansion(Other,Body->GetCollisionObjectType()))
            return Fail(OutReason,TEXT("HomeScaleRestoreBlocked"));
    // 五点复核同一稳定支撑，取消后的落点也可使用同房屋静止的登记家具
    const FBox Footprint=GetPlacementFootprintBounds(Target);
    if (!Footprint.IsValid) return Fail(OutReason,TEXT("HomePropBoundsMissing"));
    Query.bTraceComplex=PlacementSupport && PlacementSupport->bTracePlacementSupportComplex;
    const FVector Center=Footprint.GetCenter(),Extent=Footprint.GetExtent();
    const UPrimitiveComponent* RestingFurniture=nullptr;
    bool bCenterSample=true;
    for (const FVector Offset:{FVector::ZeroVector,FVector(Extent.X*.7,Extent.Y*.7,0),
        FVector(Extent.X*.7,-Extent.Y*.7,0),FVector(-Extent.X*.7,Extent.Y*.7,0),FVector(-Extent.X*.7,-Extent.Y*.7,0)})
    {
        const FVector Bottom(Center.X+Offset.X,Center.Y+Offset.Y,Full.Min.Z);
        TArray<FHitResult> Hits;
        GetWorld()->LineTraceMultiByObjectType(Hits,Bottom+FVector(0,0,2),Bottom-FVector(0,0,SupportGap),Objects,Query);
        const FHitResult* Support=Hits.FindByPredicate([&](const FHitResult& Hit)
        { return BlocksScaleExpansion(Hit.GetComponent(),Body->GetCollisionObjectType()); });
        const auto* HitActor=Support?Support->GetActor():nullptr;
        const auto* HitBody=Support?Support->GetComponent():nullptr;
        if (!IsValid(HitActor) || !IsValid(HitBody) || HitActor->IsA<APawn>() ||
            HitBody->GetCollisionObjectType()==ECC_Vehicle ||
            Support->ImpactNormal.Z<MinimumSupportNormalZ || Support->bStartPenetrating)
            return Fail(OutReason,TEXT("HomeScaleRestoreUnsupported"));
        if (bCenterSample && bAllowRestingFurniture && !PlacementSupport && HitBody->IsSimulatingPhysics())
        {
            const auto* Furniture=HitActor->FindComponentByClass<UDeliveryHomeObjectComponent>();
            if (Furniture && Furniture!=this && Furniture->Home==Home && HitBody==HitActor->GetRootComponent() &&
                Furniture->IsStablePlacementSurface()) RestingFurniture=HitBody;
        }
        if (PlacementSupport?HitActor!=PlacementSupport->GetOwner():
            (RestingFurniture?HitBody!=RestingFurniture:HitBody->IsSimulatingPhysics()))
            return Fail(OutReason,TEXT("HomeScaleRestoreUnsupported"));
        bCenterSample=false;
    }
    OutReason=NAME_None;
    return true;
}

bool UDeliveryHomeObjectComponent::CanRestoreScaleAt(const FTransform& FullSizeGoal,FName& OutReason,const UDeliveryHomeObjectComponent* Support) const
{
    if (!ValidateScaleObject(OutReason)) return false;
    if (FullSizeGoal.ContainsNaN() || !FullSizeGoal.GetRotation().IsNormalized() ||
        !FullSizeGoal.GetScale3D().Equals(GetOriginalActorScale(),.001)) return Fail(OutReason,TEXT("HomeRestoreScaleGoalInvalid"));
    return HasRestoreSpaceAndSupport(FullSizeGoal,true,OutReason,Support);
}

bool UDeliveryHomeObjectComponent::CanPlaceAtOriginalSize(const FTransform& Goal,FName& OutReason,const UDeliveryHomeObjectComponent* Support) const
{
    if (!ValidateScaleObject(OutReason)) return false;
    if (Goal.ContainsNaN() || !Goal.GetRotation().IsNormalized() || !Goal.GetScale3D().Equals(GetOriginalActorScale(),.001))
        return Fail(OutReason,TEXT("HomeRestoreScaleGoalInvalid"));
    return HasRestoreSpaceAndSupport(Goal,false,OutReason,Support);
}

bool UDeliveryHomeObjectComponent::RestoreScale(const FTransform& Target,FName& OutReason,const UDeliveryHomeObjectComponent* Support,
    bool bAllowRestingFurniture)
{
    if (bChangingCarryScale) return Fail(OutReason,TEXT("HomeScaleChangeInProgress"));
    if (!ValidateScaleObject(OutReason)) return false;
    auto* Actor=GetOwner();
    auto* Body=Cast<UPrimitiveComponent>(Actor->GetRootComponent());
    auto* Grabbable=Actor->FindComponentByClass<UDeliveryGrabbableComponent>();
    if (Grabbable->HasAnyCarrier() || Actor->GetOwner() || Actor->GetAttachParentActor() || !Body->IsSimulatingPhysics())
        return Fail(OutReason,TEXT("HomePropStillCarried"));
    if (!HasRestoreSpaceAndSupport(Target,true,OutReason,Support,12.f,.7f,bAllowRestingFurniture)) return false;
    if (!IsCarryScaleActive()) { OutReason=NAME_None; return true; }
    const uint32 Generation=ScaleGeneration;
    const TWeakObjectPtr<UDeliveryHomeObjectComponent> WeakThis(this);
    const TWeakObjectPtr<UPrimitiveComponent> WeakBody(Body);
    const TWeakObjectPtr<UDeliveryGrabbableComponent> WeakGrab(Grabbable);
    const TWeakObjectPtr<AActor> WeakActor(Actor);
    TGuardValue<bool> ChangeGuard(bChangingCarryScale,true);
    // 目标已验证全过程；竖直底面补偿与尺度在同一次变换更新中生效，保留当前物理速度
    Actor->SetActorTransform(Target,false,nullptr,ETeleportType::TeleportPhysics);
    if (!WeakThis.IsValid()) return false;
    if (bEndingPlay || Generation!=ScaleGeneration || !WeakBody.IsValid() || !WeakGrab.IsValid() ||
        !WeakActor.IsValid() || Actor->IsActorBeingDestroyed() || Actor->GetRootComponent()!=WeakBody.Get() ||
        ScaleRoot.Get()!=WeakBody.Get())
        return Fail(OutReason,TEXT("HomeScaleRestoreChanged"));
    // 变换通知可同步释放或重拿，清理前确认实际尺度已经恢复且没有新搬运者
    if (Grabbable->HasAnyCarrier() || Actor->GetOwner() || Actor->GetAttachParentActor() ||
        !Actor->GetActorScale3D().Equals(CarryScaleState.OriginalActorScale,.001))
        return Fail(OutReason,TEXT("HomeScaleRestoreChanged"));
    CarryScaleState.OriginalActorScale=FVector::OneVector;
    ScaleRoot.Reset();
    SetCarryScalePhase(EDeliveryHomeCarryScalePhase::Original);
    OutReason=NAME_None;
    return true;
}

bool UDeliveryHomeObjectComponent::TryRestoreScaleAt(const FTransform& FullSizeGoal,FName& OutReason,const UDeliveryHomeObjectComponent* Support)
{
    if (!CanRestoreScaleAt(FullSizeGoal,OutReason,Support)) return false;
    const FTransform Actual=GetOwner()->GetActorTransform();
    FVector SmallGoal;
    if (!GetCarryPlacementLocation(FullSizeGoal,SmallGoal) || FVector::Dist(Actual.GetLocation(),SmallGoal)>4 ||
        FMath::RadiansToDegrees(Actual.GetRotation().AngularDistance(FullSizeGoal.GetRotation()))>3)
        return Fail(OutReason,TEXT("HomePropNotAtPlacement"));
    FTransform Restored=Actual;
    Restored.AddToTranslation(FVector(0,0,FullSizeGoal.GetLocation().Z-SmallGoal.Z));
    Restored.SetScale3D(GetOriginalActorScale());
    return RestoreScale(Restored,OutReason,Support);
}

void UDeliveryHomeObjectComponent::QueueScaleRestore()
{
    if (bEndingPlay || !GetOwner() || !GetOwner()->HasAuthority() || !IsCarryScaleActive()) return;
    const auto* Grabbable=GetOwner()->FindComponentByClass<UDeliveryGrabbableComponent>();
    SetCarryScalePhase(Grabbable && Grabbable->HasAnyCarrier()?EDeliveryHomeCarryScalePhase::CarriedSmall:
        EDeliveryHomeCarryScalePhase::WaitingToRestore);
}

void UDeliveryHomeObjectComponent::UpdateScaleRestore()
{
    if (!IsCarryScaleActive()) return;
    FName Reason;
    if (!ValidateScaleObject(Reason)) return;
    auto* Actor=GetOwner();
    const auto* Grabbable=Actor->FindComponentByClass<UDeliveryGrabbableComponent>();
    if (Grabbable->HasAnyCarrier() || Actor->GetOwner() || Actor->GetAttachParentActor())
    { SetCarryScalePhase(EDeliveryHomeCarryScalePhase::CarriedSmall); return; }
    SetCarryScalePhase(EDeliveryHomeCarryScalePhase::WaitingToRestore);
    const auto* Body=Cast<UPrimitiveComponent>(Actor->GetRootComponent());
    if (!Body->IsSimulatingPhysics() || Body->GetPhysicsLinearVelocity().Size()>10 ||
        Body->GetPhysicsAngularVelocityInDegrees().Size()>10)
    { NextScaleRestoreOffsetAt=-1; return; }
    FTransform Target=Actor->GetActorTransform();
    Target.SetScale3D(GetOriginalActorScale());
    if (bRestoreScaleFromSupportBottom)
    {
        const FBox Current=CalculateSizeBounds(Actor->GetActorTransform()),Full=GetOriginalSizeBounds(Target);
        if (!Current.IsValid || !Full.IsValid) return;
        const double Lift=Current.Min.Z-Full.Min.Z;
        if (!FMath::IsFinite(Lift) || Lift<-.5) return;
        Target.AddToTranslation(FVector(0,0,FMath::Max(0.,Lift)));
    }
    const FName Blocked(TEXT("HomeScaleRestoreBlocked"));
    const FName Unsupported(TEXT("HomeScaleRestoreUnsupported"));
    const TWeakObjectPtr<UDeliveryHomeObjectComponent> WeakThis(this);
    if (RestoreScale(Target,Reason,nullptr,true) || !WeakThis.IsValid() || (Reason!=Blocked && Reason!=Unsupported)) return;
    const double Now=GetWorld()->GetTimeSeconds();
    if (NextScaleRestoreOffsetAt<0)
    {
        NextScaleRestoreOffsetAt=Now+.75;
        UE_LOG(LogDeliveryHomeObject,Log,TEXT("HomeScaleRestoreDeferred object=%s reason=%s location=%s current=%s original=%s"),
            *GetNameSafe(Actor),*Reason.ToString(),*Actor->GetActorLocation().ToCompactString(),
            *Actor->GetActorScale3D().ToCompactString(),*GetOriginalActorScale().ToCompactString());
        return;
    }
    if (Now<NextScaleRestoreOffsetAt) return;
    NextScaleRestoreOffsetAt=Now+1;
    // 原地受阻后只试同一高度附近的落点，完整包络覆盖小物件当前位置到全尺寸目标
    const FVector2D Directions[]={FVector2D(1,0),FVector2D(-1,0),FVector2D(0,1),FVector2D(0,-1),
        FVector2D(1,1),FVector2D(1,-1),FVector2D(-1,1),FVector2D(-1,-1)};
    for (const float Distance:{24.f,48.f,72.f})
        for (const FVector2D& Direction:Directions)
        {
            const FVector2D Offset=Direction.GetSafeNormal()*Distance;
            FTransform Nearby=Target;
            Nearby.AddToTranslation(FVector(Offset.X,Offset.Y,0));
            if (RestoreScale(Nearby,Reason,nullptr,true) || !WeakThis.IsValid() ||
                (Reason!=Blocked && Reason!=Unsupported)) return;
        }
}

void UDeliveryHomeObjectComponent::ReleaseUse(const FDeliveryHomeUseClaim& Claim,FName Reason)
{
    if (!IsInGameThread() || !GetOwner() || !GetOwner()->HasAuthority()) return;
    const auto* Use=FindOwned(Claim);
    if (!Use || Use->bPlacementCommitted) return;
    ReleaseOwned(Claim.Token,Reason);
}

void UDeliveryHomeObjectComponent::ReleaseOwned(FGuid Token,FName Reason,bool bSlotAlreadyInvalid)
{
    FOwnedUse Use;
    if (!OwnedUses.RemoveAndCopyValue(Token,Use)) return;
    const TWeakObjectPtr<UDeliveryHomeObjectComponent> WeakThis(this);
    if (auto* Subsystem=GetWorld()?GetWorld()->GetSubsystem<USmartObjectSubsystem>():nullptr;
        Subsystem && !bSlotAlreadyInvalid && IsCurrentSmartObjectClaim(Use))
    {
        Subsystem->UnregisterSlotInvalidationCallback(Use.Handle);
        Subsystem->MarkSlotAsFree(Use.Handle);
    }
    if (!WeakThis.IsValid()) return;
    RefreshReplicatedState();
    if (!Use.bExecutionEnded || Use.bPlacementCommitted) OnUseEnded.Broadcast(Use.Ticket.ActionId,Reason);
}

void UDeliveryHomeObjectComponent::HandleSlotInvalidated(FSmartObjectClaimHandle Handle)
{
    for (const auto& Pair:OwnedUses)
        if (Pair.Value.Handle==Handle) { ReleaseOwned(Pair.Key,TEXT("HomeSlotInvalidated"),true); return; }
}

void UDeliveryHomeObjectComponent::RefreshReplicatedState()
{
    UseStates.Reset();
    const auto* Subsystem=GetWorld()?GetWorld()->GetSubsystem<USmartObjectSubsystem>():nullptr;
    for (const auto& Pair:OwnedUses)
    {
        const auto& Use=Pair.Value;
        const auto* Definition=Use.Definition.Get();
        if (!Definition || !Subsystem || !IsCurrentSmartObjectClaim(Use)) continue;
        FDeliveryHomeUseState State;
        State.SlotId=Definition->SlotId; State.UseId=Definition->UseId; State.Operation=Definition->Operation;
        State.ActionId=Use.Ticket.ActionId;
        State.Phase=Use.bPlacementCommitted?EDeliveryHomeUsePhase::Placed:
            Subsystem->GetSlotState(Use.Handle.SlotHandle)==ESmartObjectSlotState::Occupied?EDeliveryHomeUsePhase::Using:EDeliveryHomeUsePhase::Approaching;
        State.User=Use.bPlacementCommitted?Use.PlacementProp.Get():Use.Companion.Get();
        UseStates.Add(State);
    }
    ++Revision;
    if (GetOwner()) GetOwner()->ForceNetUpdate();
}

void UDeliveryHomeObjectComponent::TickComponent(float DeltaTime,ELevelTick Type,FActorComponentTickFunction* Function)
{
    Super::TickComponent(DeltaTime,Type,Function);
    if (!GetOwner() || !GetOwner()->HasAuthority() || bEndingPlay) return;
    const TWeakObjectPtr<UDeliveryHomeObjectComponent> WeakScaleThis(this);
    UpdateScaleRestore();
    if (!WeakScaleThis.IsValid() || bEndingPlay) return;
    if (auto* Smart=ResolveSmartObject())
        if (auto* Subsystem=GetWorld()->GetSubsystem<USmartObjectSubsystem>(); Subsystem && Smart->GetRegisteredHandle().IsValid() &&
            (LastSmartObjectHandle!=Smart->GetRegisteredHandle() || !LastSmartObjectTransform.Equals(Smart->GetComponentTransform())))
        {
            if (Subsystem->UpdateSmartObjectTransform(Smart->GetRegisteredHandle(),Smart->GetComponentTransform()))
            { LastSmartObjectHandle=Smart->GetRegisteredHandle(); LastSmartObjectTransform=Smart->GetComponentTransform(); }
        }
    TArray<FGuid> Tokens; OwnedUses.GetKeys(Tokens);
    const TWeakObjectPtr<UDeliveryHomeObjectComponent> WeakThis(this);
    for (const FGuid Token:Tokens)
    {
        const auto* Use=OwnedUses.Find(Token);
        if (!Use) continue;
        FName Reason;
        const bool bValid=Use->bPlacementCommitted?
            ValidateHome(Reason) && IsCurrentSmartObjectClaim(*Use) && IsPlacementValid(*Use,Use->PlacementProp.Get(),Reason):
            ValidateClaim(Use->Ticket,GetWorld()->GetSubsystem<USmartObjectSubsystem>() &&
                GetWorld()->GetSubsystem<USmartObjectSubsystem>()->GetSlotState(Use->Handle.SlotHandle)==ESmartObjectSlotState::Occupied,Reason);
        if (!bValid) ReleaseOwned(Token,Reason.IsNone()?FName(TEXT("HomeClaimExpired")):Reason);
        if (!WeakThis.IsValid()) return;
    }
}

double UDeliveryHomeObjectComponent::GetServerTime() const
{
    const auto* State=GetWorld()?GetWorld()->GetGameState():nullptr;
    return State?State->GetServerWorldTimeSeconds():GetWorld()?GetWorld()->GetTimeSeconds():0;
}

void UDeliveryHomeObjectComponent::OnRep_Audio()
{
    auto* Audio=ResolveAudio();
    const TWeakObjectPtr<UDeliveryHomeObjectComponent> WeakThis(this);
    if (!bAudioPlaying || bEndingPlay)
    {
        if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(AudioReadyTimer);
        PresentedSound.Reset(); PresentedStartTime=-1;
        if (Audio) Audio->Stop();
        return;
    }
    const bool bNeedsServerClock=GetOwner() && !GetOwner()->HasAuthority() &&
        (!GetWorld() || !IsValid(GetWorld()->GetGameState()));
    if (!Audio || !IsValid(ActiveSound) || bNeedsServerClock)
    {
        // 不把本地时钟算出的错误偏移记作已播放，晚加入可在引用就绪后重新定位
        PresentedSound.Reset(); PresentedStartTime=-1;
        if (Audio) Audio->Stop();
        if (WeakThis.IsValid()) RetryAudioWhenReady();
        return;
    }
    if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(AudioReadyTimer);
    if (PresentedSound.Get()==ActiveSound && PresentedStartTime==AudioStartedServerTime) return;
    const TWeakObjectPtr<USoundBase> Sound(ActiveSound);
    const double StartedTime=AudioStartedServerTime;
    PresentedSound=Sound; PresentedStartTime=StartedTime;
    Audio->Stop();
    // Stop 可能触发旧音频回调，先确认当前播放仍属于本次状态
    if (!WeakThis.IsValid() || !IsValid(Audio) || !Sound.IsValid() || bEndingPlay || !bAudioPlaying ||
        ActiveSound!=Sound.Get() || AudioStartedServerTime!=StartedTime || ResolveAudio()!=Audio) return;
    if (!GetOwner()->HasAuthority() && (!GetWorld() || !IsValid(GetWorld()->GetGameState())))
    {
        PresentedSound.Reset(); PresentedStartTime=-1;
        RetryAudioWhenReady();
        return;
    }
    Audio->SetSound(Sound.Get());
    double Offset=FMath::Max(0.0,GetServerTime()-StartedTime);
    const float Duration=Sound->GetDuration();
    if (!Sound->IsLooping() && FMath::IsFinite(Duration) && Duration>0 && Offset>=Duration) return;
    Audio->Play(static_cast<float>(Offset));
    // 声源音量由资产配置，不改现有音乐组件的 MusicVolume 或全局 SoundClass
}

void UDeliveryHomeObjectComponent::RetryAudioWhenReady()
{
    if (!GetWorld() || bEndingPlay || !bAudioPlaying) return;
    GetWorld()->GetTimerManager().SetTimer(AudioReadyTimer,this,&UDeliveryHomeObjectComponent::OnRep_Audio,.1f,false);
}

void UDeliveryHomeObjectComponent::StopFinishedAudio(int32 ExpectedRevision)
{
    if (bEndingPlay || !GetOwner()->HasAuthority() || AudioGeneration!=ExpectedRevision || !bAudioPlaying) return;
    bAudioPlaying=false; ActiveSound=nullptr; ++AudioGeneration;
    RefreshReplicatedState();
    OnRep_Audio();
}

void UDeliveryHomeObjectComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    bEndingPlay=true;
    SetComponentTickEnabled(false);
    if (GetWorld())
    {
        GetWorld()->GetTimerManager().ClearTimer(AudioFinishTimer);
        GetWorld()->GetTimerManager().ClearTimer(AudioReadyTimer);
    }
    if (GetOwner() && GetOwner()->HasAuthority())
    {
        TArray<FGuid> Tokens; OwnedUses.GetKeys(Tokens);
        for (const FGuid Token:Tokens) ReleaseOwned(Token,TEXT("HomeObjectEndPlay"));
    }
    bAudioPlaying=false;
    OnRep_Audio();
    Super::EndPlay(Reason);
}

void UDeliveryHomeObjectComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UDeliveryHomeObjectComponent,Revision);
    DOREPLIFETIME(UDeliveryHomeObjectComponent,UseStates);
    DOREPLIFETIME(UDeliveryHomeObjectComponent,CarryScaleState);
    DOREPLIFETIME(UDeliveryHomeObjectComponent,bAudioPlaying);
    DOREPLIFETIME(UDeliveryHomeObjectComponent,ActiveSound);
    DOREPLIFETIME(UDeliveryHomeObjectComponent,AudioStartedServerTime);
}
