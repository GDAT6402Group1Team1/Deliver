#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionTuning.h"
#include "Character/DeliveryCharacter.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "NavigationSystem.h"
#include "World/Home/DeliveryHomeObjectComponent.h"

bool UDeliveryCompanionComponent::IsHomePlacementInFrontOfOwner(const APawn* Pawn,const FVector& GroundPoint,const AActor* Prop) const
{
    const auto* Player=Cast<ADeliveryCharacter>(Pawn);
    if (!IsValid(Player) || Player->IsActorBeingDestroyed() || Player->GetWorld()!=GetWorld() ||
        Player!=GetOwnerPawn() || Player->GetTaskPlayerState()!=OwnerPlayerState || Player->IsIncapacitated()) return false;
    const FVector Offset=GroundPoint-Player->GetNavAgentLocation();
    const FVector Forward=Player->GetActorForwardVector().GetSafeNormal2D();
    if (Offset.ContainsNaN() || Forward.IsNearlyZero() || FMath::Abs(Offset.Z)>45. || Offset.SizeSquared2D()>FMath::Square(460.) ||
        FVector::DotProduct(Offset.GetSafeNormal2D(),Forward)<.65) return false;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(HomeOwnerPlacementView),false,Player);
    Query.AddIgnoredActor(GetCompanion());
    Query.AddIgnoredActor(Prop);
    return !GetWorld()->LineTraceTestByChannel(Player->GetPawnViewLocation(),GroundPoint+FVector(0,0,9),ECC_Visibility,Query);
}

bool UDeliveryCompanionComponent::ResolveHomeOwnerPlacement(const UDeliveryHomeObjectComponent& Source,const APawn* Pawn,
    FTransform& Entrance,FTransform& Placement) const
{
    const auto* Npc=GetCompanion();
    const auto* Player=Cast<ADeliveryCharacter>(Pawn);
    const auto* Prop=Source.GetOwner();
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!IsValid(Npc) || !IsValid(Player) || !IsValid(Prop) || !Nav || Player!=GetOwnerPawn() ||
        Player->GetTaskPlayerState()!=OwnerPlayerState || Player->IsIncapacitated() || Prop->GetWorld()!=GetWorld()) return false;
    const FVector Feet=Player->GetNavAgentLocation();
    const FVector Forward=Player->GetActorForwardVector().GetSafeNormal2D();
    if (Feet.ContainsNaN() || Forward.IsNearlyZero()) return false;
    const FQuat Rotation=Forward.Rotation().Quaternion();
    const FBox Bounds=Source.GetOriginalSizeBounds(FTransform(Rotation));
    const FBox LocalBounds=Source.GetOriginalSizeBounds(FTransform::Identity);
    if (!Bounds.IsValid || !LocalBounds.IsValid || Bounds.Min.ContainsNaN() || Bounds.Max.ContainsNaN()) return false;
    const FVector Extent=LocalBounds.GetExtent();
    if (Extent.GetMin()<=0.) return false;
    const FVector RootOffset(Bounds.GetCenter().X,Bounds.GetCenter().Y,Bounds.Min.Z);
    const FVector Right=FVector::CrossProduct(FVector::UpVector,Forward);
    // 先试脚边仍容得下原尺寸的位置，受阻时再往前找
    const double Depth=FMath::Max(85.,Extent.X+Player->GetSimpleCollisionRadius()+18.);
    const double StepSide=FMath::Max(85.,Extent.Y+Npc->GetSimpleCollisionRadius()+22.);
    FCollisionObjectQueryParams GroundObjects(ECC_WorldStatic);
    GroundObjects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Query(SCENE_QUERY_STAT(HomeOwnerPlacementFloor),false,Prop);
    for (const double ExtraDepth:{0.,37.,112.}) for (const double Lateral:{0.,-65.,65.})
    {
        const FVector Near=Feet+Forward*(Depth+ExtraDepth)+Right*Lateral;
        FHitResult Ground;
        if (!GetWorld()->LineTraceSingleByObjectType(Ground,Near+FVector(0,0,45),Near-FVector(0,0,55),GroundObjects,Query) ||
            Ground.bStartPenetrating || !IsValid(Ground.GetComponent()) || Ground.GetComponent()->IsSimulatingPhysics() ||
            Ground.ImpactNormal.Z<.9 || !IsHomePlacementInFrontOfOwner(Pawn,Ground.ImpactPoint,Prop)) continue;
        const FTransform Target(Rotation,Ground.ImpactPoint-RootOffset,Source.GetOriginalActorScale());
        FName Reason;
        if (!Source.CanPlaceAtOriginalSize(Target,Reason)) continue;
        for (const double Side:{-1.,1.})
        {
            const FVector Stand=Ground.ImpactPoint+Right*(StepSide*Side);
            FNavLocation Projected;
            if (!Nav->ProjectPointToNavigation(Stand,Projected,FVector(20,20,65),&Npc->GetNavAgentPropertiesRef()) ||
                FVector::DistSquared2D(Stand,Projected.Location)>FMath::Square(20.) ||
                FMath::Abs(Projected.Location.Z-Ground.ImpactPoint.Z)>25. || !IsDestinationClear(Projected.Location)) continue;
            const FVector Facing=(Ground.ImpactPoint-Projected.Location).GetSafeNormal2D();
            Entrance=FTransform(Facing.Rotation(),Projected.Location);
            Placement=Target;
            return true;
        }
    }
    return false;
}

void UDeliveryCompanionComponent::UpdateHomeOwnerPlacementApproach(FGuid Id,double Now)
{
    auto* Source=ActiveHomeChoice.SourceObject.Get();
    const auto* Player=ActiveHomeChoice.PlacementOwnerPawn.Get();
    if (!OwnsHomeSource(Source))
    { FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,TEXT("HomeCarriedPropChanged")); return; }
    if (!IsValid(Player) || Player!=GetOwnerPawn())
    { FinishHomeArrangement(EDeliveryCompanionHomePhase::Cancelled,TEXT("HomePlacementOwnerChanged"),true); return; }
    const auto* Prop=Source->GetOwner();
    const FBox Bounds=Source->GetOriginalSizeBounds(PropPlacementTarget);
    const FVector GroundPoint(Bounds.GetCenter().X,Bounds.GetCenter().Y,Bounds.Min.Z);
    if (bHasOwnerPlacementTarget && !IsHomePlacementInFrontOfOwner(Player,GroundPoint,Prop)) bHasOwnerPlacementTarget=false;
    if (!bHasOwnerPlacementTarget)
    {
        if (Now<NextHomeNavigationAt) return;
        if (!ResolveHomeOwnerPlacement(*Source,Player,OwnerPlacementEntrance,PropPlacementTarget))
        {
            NextHomeNavigationAt=Now+GetTuning().NavigationInterval;
            if (HomeOwnerPlacementBlockedSince<0) HomeOwnerPlacementBlockedSince=Now;
            // 主人面前一直放不下原尺寸时，先走到主人身边，再把东西放在脚边，不让它一直卡在手里
            if (Now-HomeOwnerPlacementBlockedSince>=3.)
            {
                const auto* Npc=GetCompanion();
                const bool bBesideOwner=Npc && FVector::DistSquared2D(Npc->GetActorLocation(),Player->GetActorLocation())<=FMath::Square(200.f);
                if (bBesideOwner || Now-HomeOwnerPlacementBlockedSince>=18.)
                { FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,TEXT("HomeDroppedAtFeet")); return; }
                MoveForHome(Player->GetActorLocation(),150.f);
                if (!IsCurrentHome(Id)) return;
                SetSuspension(TEXT("HomeWalkingToOwnerToDrop"));
                return;
            }
            StopHomeMovement();
            if (!IsCurrentHome(Id)) return;
            SetSuspension(TEXT("HomeOwnerPlacementUnavailable"));
            return;
        }
        bHasOwnerPlacementTarget=true;
    }
    const bool bReached=MoveForHome(OwnerPlacementEntrance.GetLocation(),FMath::Max(HomeUseReachTolerance,12.f));
    LastHomeUpdateAt=Now;
    if (!bReached || !IsCurrentHome(Id) || !AlignForHome(OwnerPlacementEntrance)) return;
    if (!IsHomePropReadyAtHand(Now)) { SetSuspension(TEXT("HomePropReturningToHand")); return; }
    FName Reason;
    if (!Source->CanPlaceAtOriginalSize(PropPlacementTarget,Reason))
    {
        bHasOwnerPlacementTarget=false;
        NextHomeNavigationAt=Now+GetTuning().NavigationInterval;
        SetSuspension(Reason);
        return;
    }
    bPlacingHomeProp=true;
    BeginHomePropSegment(false);
    SetHomePhase(EDeliveryCompanionHomePhase::PlaceProp);
}
