#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimSequence.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "Delivery.h"

bool UDeliveryCompanionComponent::FindPackageGroundLocation(const ADeliveryPackageItem& Package,const APawn& Receiver,FVector& OutLocation) const
{
    const auto* Npc=GetCompanion();
    const auto* Mesh=Package.GetItemMesh();
    if (!IsValid(Npc) || !Mesh || !Mesh->IsRegistered() || !GetWorld() || Package.GetWorld()!=GetWorld() ||
        Receiver.GetWorld()!=GetWorld()) return false;
    const FVector Feet=Npc->GetNavAgentLocation();
    const FVector OwnerFeet=Receiver.GetNavAgentLocation();
    const FVector Extent=Mesh->Bounds.BoxExtent+FVector(2);
    const FVector PivotOffset=Mesh->Bounds.Origin-Package.GetActorLocation();
    if (Feet.ContainsNaN() || OwnerFeet.ContainsNaN() || Extent.ContainsNaN() || PivotOffset.ContainsNaN() ||
        Extent.GetMin()<=0 || Extent.GetMax()>100) return false;
    FVector Toward=(OwnerFeet-Feet).GetSafeNormal2D();
    if (Toward.IsNearlyZero()) Toward=Npc->GetActorForwardVector();
    FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionPlacePackage),false,Npc);
    Query.AddIgnoredActor(&Package);
    FCollisionQueryParams GroundQuery=Query;
    GroundQuery.AddIgnoredActor(&Receiver);
    const FCollisionShape Shape=FCollisionShape::MakeBox(Extent);
    // 只找双方能伸手够到的同层地面，不把包裹隔墙放到主人脚下
    for (const float Distance:{100.f,150.f,75.f}) for (const float Angle:{0.f,45.f,-45.f,90.f,-90.f})
    {
        const FVector Near=Feet+Toward.RotateAngleAxis(Angle,FVector::UpVector)*Distance;
        FHitResult Ground;
        if (!GetWorld()->LineTraceSingleByChannel(Ground,Near+FVector(0,0,100),Near-FVector(0,0,100),ECC_Visibility,GroundQuery) ||
            Ground.ImpactNormal.Z<.9 || FMath::Abs(Ground.ImpactPoint.Z-Feet.Z)>55 ||
            FMath::Abs(Ground.ImpactPoint.Z-OwnerFeet.Z)>75 || FVector::Dist2D(Ground.ImpactPoint,OwnerFeet)>180) continue;
        bool bSupported=true;
        for (const float X:{-.85f,.85f}) for (const float Y:{-.85f,.85f})
        {
            const FVector Corner=Ground.ImpactPoint+FVector(Extent.X*X,Extent.Y*Y,0);
            FHitResult Support;
            if (!GetWorld()->LineTraceSingleByChannel(Support,Corner+FVector(0,0,18),Corner-FVector(0,0,18),ECC_Visibility,GroundQuery) ||
                Support.ImpactNormal.Z<.9) bSupported=false;
        }
        if (!bSupported) continue;
        const FVector Center=Ground.ImpactPoint+FVector(0,0,Extent.Z+6);
        FHitResult OwnerViewBlock;
        if (GetWorld()->LineTraceSingleByChannel(OwnerViewBlock,Receiver.GetPawnViewLocation(),Center,ECC_Visibility,GroundQuery)) continue;
        if (GetWorld()->OverlapBlockingTestByProfile(Center,FQuat::Identity,TEXT("PhysicsActor"),Shape,Query) ||
            GetWorld()->SweepTestByProfile(Mesh->Bounds.Origin,Center,FQuat::Identity,TEXT("PhysicsActor"),Shape,Query)) continue;
        OutLocation=Center-PivotOffset;
        return true;
    }
    return false;
}

bool UDeliveryCompanionComponent::TryPlacePackageOnGround(FName& Failure)
{
    Failure=TEXT("PackagePlacementUnavailable");
    auto* Npc=GetCompanion();
    auto* Receiver=Cast<ADeliveryCharacter>(GetOwnerPawn());
    auto* Package=GetCarriedTaskPackage();
    auto* Item=IsValid(Package)?Package->GetDeliveryItemComponent():nullptr;
    if (!IsValid(Npc) || !Npc->HasAuthority() || !IsValid(Receiver) || Receiver->GetTaskPlayerState()!=OwnerPlayerState ||
        !CanCommand(OwnerPlayerState) || !HasActiveErrand() || Errand.Action!=EDeliveryNpcAction::DropPackage ||
        Errand.Phase!=EDeliveryCompanionErrandPhase::ReturnToOwner || Errand.CommandRevision!=CommandRevision ||
        !Item || Package!=Errand.Package || Item->OwningTask!=Errand.Task || Package->GetOwner()!=Npc ||
        !Npc->CanHandleTaskPackage() || !Npc->GetCharacterMovement()->IsMovingOnGround() ||
        Npc->GetVelocity().SizeSquared2D()>FMath::Square(20.f) ||
        FVector::DistSquared(Npc->GetActorLocation(),Receiver->GetTaskPackageTransferPoint())>FMath::Square(220.f)) return false;
    FVector Location;
    if (!FindPackageGroundLocation(*Package,*Receiver,Location)) { Failure=TEXT("NoSafePackageGround"); return false; }
    const auto* Clip=GetTuning().GroundPickupAnimation.Get();
    const auto* Mesh=Npc->GetMesh()->GetSkeletalMeshAsset();
    if (!IsValid(Clip) || !Mesh || Clip->GetSkeleton()!=Mesh->GetSkeleton() || Clip->HasRootMotion())
    { Failure=TEXT("PackagePlacementAnimationUnavailable"); return false; }
    const float Contact=GetTuning().GroundPickupContactTime;
    if (!FMath::IsFinite(Contact) || Contact<=0 || Contact>Clip->GetPlayLength()) return false;
    const double Now=GetWorld()->GetTimeSeconds();
    if (Errand.GroundPlacementServerTime<0)
    {
        Errand.GroundPlacementServerTime=Now;
        Npc->ForceNetUpdate();
    }
    if (Now-Errand.GroundPlacementServerTime<Contact) { Failure=TEXT("PlacingPackage"); return false; }

    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const TWeakObjectPtr<ADeliveryPackageItem> WeakPackage(Package);
    const TWeakObjectPtr<ADeliveryCharacter> WeakReceiver(Receiver);
    const TWeakObjectPtr<UDeliveryItemComponent> WeakItem(Item);
    const int32 Revision=CommandRevision;
    const auto IsCurrent=[&]()
    {
        const auto* Tasks=WeakThis.IsValid()?UDeliveryTaskManagerComponent::Get(this):nullptr;
        return WeakThis.IsValid() && WeakNpc.IsValid() && !Npc->IsActorBeingDestroyed() &&
            WeakPackage.IsValid() && !Package->IsActorBeingDestroyed() && WeakReceiver.IsValid() && WeakItem.IsValid() &&
            Receiver==GetOwnerPawn() && Receiver->GetTaskPlayerState()==OwnerPlayerState && CanCommand(OwnerPlayerState) &&
            CommandRevision==Revision && HasActiveErrand() && Errand.CommandRevision==Revision &&
            Errand.Action==EDeliveryNpcAction::DropPackage && Errand.Package==Package && Errand.Task==Item->OwningTask &&
            Tasks && Tasks->CanAcquireItem(Errand.Task) && Npc->CanHandleTaskPackage() &&
            !Npc->Agent->ShouldPauseForConversation() && (!Npc->Agent->HasActiveAction() || Npc->Agent->CanContinueCompanionMovement());
    };
    if (!IsCurrent() || !Item->BeginTransfer(Npc)) { Failure=TEXT("TransferBusy"); return false; }
    ON_SCOPE_EXIT { if (WeakItem.IsValid()) Item->EndTransfer(); };
    const FVector Original=Package->GetActorLocation();
    if (!ReleaseTaskPackage(Package)) { Failure=TEXT("SourceReleaseFailed"); return false; }
    // 释放后的回调若改令或改变现场，先恢复原实物，不能继续替旧安排放件
    if (!IsCurrent() || Package->GetOwner() || !FindPackageGroundLocation(*Package,*Receiver,Location))
    {
        if (WeakThis.IsValid() && WeakPackage.IsValid() && !Package->GetOwner())
        {
            if (!TryReceiveTaskPackage(Package) && WeakPackage.IsValid() && !Package->GetOwner())
                Package->DropFromInventory(Original,FVector::ZeroVector);
        }
        Failure=TEXT("PackagePlacementChanged");
        return false;
    }
    Package->DropFromInventory(Location,FVector::ZeroVector);
    if (!WeakPackage.IsValid() || Package->GetOwner() || Package->GetAttachParentActor() ||
        Package->IsHidden() || !Package->GetActorEnableCollision())
    { Failure=TEXT("PackagePlacementNotConfirmed"); return false; }
    Package->ForceNetUpdate();
    UE_LOG(LogDelivery,Log,TEXT("CompanionPackagePlacedOnGround npc=%s package=%s revision=%d location=%s"),
        *GetNameSafe(Npc),*GetNameSafe(Package),Revision,*Package->GetActorLocation().ToCompactString());
    Failure=NAME_None;
    return true;
}

bool UDeliveryCompanionComponent::TryRecoverGroundPackage(FName& Failure)
{
    Failure=TEXT("GroundPackageUnavailable");
    auto* Npc=GetCompanion();
    auto* Package=Errand.Package.Get();
    auto* Item=IsValid(Package)?Package->GetDeliveryItemComponent():nullptr;
    if (!IsValid(Npc) || !Npc->HasAuthority() || !HasActiveErrand() || Errand.CommandRevision!=CommandRevision ||
        !Errand.bRecoverGroundPackage || Errand.Phase!=EDeliveryCompanionErrandPhase::GoToPickup ||
        !CanCommand(OwnerPlayerState) || !Item || Item->OwningTask!=Errand.Task || Package!=GetOwnerDroppedPackage() ||
        !CanReceiveTaskPackage(Package) || Npc->Combat->IsReactingToHit() || Npc->Combat->IsStriking()) return false;

    Failure=TEXT("GroundPackageUnreachable");
    const auto* Movement=Npc->GetCharacterMovement();
    const auto* PackageMesh=Package->GetItemMesh();
    const FVector From=Npc->GetActorLocation(),Feet=Npc->GetNavAgentLocation();
    const FVector Goal=PackageMesh?PackageMesh->Bounds.Origin:Package->GetActorLocation();
    if (!Movement || !Movement->IsMovingOnGround() || !IsValid(PackageMesh) || !PackageMesh->IsRegistered() ||
        Npc->GetVelocity().SizeSquared2D()>FMath::Square(20.f) || Package->GetVelocity().SizeSquared()>FMath::Square(30.f) ||
        Goal.ContainsNaN() || FVector::DistSquared(From,Goal)>FMath::Square(GroundPackageReach)) return false;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionRecoverPackage),false,Npc);
    Query.AddIgnoredActor(Package);
    FCollisionObjectQueryParams Objects;
    for (const auto Channel:{ECC_WorldStatic,ECC_WorldDynamic,ECC_Pawn,ECC_Vehicle,ECC_PhysicsBody}) Objects.AddObjectTypesToQuery(Channel);
    FHitResult Reach;
    if (GetWorld()->LineTraceSingleByObjectType(Reach,From,Goal,Objects,Query)) return false;
    FHitResult Ground;
    const FVector Below(Goal.X,Goal.Y,Feet.Z-45.);
    if (!GetWorld()->LineTraceSingleByObjectType(Ground,Goal,Below,Objects,Query) || !Movement->IsWalkable(Ground) ||
        FMath::Abs(Ground.ImpactPoint.Z-Feet.Z)>45. || Goal.Z-Ground.ImpactPoint.Z>PackageMesh->Bounds.BoxExtent.Z+18.) return false;

    Failure=TEXT("PackagePickupAnimationUnavailable");
    const auto* Clip=GetTuning().GroundPickupAnimation.Get();
    const auto* Mesh=Npc->GetMesh()->GetSkeletalMeshAsset();
    const float Contact=GetTuning().GroundPickupContactTime;
    if (!IsValid(Clip) || !Mesh || Clip->GetSkeleton()!=Mesh->GetSkeleton() || Clip->HasRootMotion() ||
        !FMath::IsFinite(Contact) || Contact<=0 || Contact>=Clip->GetPlayLength()) return false;
    SetItemSearchFocus(Goal,0.f);
    const FVector Toward=(Goal-From).GetSafeNormal2D();
    if (!Toward.IsNearlyZero() && FVector::DotProduct(Toward,Npc->GetActorForwardVector())<.76605)
    { Failure=TEXT("PickingUpGroundPackage"); return false; }
    const double Now=GetHomeServerTime();
    if (Errand.GroundPickupServerTime<0)
    {
        Errand.GroundPickupServerTime=Now;
        Npc->ForceNetUpdate();
    }
    if (Now-Errand.GroundPickupServerTime<Contact) { Failure=TEXT("PickingUpGroundPackage"); return false; }

    Failure=TEXT("GroundPackageTransferBusy");
    const TWeakObjectPtr<ADeliveryPackageItem> PackageGuard(Package);
    const TWeakObjectPtr<UDeliveryItemComponent> ItemGuard(Item);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> NpcGuard(Npc);
    const int32 Revision=CommandRevision;
    if (!Item->BeginTransfer(Npc)) return false;
    ON_SCOPE_EXIT { if (ItemGuard.IsValid()) ItemGuard->EndTransfer(); };
    // 预约后复用接收入口，进行中的订单不会再次取件或重置计时
    Failure=TEXT("GroundPackageReceiveRejected");
    if (!TryReceiveTaskPackage(Package) || !PackageGuard.IsValid() || !NpcGuard.IsValid() || Package->GetOwner()!=Npc) return false;
    UE_LOG(LogDelivery,Log,TEXT("CompanionPackagePickedUpFromGround npc=%s package=%s revision=%d"),
        *GetNameSafe(Npc),*GetNameSafe(Package),Revision);
    Failure=NAME_None;
    return true;
}
