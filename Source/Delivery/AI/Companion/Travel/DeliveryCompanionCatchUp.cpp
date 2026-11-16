#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionTuning.h"
#include "Player/DeliveryPlayerController.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "Engine/World.h"

bool UDeliveryCompanionComponent::CanUseCatchUpDestination(APawn* Pawn,const FVector& Destination) const
{
    auto* Npc=GetCompanion();
    if (!Npc || !Pawn || Mode!=EDeliveryCompanionMode::Follow || Npc->IsKnockedDown() || GetCarriedItem() ||
        !Npc->GetCharacterMovement()->IsMovingOnGround() ||
        FVector::Dist2D(Npc->GetActorLocation(),Pawn->GetActorLocation())<=GetTuning().CatchUpDistance ||
        !IsDestinationClear(Destination) || !IsOutsideTaskEntrance(Destination)) return false;
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    auto* Path=Nav?Nav->FindPathToLocationSynchronously(GetWorld(),Npc->GetNavAgentLocation(),Destination,Npc):nullptr;
    if (!Path || !Path->IsValid() || Path->IsPartial()) return false;
    const auto* Capsule=Npc->GetCapsuleComponent();
    const float Half=Capsule->GetScaledCapsuleHalfHeight();
    FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionCatchUp),false,Npc);
    FHitResult Hit;
    // 完整导航路径之外还要求直线胶囊通道无遮挡，不能越过尚未更新导航的闭门
    return !GetWorld()->SweepSingleByChannel(Hit,Npc->GetActorLocation()+FVector(0,0,3),
        Destination+FVector(0,0,Half+3),FQuat::Identity,ECC_Pawn,
        FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius()+8,Half),Params);
}

bool UDeliveryCompanionComponent::TryBeginCatchUp(APawn* Pawn,const FVector& Destination)
{
    const double Now=GetWorld()->GetTimeSeconds();
    if (CatchUpRequestId.IsValid() || Now<NextCatchUpAt ||
        FVector::Dist2D(GetOwner()->GetActorLocation(),Pawn->GetActorLocation())<=GetTuning().CatchUpDistance) return false;
    NextCatchUpAt=Now+GetTuning().CatchUpRetrySeconds;
    if (!CanUseCatchUpDestination(Pawn,Destination)) return false;
    TArray<FCatchUpViewer> Viewers;
    for (auto It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        auto* PC=Cast<ADeliveryPlayerController>(It->Get());
        // 不支持的玩家或旁观视角也会阻止补位，不能只检查主人或房主
        if (!IsValid(PC) || !IsValid(PC->GetViewTarget())) return false;
        FCatchUpViewer Viewer;
        Viewer.Player=PC; Viewer.Pawn=PC->GetPawn(); Viewer.ViewTarget=PC->GetViewTarget();
        Viewer.ViewRevision=PC->GetCompanionViewRevision();
        Viewers.Add(Viewer);
    }
    if (Viewers.IsEmpty()) return false;
    const auto* Npc=GetCompanion();
    const FBox Bounds=Npc->GetComponentsBoundingBox(true);
    if (!Bounds.IsValid || Bounds.GetExtent().ContainsNaN() || Bounds.GetExtent().GetMax()>1000) return false;
    StopFollowMovement();
    Npc->GetCharacterMovement()->StopMovementImmediately();
    CatchUpViewers=MoveTemp(Viewers);
    CatchUpStart=Npc->GetActorLocation(); CatchUpDestination=Destination;
    CatchUpOwnerPawn=Pawn; CatchUpOwnerPosition=Pawn->GetActorLocation();
    CatchUpStartCenter=Bounds.GetCenter();
    const FVector End=Destination+FVector(0,0,Npc->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()+3);
    CatchUpEndCenter=CatchUpStartCenter+End-CatchUpStart;
    CatchUpExtent=Bounds.GetExtent()+FVector(GetTuning().CatchUpVisibilityMargin);
    CatchUpCommandRevision=CommandRevision; CatchUpRound=0; bCatchUpDenied=false;
    NextCatchUpRoundAt=0;
    SetSuspension(TEXT("CheckingCatchUpVisibility"));
    SendCatchUpQueries();
    return true;
}

void UDeliveryCompanionComponent::SendCatchUpQueries()
{
    CatchUpRequestId=FGuid::NewGuid();
    CatchUpDeadline=GetWorld()->GetTimeSeconds()+.6;
    // 先建立全体待回包状态，监听服务器本机RPC可能同步返回
    for (auto& Viewer:CatchUpViewers) Viewer.bReplied=false;
    for (const auto& Viewer:CatchUpViewers)
        if (Viewer.Player.IsValid()) Viewer.Player->ClientCheckCompanionCatchUp(GetCompanion(),CatchUpRequestId,
            CatchUpStartCenter,CatchUpEndCenter,CatchUpExtent);
}

bool UDeliveryCompanionComponent::AreCatchUpViewersCurrent() const
{
    int32 Count=0;
    for (auto It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        const auto* PC=It->Get();
        const auto* Viewer=CatchUpViewers.FindByPredicate([PC](const FCatchUpViewer& Item){return Item.Player.Get()==PC;});
        if (!Viewer || !Viewer->Player.IsValid() || Viewer->Pawn.Get()!=PC->GetPawn() ||
            Viewer->ViewTarget.Get()!=PC->GetViewTarget() ||
            Viewer->ViewRevision!=Viewer->Player->GetCompanionViewRevision()) return false;
        ++Count;
    }
    return Count>0 && Count==CatchUpViewers.Num();
}

void UDeliveryCompanionComponent::AcceptCatchUpVisibility(APlayerController* Player,FGuid RequestId,
    APawn* ViewPawn,AActor* ViewTarget,bool bUnseen)
{
    if (!GetOwner()->HasAuthority() || !RequestId.IsValid() || RequestId!=CatchUpRequestId) return;
    auto* Viewer=CatchUpViewers.FindByPredicate([Player](const FCatchUpViewer& Item){return Item.Player.Get()==Player;});
    if (!Viewer || Viewer->bReplied) return;
    Viewer->bReplied=true;
    if (!bUnseen || Viewer->Pawn.Get()!=ViewPawn || Viewer->ViewTarget.Get()!=ViewTarget ||
        GetWorld()->GetTimeSeconds()>CatchUpDeadline || !AreCatchUpViewersCurrent()) bCatchUpDenied=true;
}

bool UDeliveryCompanionComponent::UpdateCatchUp(APawn* Pawn)
{
    if (!CatchUpRequestId.IsValid()) return false;
    const double Now=GetWorld()->GetTimeSeconds();
    const bool bAwaitingReplies=CatchUpViewers.ContainsByPredicate([](const FCatchUpViewer& Viewer){return !Viewer.bReplied;});
    if (bCatchUpDenied || (bAwaitingReplies && Now>CatchUpDeadline) || Mode!=EDeliveryCompanionMode::Follow ||
        CommandRevision!=CatchUpCommandRevision || Pawn!=CatchUpOwnerPawn.Get() || !AreCatchUpViewersCurrent() ||
        FVector::DistSquared(GetOwner()->GetActorLocation(),CatchUpStart)>FMath::Square(25.f) ||
        FVector::DistSquared(Pawn->GetActorLocation(),CatchUpOwnerPosition)>FMath::Square(GetTuning().RepathDistance))
    {
        CancelCatchUp(); NextNavigationAt=0; bHasFollowDestination=false;
        return false;
    }
    if (bAwaitingReplies) return true;
    if (CatchUpRound==0)
    {
        // 从全员第一轮回答完成后再等，不能让慢回包把两次本机检查挤成同一帧
        if (NextCatchUpRoundAt==0) NextCatchUpRoundAt=Now+.2;
        if (Now<NextCatchUpRoundAt) return true;
        CatchUpRound=1; SendCatchUpQueries();
        return true;
    }
    auto* Npc=GetCompanion();
    const FVector Destination=CatchUpDestination;
    const bool bAllowed=CanUseCatchUpDestination(Pawn,Destination);
    CancelCatchUp(); NextNavigationAt=0; bHasFollowDestination=false;
    if (!bAllowed) return false;
    // 落点已经完成净空和路径复查，不使用会调整位置的试探TeleportTo
    const FVector End=Destination+FVector(0,0,Npc->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()+3);
    if (!Npc->TeleportTo(End,Npc->GetActorRotation(),false,true)) return false;
    Npc->GetCharacterMovement()->StopMovementImmediately();
    Npc->ForceNetUpdate();
    return true;
}

void UDeliveryCompanionComponent::CancelCatchUp()
{
    CatchUpRequestId.Invalidate(); CatchUpViewers.Reset(); CatchUpOwnerPawn.Reset(); bCatchUpDenied=false;
    if (SuspensionReason==TEXT("CheckingCatchUpVisibility")) SetSuspension(NAME_None);
}
