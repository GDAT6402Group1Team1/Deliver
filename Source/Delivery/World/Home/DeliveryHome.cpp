#include "World/Home/DeliveryHome.h"
#include "World/Home/DeliveryHomeObjectComponent.h"
#include "World/Location/DeliveryLocationComponent.h"
#include "World/Location/DeliveryLocationRegistry.h"
#include "World/DeliveryNavigationAreas.h"
#include "NPC/Grandpa/DeliveryGrandpa.h"
#include "Task/World/DeliveryBreakableDoor.h"
#include "NavLinkCustomComponent.h"
#include "NavAreas/NavArea_Null.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "Delivery.h"

ADeliveryHome::ADeliveryHome()
{
    bReplicates=true;
    bAlwaysRelevant=true;
    SetReplicateMovement(true);
    LocationId=TEXT("Location_PlayerHome");
    LocationComponent->LocationId=LocationId;
    ReturnAnchor=CreateDefaultSubobject<USceneComponent>(TEXT("ReturnAnchor"));
    ReturnAnchor->SetupAttachment(GetRootComponent());
    InteriorAnchor=CreateDefaultSubobject<USceneComponent>(TEXT("InteriorAnchor"));
    InteriorAnchor->SetupAttachment(GetRootComponent());
    InteriorAnchor->SetRelativeLocation(FVector(0,480,0));
    DoorwayLink=CreateDefaultSubobject<UNavLinkCustomComponent>(TEXT("DoorwayLink"));
    DoorwayLink->SetLinkData(FVector(0,20,-35),FVector(0,220,0),ENavLinkDirection::BothWays);
    DoorwayLink->SetEnabledArea(UDeliveryPedestrianLinkArea::StaticClass());
    DoorwayLink->SetDisabledArea(UNavArea_Null::StaticClass());
    DoorwayLink->SetEnabled(false);
    // 解锁反馈素材来自 Design/Art/VFX/HomeClaim/Selected，实例上可以换
    ClaimStarTexture=FSoftObjectPath(TEXT("/Game/UI/HomeClaim/T_HomeClaim_Star01.T_HomeClaim_Star01"));
    ClaimSparkTexture=FSoftObjectPath(TEXT("/Game/UI/HomeClaim/T_HomeClaim_Star02.T_HomeClaim_Star02"));
    ClaimMarkerTexture=FSoftObjectPath(TEXT("/Game/UI/HomeClaim/T_HomeClaim_Marker.T_HomeClaim_Marker"));
    ClaimSound=FSoftObjectPath(TEXT("/Game/UI/HomeClaim/SFX_HomeClaim_Confirm.SFX_HomeClaim_Confirm"));
    ClaimFallbackSound=FSoftObjectPath(TEXT("/Game/Sound/SFX/SFX_Task_Delivered.SFX_Task_Delivered"));
}

void ADeliveryHome::BeginPlay()
{
    Super::BeginPlay();
    DoorwayLink->SetEnabled(false);
    // 有画面的机器都挂上反馈层，晚加入时只会看到门口的持续标记
    if (GetNetMode()!=NM_DedicatedServer)
        GetWorldTimerManager().SetTimer(ClaimOverlayTimer,this,&ADeliveryHome::EnsureClaimOverlay,.5f,true,.1f);
    if (!HasAuthority()) return;
    // 旧地图实例的链接沿用端点和方向，启用时登记行人区域
    DoorwayLink->SetEnabledArea(UDeliveryPedestrianLinkArea::StaticClass());
    auto* Door=IsValid(FormerResident)?FormerResident->FrontDoor.Get():nullptr;
    if (IsValid(Door) && Door->GetWorld()==GetWorld())
    {
        BoundDoor=Door;
        Door->OnDoorBroken.AddUniqueDynamic(this,&ADeliveryHome::RefreshDoorwayNavigation);
        Door->OnDestroyed.AddUniqueDynamic(this,&ADeliveryHome::OnDoorDestroyed);
    }
    RefreshDoorwayNavigation();
}

void ADeliveryHome::RefreshDoorwayNavigation()
{
    // 破门只开放导航边，不认领房屋、不移动人物；碎片和行人仍由碰撞处理
    DoorwayLink->SetEnabled(HasAuthority() && FindSharedHome(this)==this && BoundDoor.IsValid() &&
        BoundDoor->GetWorld()==GetWorld() && BoundDoor->IsBroken());
}

void ADeliveryHome::OnDoorDestroyed(AActor* DestroyedActor)
{
    BoundDoor.Reset();
    DoorwayLink->SetEnabled(false);
}

void ADeliveryHome::EndPlay(const EEndPlayReason::Type Reason)
{
    GetWorldTimerManager().ClearTimer(ClaimOverlayTimer);
    RemoveClaimOverlay();
    if (auto* Door=BoundDoor.Get())
    {
        Door->OnDoorBroken.RemoveDynamic(this,&ADeliveryHome::RefreshDoorwayNavigation);
        Door->OnDestroyed.RemoveDynamic(this,&ADeliveryHome::OnDoorDestroyed);
    }
    BoundDoor.Reset();
    DoorwayLink->SetEnabled(false);
    Super::EndPlay(Reason);
}

ADeliveryHome* ADeliveryHome::FindSharedHome(const UObject* WorldContextObject)
{
    const auto* Registry=UDeliveryLocationRegistry::Get(WorldContextObject);
    if (!Registry) return nullptr;
    const auto Homes=Registry->ResolveAllActors(TEXT("Location_PlayerHome"));
    // 重复标记不选择其中一份，避免不同玩家使用不同房屋状态
    return Homes.Num()==1?Cast<ADeliveryHome>(Homes[0]):nullptr;
}

bool ADeliveryHome::CrossesPedestrianDoorway(const FVector& FromFeet,const FVector& ToFeet) const
{
    const auto* Door=BoundDoor.Get();
    const auto* Mesh=IsValid(Door) && !Door->IsActorBeingDestroyed()?Door->GetIntactMesh():nullptr;
    if (!IsValid(DoorwayLink) || !IsValid(Mesh) || !Mesh->IsRegistered() || FromFeet.ContainsNaN() || ToFeet.ContainsNaN()) return false;
    const FVector Start=DoorwayLink->GetStartPoint(), End=DoorwayLink->GetEndPoint();
    const FVector DoorCenter=Door->GetActorLocation();
    if (Start.ContainsNaN() || End.ContainsNaN() || DoorCenter.ContainsNaN()) return false;
    const FVector Axis=(End-Start).GetSafeNormal2D();
    const double DoorAlongLink=FVector::DotProduct(DoorCenter-Start,Axis);
    if (DoorAlongLink<=0. || DoorAlongLink>=FVector::Dist2D(Start,End) ||
        FVector::DistSquared2D(DoorCenter,Start+Axis*DoorAlongLink)>FMath::Square(150.)) return false;
    const double FromSide=FVector::DotProduct(FromFeet-DoorCenter,Axis);
    const double ToSide=FVector::DotProduct(ToFeet-DoorCenter,Axis);
    if (FromSide*ToSide>=0.) return false;
    const FVector Crossing=FMath::Lerp(FromFeet,ToFeet,FromSide/(FromSide-ToSide));
    constexpr double FloorTolerance=75.;
    if (!FMath::IsWithinInclusive(Crossing.Z,FMath::Min(Start.Z,End.Z)-FloorTolerance,FMath::Max(Start.Z,End.Z)+FloorTolerance)) return false;
    // 使用真实门叶的宽度，平面以外的路缘和上层通道可以照常通过
    const FVector Lateral=FVector::CrossProduct(FVector::UpVector,Axis);
    const double DoorHalfWidth=FVector::DotProduct(Mesh->Bounds.BoxExtent,Lateral.GetAbs());
    return DoorHalfWidth>0. && FMath::Abs(FVector::DotProduct(Crossing-Mesh->Bounds.Origin,Lateral))<=DoorHalfWidth;
}

bool ADeliveryHome::TryGetReturnLocation(FVector& OutLocation) const
{
    if (!bClaimed || FindSharedHome(this)!=this || !IsValid(ReturnAnchor)) return false;
    OutLocation=ReturnAnchor->GetComponentLocation();
    return !OutLocation.ContainsNaN();
}

bool ADeliveryHome::GetDeparturePoint(int32 Index,FVector& OutLocation) const
{
    if (!DepartureRoute.IsValidIndex(Index) || DepartureRoute[Index].ContainsNaN()) return false;
    OutLocation=GetActorTransform().TransformPosition(DepartureRoute[Index]);
    return !OutLocation.ContainsNaN();
}

bool ADeliveryHome::TryGetInteriorLocation(FVector& OutLocation) const
{
    if (!bClaimed || FindSharedHome(this)!=this || !IsValid(InteriorAnchor)) return false;
    OutLocation=InteriorAnchor->GetComponentLocation();
    return !OutLocation.ContainsNaN();
}

void ADeliveryHome::GetFurnitureObjects(TArray<UDeliveryHomeObjectComponent*>& OutObjects) const
{
    OutObjects.Reset();
    TSet<FGuid> Ids;
    TSet<FGuid> DuplicateIds;
    TSet<AActor*> Actors;
    for (AActor* Actor:Furnishings)
    {
        if (!IsValid(Actor) || Actor->IsActorBeingDestroyed() || Actor->GetWorld()!=GetWorld() || Actors.Contains(Actor)) continue;
        Actors.Add(Actor);
        TInlineComponentArray<UDeliveryHomeObjectComponent*> Objects(Actor);
        if (Objects.Num()!=1) continue;
        auto* Object=Objects[0];
        if (!IsValid(Object) || Object->Home!=this || !Object->ObjectId.IsValid() || Object->DisplayName.IsEmpty()) continue;
        if (Ids.Contains(Object->ObjectId)) DuplicateIds.Add(Object->ObjectId);
        Ids.Add(Object->ObjectId);
        OutObjects.Add(Object);
    }
    // 重复身份整组排除，模型不能把两件实物当成同一件
    OutObjects.RemoveAll([&](const auto* Object){return DuplicateIds.Contains(Object->ObjectId);});
    OutObjects.Sort([](const auto& A,const auto& B)
    { return A.ObjectId.ToString()<B.ObjectId.ToString(); });
}

bool ADeliveryHome::ContainsFurniture(const UDeliveryHomeObjectComponent* Object) const
{
    TArray<UDeliveryHomeObjectComponent*> Objects;
    GetFurnitureObjects(Objects);
    return Objects.Contains(Object);
}

bool ADeliveryHome::TryClaimAfterDeparture(ADeliveryGrandpa* Resident)
{
    if (!HasAuthority() || GetNetMode()==NM_Client || FindSharedHome(this)!=this || !IsValid(ExistingHouse) ||
        ExistingHouse->GetWorld()!=GetWorld() || !IsValid(Resident) || Resident!=FormerResident ||
        Resident->GetWorld()!=GetWorld() || Resident->SharedHome!=this || !Resident->HasConfirmedHomeDelivery() ||
        !Resident->HasMovedOut()) return false;
    if (bClaimed) return true;
    bClaimed=true;
    ClaimEventId=Resident->GetConfirmedDeliveryId();
    ClaimedBy=Resident->GetUrgedBy();
    const auto* State=GetWorld()->GetGameState();
    ClaimedServerTime=State?State->GetServerWorldTimeSeconds():GetWorld()->GetTimeSeconds();
    UE_LOG(LogDelivery,Log,TEXT("SharedHomeClaimed home=%s delivery=%s at=%s"),
        *GetName(),*ClaimEventId.ToString(),*Resident->GetActorLocation().ToCompactString());
    ForceNetUpdate();
    // 监听服务器上的本机玩家不会收到复制通知，这里直接提示
    ShowClaimHint();
    return true;
}

void ADeliveryHome::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ADeliveryHome,bClaimed);
    DOREPLIFETIME(ADeliveryHome,ClaimEventId);
    DOREPLIFETIME(ADeliveryHome,ClaimedBy);
    DOREPLIFETIME(ADeliveryHome,ClaimedServerTime);
}
