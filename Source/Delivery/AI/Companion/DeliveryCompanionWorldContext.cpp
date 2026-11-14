#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "World/DeliveryLocationComponent.h"
#include "World/DeliveryLocationRegistry.h"
#include "AIController.h"
#include "AIHelpers.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISense_Sight.h"
#include "Engine/World.h"

namespace
{
constexpr int32 MaxObservedPlaces=8;
constexpr int32 MaxPlaceSightQueries=32;
constexpr float MaxPlaceObservationRadius=3000;

FString PublicPlaceText(const FText& Value,int32 Limit)
{
    FString Result;
    for (TCHAR Character:Value.ToString())
    {
        Result.AppendChar(Character>=32 && Character!=127?Character:TCHAR(' '));
        if (Result.Len()>=Limit) break;
    }
    return Result.TrimStartAndEnd();
}

bool IsFinitePoint(const FVector& Point)
{
    return FMath::IsFinite(Point.X) && FMath::IsFinite(Point.Y) && FMath::IsFinite(Point.Z);
}

struct FObservedPlaceCandidate
{
    TWeakObjectPtr<UDeliveryLocationComponent> Location;
    FVector Anchor=FVector::ZeroVector;
    double DistanceSquared=0;
    FString SortKey;
};
}

void UDeliveryCompanionComponent::ExtendWorldDecisionRequest(FDeliveryNpcDecisionRequest& Request) const
{
    Request.CompanionObservedPlaces.Reset();
    const auto* Npc=GetCompanion();
    UWorld* World=GetWorld();
    if (!IsValid(Npc) || !Npc->HasAuthority() || !World || Npc->GetWorld()!=World ||
        Npc->IsActorBeingDestroyed() || Npc->IsHidden() || Npc->IsKnockedDown()) return;
    const auto* Controller=Cast<AAIController>(Npc->GetController());
    const auto* Perception=Controller?Controller->GetAIPerceptionComponent():nullptr;
    const auto* Sight=Perception?Perception->GetSenseConfig<UAISenseConfig_Sight>():nullptr;
    const auto* Registry=World->GetSubsystem<UDeliveryLocationRegistry>();
    // 停用视觉或离开本世界时，本次上下文不能继续声称看见公开地点
    if (!Sight || !Registry || Controller->GetWorld()!=World || Perception->GetWorld()!=World ||
        !Perception->IsRegistered() || !Perception->IsSenseEnabled(UAISense_Sight::StaticClass()) ||
        !FMath::IsFinite(Sight->SightRadius) || Sight->SightRadius<=0 ||
        !FMath::IsFinite(Sight->PeripheralVisionAngleDegrees) ||
        !FMath::IsFinite(Sight->PointOfViewBackwardOffset) || Sight->PointOfViewBackwardOffset<0 ||
        Sight->PointOfViewBackwardOffset>MaxPlaceObservationRadius ||
        !FMath::IsFinite(Sight->NearClippingRadius) || Sight->NearClippingRadius<0 ||
        Sight->NearClippingRadius>2*MaxPlaceObservationRadius) return;
    const float Radius=FMath::Min(Sight->SightRadius,MaxPlaceObservationRadius);
    FVector Eye=FVector::ZeroVector;
    FVector Direction=FVector::ZeroVector;
    Perception->GetLocationAndDirection(Eye,Direction);
    if (!IsFinitePoint(Eye) || !IsFinitePoint(Direction) || !Direction.Normalize()) return;
    const float PeripheralCos=FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Sight->PeripheralVisionAngleDegrees,0.f,180.f)));
    const float FarRadiusSquared=FMath::Square(Radius+Sight->PointOfViewBackwardOffset);

    TArray<FObservedPlaceCandidate> Nearby;
    for (FName RegisteredId:Registry->GetRegisteredIds())
    {
        for (auto* Location:Registry->ResolveAllComponents(RegisteredId))
        {
            const auto* PlaceActor=IsValid(Location)?Location->GetOwner():nullptr;
            if (!IsValid(PlaceActor) || PlaceActor==Npc || PlaceActor->IsHidden() ||
                Location->GetWorld()!=World || PlaceActor->GetWorld()!=World || !Location->IsRegistered() ||
                Location->LocationId!=RegisteredId || !Location->bDescribeToNpcs ||
                PublicPlaceText(Location->NpcDisplayName,64).IsEmpty()) continue;
            const FTransform ObservationTransform=Location->GetComponentTransform();
            if (!IsFinitePoint(Location->NpcObservationOffset) || ObservationTransform.ContainsNaN() ||
                !ObservationTransform.GetRotation().IsNormalized()) continue;
            const FVector Anchor=ObservationTransform.TransformPosition(Location->NpcObservationOffset);
            if (!IsFinitePoint(Anchor)) continue;
            const double DistanceSquared=FVector::DistSquared(Eye,Anchor);
            if (!FMath::IsFinite(DistanceSquared) || DistanceSquared>FMath::Square(Radius)) continue;
            // 复用 UE 当前视锥定义，不用登记过地点代替这次确实朝它看见
            if (!FAISystem::CheckIsTargetInSightCone(Eye,Direction,PeripheralCos,Sight->PointOfViewBackwardOffset,
                FMath::Square(Sight->NearClippingRadius),FarRadiusSquared,Anchor)) continue;
            FObservedPlaceCandidate Candidate;
            Candidate.Location=Location;
            Candidate.Anchor=Anchor;
            Candidate.DistanceSquared=DistanceSquared;
            Candidate.SortKey=RegisteredId.ToString()+TEXT("|")+Location->GetPathName();
            Nearby.Add(MoveTemp(Candidate));
        }
    }
    Nearby.Sort([](const FObservedPlaceCandidate& A,const FObservedPlaceCandidate& B)
    {
        if (A.DistanceSquared!=B.DistanceSquared) return A.DistanceSquared<B.DistanceSquared;
        return A.SortKey<B.SortKey;
    });

    // 公开观察点应在可见空处，连终点命中也拒绝，不忽略建筑或门
    const FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionObservedPlace),false,Npc);
    const int32 QueryCount=FMath::Min(Nearby.Num(),MaxPlaceSightQueries);
    for (int32 Index=0;Index<QueryCount && Request.CompanionObservedPlaces.Num()<MaxObservedPlaces;++Index)
    {
        const auto& Candidate=Nearby[Index];
        const auto* Location=Candidate.Location.Get();
        if (!Location) continue;
        FHitResult Hit;
        if (World->LineTraceSingleByChannel(Hit,Eye,Candidate.Anchor,ECC_Visibility,Params)) continue;
        FDeliveryNpcObservedPlace Place;
        Place.PlaceId=FString::Printf(TEXT("place_%d"),Request.CompanionObservedPlaces.Num());
        Place.Name=PublicPlaceText(Location->NpcDisplayName,64);
        Place.Description=PublicPlaceText(Location->NpcDescription,160);
        Place.DistanceCm=static_cast<float>(FMath::Sqrt(Candidate.DistanceSquared));
        Request.CompanionObservedPlaces.Add(MoveTemp(Place));
    }
}
