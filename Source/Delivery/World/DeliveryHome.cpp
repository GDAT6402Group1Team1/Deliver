#include "World/DeliveryHome.h"
#include "World/DeliveryHomeObjectComponent.h"
#include "World/DeliveryLocationComponent.h"
#include "World/DeliveryLocationRegistry.h"
#include "NPC/DeliveryGrandpa.h"
#include "Task/World/DeliveryBreakableDoor.h"
#include "NavLinkCustomComponent.h"
#include "NavAreas/NavArea_Default.h"
#include "NavAreas/NavArea_Null.h"
#include "Components/SceneComponent.h"
#include "GameFramework/GameStateBase.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

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
    DoorwayLink->SetEnabledArea(UNavArea_Default::StaticClass());
    DoorwayLink->SetDisabledArea(UNavArea_Null::StaticClass());
    DoorwayLink->SetEnabled(false);
}

void ADeliveryHome::BeginPlay()
{
    Super::BeginPlay();
    DoorwayLink->SetEnabled(false);
    if (!HasAuthority()) return;
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
    const auto* State=GetWorld()->GetGameState();
    ClaimedServerTime=State?State->GetServerWorldTimeSeconds():GetWorld()->GetTimeSeconds();
    ForceNetUpdate();
    return true;
}

void ADeliveryHome::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ADeliveryHome,bClaimed);
    DOREPLIFETIME(ADeliveryHome,ClaimEventId);
    DOREPLIFETIME(ADeliveryHome,ClaimedServerTime);
}
