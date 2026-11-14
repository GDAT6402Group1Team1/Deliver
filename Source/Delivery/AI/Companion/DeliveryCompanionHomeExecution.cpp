#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AbilitySystem/Abilities/GA_UseHomeObject.h"
#include "AbilitySystemComponent.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Animation/AnimSequence.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "AIController.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "Navigation/PathFollowingComponent.h"
#include "Engine/World.h"

namespace
{
    bool IsHomeRunning(EDeliveryCompanionHomePhase Phase)
    {
        return Phase!=EDeliveryCompanionHomePhase::None && Phase!=EDeliveryCompanionHomePhase::Completed &&
            Phase!=EDeliveryCompanionHomePhase::Failed && Phase!=EDeliveryCompanionHomePhase::Cancelled;
    }
    bool IsUprightHomePlacement(const FTransform& Transform)
    {
        return !Transform.ContainsNaN() && Transform.GetRotation().IsNormalized() &&
            FVector::DotProduct(Transform.GetRotation().GetUpVector(),FVector::UpVector)>.9998;
    }
    FString HomeObjectName(const UDeliveryHomeObjectComponent* Object)
    {
        return IsValid(Object) && !Object->DisplayName.IsEmpty()?Object->DisplayName.ToString().Left(64):TEXT("这件家居物品");
    }
    FString HomeStopDescription(FName Reason)
    {
        if (Reason==TEXT("OwnerChangedArrangement") || Reason==TEXT("ReplacedByHomeArrangement")) return TEXT("主人已经换了安排");
        if (Reason==TEXT("AssistCombat")) return TEXT("我要先放下手里的东西、离开座位去帮主人");
        if (Reason==TEXT("HomeUserDown")) return TEXT("我被打倒了");
        if (Reason==TEXT("HomeOwnerLost") || Reason==TEXT("OwnerLost")) return TEXT("和主人的当前联系已经中断");
        if (Reason==TEXT("HomeArrangementTimedOut")) return TEXT("接近或使用等待太久，仍没有完成");
        if (Reason==TEXT("HomePropCarryLost")) return TEXT("这件物品的实际搬运已经中断");
        if (Reason==TEXT("HomePlacementNotSettled")) return TEXT("放下的物品没有稳定在指定位置");
        if (Reason==TEXT("HomePropScaleRestoreBlocked")) return TEXT("放下的位置没有足够空间恢复物品原来的大小，物品留在现场等待安全恢复");
        if (Reason==TEXT("HomePropScaleRejected")) return TEXT("这件物品未能安全缩小，搬运没有开始");
        if (Reason==TEXT("HomeReleasedPropTakenOrLost")) return TEXT("放下的物品已经被拿走或不在原处");
        if (Reason==TEXT("HomePropTransferChanged")) return TEXT("家具或搬放中转点已经变化，我停止这次搬放，物品留在现场");
        return TEXT("物件、位置或使用条件发生变化，游戏没有确认完成");
    }
}

double UDeliveryCompanionComponent::GetHomeServerTime() const
{
    const auto* Clock=GetWorld()?GetWorld()->GetGameState():nullptr;
    return Clock?Clock->GetServerWorldTimeSeconds():GetWorld()?GetWorld()->GetTimeSeconds():0;
}

void UDeliveryCompanionComponent::InitializeHomeAbility()
{
    auto* Npc=GetCompanion();
    auto* ASC=Npc?Npc->GetAbilitySystemComponent():nullptr;
    if (!Npc || !Npc->HasAuthority() || !ASC || bHomeEndingPlay || HomeAbilityHandle.IsValid()) return;
    UClass* Class=HomeUseAbilityClass?HomeUseAbilityClass.Get():UGA_UseHomeObject::StaticClass();
    HomeAbilitySystem=ASC;
    HomeAbilityHandle=ASC->GiveAbility(FGameplayAbilitySpec(Class,1,INDEX_NONE,this));
}

bool UDeliveryCompanionComponent::HasActiveHomeArrangement() const
{
    return HomeState.ArrangementId.IsValid() && IsHomeRunning(HomeState.Phase);
}

bool UDeliveryCompanionComponent::IsCurrentHome(FGuid ArrangementId) const
{
    return IsValid(this) && !bHomeEndingPlay && IsValid(GetOwner()) && !GetOwner()->IsActorBeingDestroyed() &&
        HasActiveHomeArrangement() && HomeState.ArrangementId==ArrangementId;
}

bool UDeliveryCompanionComponent::NeedsHomeExit() const
{
    return HasActiveHomeArrangement() && HomeState.Operation==EDeliveryHomeOperation::Sit && bHomeUseStarted &&
        (HomeState.Phase==EDeliveryCompanionHomePhase::Using || HomeState.Phase==EDeliveryCompanionHomePhase::LeaveUse ||
            (HomeState.Phase==EDeliveryCompanionHomePhase::EnterUse && HomeState.PoseAnimation));
}

bool UDeliveryCompanionComponent::OwnsHomeSource(const UDeliveryHomeObjectComponent* Source) const
{
    FName Reason;
    return IsValid(Source) && HomeTakeClaim.Object.Get()==Source && HomePropCarryId.IsValid() &&
        HomeTakeClaim.ActionId==HomePropCarryId && IsValid(HomeState.CarriedProp) &&
        Source->GetOwner()==HomeState.CarriedProp && HomeGrabbable.IsValid() &&
        HomeGrabbable->IsCarriedByProvider(this,HomePropCarryId) && Source->ValidateClaim(HomeTakeClaim,false,Reason);
}

void UDeliveryCompanionComponent::SetHomePose(UAnimSequence* Animation,bool bLoop,bool bUpperBody)
{
    HomeState.PoseAnimation=Animation;
    HomeState.bLoopPose=bLoop && Animation;
    HomeState.bUpperBodyPose=bUpperBody && Animation;
    HomeState.PoseStartedServerTime=GetHomeServerTime();
    GetOwner()->ForceNetUpdate();
}

void UDeliveryCompanionComponent::RefreshHomeCarryPose()
{
    if (IsValid(HomeState.CarriedProp)) SetHomePose(PropCarryAnimation,true,true);
    else SetHomePose(nullptr,false,false);
}

void UDeliveryCompanionComponent::SetHomePhase(EDeliveryCompanionHomePhase Phase)
{
    HomeState.Phase=Phase;
    HomeState.Reason=NAME_None;
    HomePhaseStartedAt=GetWorld()->GetTimeSeconds();
    LastHomeUpdateAt=0;
    LastHomePause=NAME_None;
    NextHomeNavigationAt=0;
    GetOwner()->ForceNetUpdate();
}

void UDeliveryCompanionComponent::StopHomeMovement()
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority()) return;
    const FAIRequestID OldId=HomeMoveId;
    HomeMoveId=FAIRequestID::InvalidRequest;
    const bool bOwned=bHomeOwnsNavigation;
    bHomeOwnsNavigation=false;
    LastHomeUpdateAt=0;
    auto* AI=Cast<AAIController>(Npc->GetController());
    if (AI && bHomeOwnsFocus && AI->GetFocalPointForPriority(EAIFocusPriority::Gameplay).Equals(HomeFocusPoint,1.f))
        AI->ClearFocus(EAIFocusPriority::Gameplay);
    bHomeOwnsFocus=false;
    if (auto* Path=AI?AI->GetPathFollowingComponent():nullptr; bOwned && Path && OldId.IsValid() && Path->GetCurrentRequestId()==OldId)
        Path->AbortMove(*this,FPathFollowingResultFlags::UserAbort,OldId);
}

bool UDeliveryCompanionComponent::MoveForHome(const FVector& Goal,float Radius)
{
    auto* Npc=GetCompanion();
    auto* AI=Npc?Cast<AAIController>(Npc->GetController()):nullptr;
    auto* Path=AI?AI->GetPathFollowingComponent():nullptr;
    if (!Npc || !AI || !Path || Goal.ContainsNaN()) return false;
    const FGuid Arrangement=HomeState.ArrangementId;
    const auto Phase=HomeState.Phase;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<AAIController> WeakAI(AI);
    const auto StillCurrent=[&]() { return WeakThis.IsValid() && IsCurrentHome(Arrangement) && HomeState.Phase==Phase; };
    if (IsHomePropLagging()) { StopHomeMovement(); SetSuspension(TEXT("HomePropBlocked")); return false; }
    const FVector Feet=Npc->GetNavAgentLocation();
    FAIMoveRequest Move(Goal);
    Move.SetAcceptanceRadius(Radius);
    Move.SetAllowPartialPath(false);
    Move.SetProjectGoalLocation(false);
    Move.SetReachTestIncludesAgentRadius(false);
    Move.SetReachTestIncludesGoalRadius(false);
    if (FVector::Dist2D(Feet,Goal)<=Radius && FMath::Abs(Feet.Z-Goal.Z)<=15.f && Path->HasReached(Move))
    { StopHomeMovement(); if (!StillCurrent()) return false; SetSuspension(NAME_None); return true; }
    if (bHomeOwnsNavigation && Path->GetCurrentRequestId()==HomeMoveId &&
        AI->GetMoveStatus()!=EPathFollowingStatus::Idle && HomeMoveGoal.Equals(Goal,3.f)) return false;
    const double Now=GetWorld()->GetTimeSeconds();
    if (Now<NextHomeNavigationAt) return false;
    StopHomeMovement();
    if (!StillCurrent()) return false;
    NextHomeNavigationAt=Now+GetTuning().NavigationInterval;
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    FNavLocation Projected;
    const auto* NavPath=Nav?Nav->FindPathToLocationSynchronously(GetWorld(),Feet,Goal,Npc):nullptr;
    if (!Nav || !Nav->ProjectPointToNavigation(Goal,Projected,FVector(35,35,100),&Npc->GetNavAgentPropertiesRef()) ||
        FVector::Dist2D(Projected.Location,Goal)>FMath::Max(Radius,8.f) || FMath::Abs(Projected.Location.Z-Goal.Z)>15.f ||
        !NavPath || !NavPath->IsValid() || NavPath->IsPartial())
    {
        SetSuspension(TEXT("HomePathBlocked"));
        NextHomeNavigationAt=Now+GetTuning().BlockedRetrySeconds;
        return false;
    }
    const auto Result=AI->MoveTo(Move);
    if (!StillCurrent())
    {
        if (WeakThis.IsValid() && WeakAI.IsValid() && Result.MoveId.IsValid())
            if (auto* CurrentPath=AI->GetPathFollowingComponent(); CurrentPath && CurrentPath->GetCurrentRequestId()==Result.MoveId)
                CurrentPath->AbortMove(*this,FPathFollowingResultFlags::UserAbort,Result.MoveId);
        return false;
    }
    bHomeOwnsNavigation=Result.Code==EPathFollowingRequestResult::RequestSuccessful;
    HomeMoveId=bHomeOwnsNavigation?Result.MoveId:FAIRequestID::InvalidRequest;
    HomeMoveGoal=Goal;
    if (Result.Code==EPathFollowingRequestResult::Failed)
    { SetSuspension(TEXT("HomePathBlocked")); NextHomeNavigationAt=Now+GetTuning().BlockedRetrySeconds; }
    return false;
}

bool UDeliveryCompanionComponent::AlignForHome(const FTransform& Use)
{
    auto* Npc=GetCompanion();
    auto* AI=Npc?Cast<AAIController>(Npc->GetController()):nullptr;
    if (!Npc || !AI || Use.ContainsNaN()) return false;
    if (HomeSeatRuntime.ArrangementId.IsValid() && HomeSeatRuntime.ArrangementId==HomeState.ArrangementId &&
        HomeState.SeatMotion.bActive && !HomeState.SeatMotion.bMoving && !Npc->IsKnockedDown() &&
        Npc->GetCharacterMovement()->MovementMode==MOVE_Flying)
    {
        // 到座位或准备离开时只借用原 CMC 渐转，退出仍由座位资源记录恢复旧开关
        Npc->GetCharacterMovement()->bOrientRotationToMovement=false;
        Npc->GetCharacterMovement()->bUseControllerDesiredRotation=true;
    }
    HomeFocusPoint=Npc->GetActorLocation()+Use.GetRotation().GetForwardVector().GetSafeNormal2D()*300.f;
    AI->SetFocalPoint(HomeFocusPoint,EAIFocusPriority::Gameplay);
    bHomeOwnsFocus=true;
    // 朝向由原 CharacterMovement 转动，不直接改人物变换
    return FMath::Abs(FMath::FindDeltaAngleDegrees(Npc->GetActorRotation().Yaw,Use.Rotator().Yaw))<=HomeUseYawTolerance &&
        Npc->GetVelocity().Size2D()<5.f;
}

void UDeliveryCompanionComponent::ReleaseHomeClaim(FDeliveryHomeUseClaim& Claim,FName Reason)
{
    const FDeliveryHomeUseClaim Old=Claim;
    Claim={};
    auto* Object=Old.Object.Get();
    if (!Object) return;
    if (HomeUseClaim.Object.Get()!=Object && HomeTakeClaim.Object.Get()!=Object) Object->OnUseEnded.RemoveAll(this);
    const int32 Before=Object->GetRevision();
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<UDeliveryHomeObjectComponent> WeakObject(Object);
    Object->ReleaseUse(Old,Reason);
    if (!WeakThis.IsValid() || !WeakObject.IsValid() || !PendingHomeArrangementId.IsValid() || Object->GetRevision()!=Before+1) return;
    // 只同步这次亲自释放带来的一个版本；额外变化保留旧快照，随后重新核验会拒绝
    auto& Pending=PendingHomeChoice;
    if (Pending.SourceObject.Get()==Object && Pending.SourceRevision==Before) Pending.SourceRevision=Before+1;
    if (Pending.DestinationObject.Get()==Object && Pending.DestinationRevision==Before) Pending.DestinationRevision=Before+1;
    if (Pending.Use.Object.Get()==Object && Pending.Use.Revision==Before) Pending.Use.Revision=Before+1;
    if (Pending.TakeUse.Object.Get()==Object && Pending.TakeUse.Revision==Before) Pending.TakeUse.Revision=Before+1;
}

bool UDeliveryCompanionComponent::BeginHomeChoice(const FDeliveryCompanionHomeChoice& Choice,FGuid ArrangementId,FName& OutReason)
{
    auto* Npc=GetCompanion();
    auto* Source=Choice.SourceObject.Get();
    const bool bTake=Choice.Operation==EDeliveryHomeOperation::TakeProp;
    const bool bPlace=Choice.Operation==EDeliveryHomeOperation::PlaceProp;
    const bool bAlreadyCarried=bPlace && OwnsHomeSource(Source);
    FDeliveryHomeUseClaim NewUse,NewTake;
    const FGuid NewCarryId=bAlreadyCarried?HomePropCarryId:FGuid::NewGuid();
    auto* UseObject=bPlace?Choice.DestinationObject.Get():Source;
    if (!Npc || !Source || !UseObject || !ArrangementId.IsValid() || !HomeAbilityHandle.IsValid())
    { OutReason=TEXT("HomeExecutorUnavailable"); return false; }
    if (bPlace && !IsUprightHomePlacement(Choice.Use.UseTransform))
    { OutReason=TEXT("HomePlacementTiltUnsupported"); return false; }
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const TWeakObjectPtr<APlayerState> Requester(OwnerPlayerState.Get());
    const int32 Revision=CommandRevision;
    const auto StillCurrent=[&]()
    { return WeakThis.IsValid() && WeakNpc.IsValid() && !Npc->IsActorBeingDestroyed() && !bHomeEndingPlay &&
        CommandRevision==Revision && OwnerPlayerState==Requester.Get() && CanCommand(Requester.Get()); };
    const auto ReleaseNewClaims=[&]()
    {
        if (NewUse.Object.IsValid()) NewUse.Object->ReleaseUse(NewUse,TEXT("HomeStartInvalidated"));
        if (NewTake.Object.IsValid()) NewTake.Object->ReleaseUse(NewTake,TEXT("HomeStartInvalidated"));
    };
    if (!bTake && !UseObject->TryClaimUse(Npc,OwnerPlayerState,ArrangementId,Choice.Use,NewUse,OutReason)) return false;
    if (!StillCurrent()) { ReleaseNewClaims(); OutReason=TEXT("HomeStartInvalidated"); return false; }
    if ((bTake || bPlace) && !bAlreadyCarried &&
        !Source->TryClaimUse(Npc,OwnerPlayerState,NewCarryId,bTake?Choice.Use:Choice.TakeUse,NewTake,OutReason))
    { if (NewUse.Object.IsValid()) NewUse.Object->ReleaseUse(NewUse,OutReason); return false; }
    if (!StillCurrent()) { ReleaseNewClaims(); OutReason=TEXT("HomeStartInvalidated"); return false; }
    HomeUseClaim=NewUse;
    if (!bAlreadyCarried) { HomeTakeClaim=NewTake; if (NewTake.IsValid()) HomePropCarryId=NewCarryId; }
    if (HomeUseClaim.Object.IsValid()) HomeUseClaim.Object->OnUseEnded.AddUObject(this,&ThisClass::OnHomeObjectUseEnded);
    if (NewTake.Object.IsValid()) NewTake.Object->OnUseEnded.AddUObject(this,&ThisClass::OnHomeObjectUseEnded);
    AActor* Held=HomeState.CarriedProp;
    HomeState={}; HomeState.CarriedProp=Held;
    HomeState.ArrangementId=ArrangementId;
    HomeState.Operation=Choice.Operation;
    HomeState.Object=Source->GetOwner();
    HomeState.Destination=bPlace?UseObject->GetOwner():nullptr;
    ActiveHomeChoice=Choice;
    HomeRequester=OwnerPlayerState.Get();
    HomeActiveSeconds=0;
    bHomeUseStarted=false;
    bHomeExitAnimationFinished=false;
    bPlacingHomeProp=false;
    ClearHomePropTransfer();
    ReleasedPlacementProp.Reset(); PlacementReleasedAt=0; PlacementStableSince=0;
    SetHomePhase(bAlreadyCarried?EDeliveryCompanionHomePhase::GoToDestination:EDeliveryCompanionHomePhase::GoToObject);
    RefreshHomeCarryPose();
    return true;
}

bool UDeliveryCompanionComponent::ApplyHomeChoice(const FDeliveryCompanionHomeChoice& Choice,APlayerState* Requester,int32 ExpectedRevision)
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority() || bHomeEndingPlay || bChangingHomeArrangement ||
        !CanApplyHomeChoice(Choice,Requester,ExpectedRevision)) return false;
    TGuardValue<bool> Changing(bChangingHomeArrangement,true);
    PendingHomeChoice={}; PendingHomeArrangementId.Invalidate();
    const FGuid NewId=FGuid::NewGuid();
    if (NeedsHomeExit())
    {
        PendingHomeChoice=Choice; PendingHomeArrangementId=NewId;
        PendingHomeCommandRevision=++CommandRevision;
        BeginLeavingHomeUse(TEXT("ReplacedByHomeArrangement"));
        CancelAssist(TEXT("OwnerChangedArrangement"));
        if (HasActiveErrand()) FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("OwnerChangedArrangement"));
        Npc->ForceNetUpdate();
        return true;
    }
    // 即时替换也记录待执行快照，供亲自释放旧 claim 后更新唯一的自有版本变化
    PendingHomeChoice=Choice; PendingHomeArrangementId=NewId;
    if (HasActiveHomeArrangement()) FinishHomeArrangement(EDeliveryCompanionHomePhase::Cancelled,TEXT("ReplacedByHomeArrangement"),
        Choice.Operation==EDeliveryHomeOperation::PlaceProp && Choice.SourceObject.IsValid() && OwnsHomeSource(Choice.SourceObject.Get()));
    if (HomeState.CarriedProp && !(Choice.Operation==EDeliveryHomeOperation::PlaceProp && OwnsHomeSource(Choice.SourceObject.Get())))
        ReleaseHomeProp(TEXT("OwnerChangedArrangement"));
    const auto RefreshedChoice=PendingHomeChoice;
    PendingHomeChoice={}; PendingHomeArrangementId.Invalidate();
    FName Reason=TEXT("HomeCandidateChanged");
    if (!CanApplyHomeChoice(RefreshedChoice,Requester,ExpectedRevision) || !BeginHomeChoice(RefreshedChoice,NewId,Reason))
    {
        Npc->ObserveFact(TEXT("HomeArrangementRejected"),Choice.SourceObject.IsValid()?Choice.SourceObject->GetOwner():nullptr,
            FString::Printf(TEXT("对%s的家居安排没有开始，当前物件或可用位置已经变化；没有移动物件或完成用途。"),
                *HomeObjectName(Choice.SourceObject.Get())),EDeliveryNpcEvidenceSource::ActionResult);
        return false;
    }
    ++CommandRevision;
    CancelAssist(TEXT("OwnerChangedArrangement"));
    if (HasActiveErrand()) FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("OwnerChangedArrangement"));
    StopFollowMovement();
    bCaterpillarOffered=false; CaterpillarOpportunityId.Invalidate(); bAnnouncedCaterpillarOpportunity=true;
    Npc->ForceNetUpdate();
    return true;
}

void UDeliveryCompanionComponent::StartPendingHomeChoice()
{
    if (!PendingHomeArrangementId.IsValid() || HasActiveHomeArrangement() || bChangingHomeArrangement || bHomeEndingPlay) return;
    const auto Choice=PendingHomeChoice;
    const FGuid Id=PendingHomeArrangementId;
    const int32 Revision=PendingHomeCommandRevision;
    PendingHomeChoice={}; PendingHomeArrangementId.Invalidate();
    FName Reason=TEXT("HomePendingChoiceExpired");
    TGuardValue<bool> Changing(bChangingHomeArrangement,true);
    if (Revision==CommandRevision && CanApplyHomeChoice(Choice,OwnerPlayerState,Revision) && BeginHomeChoice(Choice,Id,Reason)) return;
    HomeState.ArrangementId=Id; HomeState.Phase=EDeliveryCompanionHomePhase::Failed;
    HomeState.Operation=Choice.Operation;
    HomeState.Object=Choice.SourceObject.IsValid()?Choice.SourceObject->GetOwner():nullptr;
    HomeState.Destination=Choice.DestinationObject.IsValid()?Choice.DestinationObject->GetOwner():nullptr;
    HomeState.Reason=Reason;
    GetCompanion()->ObserveFact(TEXT("HomeArrangementFailed"),HomeState.Object,
        FString::Printf(TEXT("我已离开旧位置，但对%s的新家居安排无法开始，当前物件或位置已经变化；没有完成这项用途。"),
            *HomeObjectName(Choice.SourceObject.Get())),EDeliveryNpcEvidenceSource::ActionResult);
    GetOwner()->ForceNetUpdate();
}

bool UDeliveryCompanionComponent::StartHomeUse(const UDeliveryHomeUseRequest& Request)
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority() || Request.GetOuter()!=Npc || !IsCurrentHome(Request.ArrangementId) ||
        bHomeUseStarted || !CanCommand(HomeRequester.Get()) || Npc->IsKnockedDown()) return false;
    const bool bTaking=HomeState.Phase==EDeliveryCompanionHomePhase::GoToObject &&
        (HomeState.Operation==EDeliveryHomeOperation::TakeProp || HomeState.Operation==EDeliveryHomeOperation::PlaceProp);
    const auto& Claim=bTaking?HomeTakeClaim:HomeUseClaim;
    auto* Object=Claim.Object.Get();
    FName Reason;
    FTransform Entrance,Use,Exit;
    if (!Object || !Object->GetUseTransforms(Claim,Entrance,Use,Exit) ||
        FVector::Dist2D(Npc->GetNavAgentLocation(),Entrance.GetLocation())>FMath::Max(HomeUseReachTolerance,12.f) ||
        FMath::Abs(Npc->GetNavAgentLocation().Z-Entrance.GetLocation().Z)>15.f ||
        ((bTaking || HomeState.Operation==EDeliveryHomeOperation::PlaceProp || HomeState.Operation==EDeliveryHomeOperation::Sit) &&
            (FMath::Abs(FMath::FindDeltaAngleDegrees(Npc->GetActorRotation().Yaw,Entrance.Rotator().Yaw))>HomeUseYawTolerance ||
                Npc->GetVelocity().Size2D()>=5.f)) ||
        (!bTaking && HomeState.Operation==EDeliveryHomeOperation::PlaceProp && !IsHomePropAtHand()) ||
        !Object->BeginUse(Claim,Reason)) return false;
    if (!IsCurrentHome(Request.ArrangementId)) return false;
    bHomeUseStarted=true;
    if (bTaking)
    {
        ClearHomePropTransfer();
        // 此时实物还未登记搬运，唯一有效摆放记录仍能提供退出用的中转点
        if (!Object->CapturePropTransferPoint(Claim,HomePropTransferPoint,Reason))
        { FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,Reason); return false; }
        bMovingToPropTransferPoint=HomePropTransferPoint.bEnabled;
        const bool bStarted=BeginHomePropCarry(Reason);
        if (!IsCurrentHome(Request.ArrangementId)) return false;
        if (!bStarted) { FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,Reason); return false; }
        SetHomePhase(EDeliveryCompanionHomePhase::TakeProp);
        RefreshHomeCarryPose();
    }
    else if (HomeState.Operation==EDeliveryHomeOperation::Sit)
    {
        const auto* Definition=Object->GetUseDefinition(Claim);
        SetHomePhase(EDeliveryCompanionHomePhase::EnterUse);
        UAnimSequence* Clip=Definition?(Definition->SeatMountAnimation?Definition->SeatMountAnimation.Get():Definition->EnterAnimation.Get()):nullptr;
        if (!BeginHomeSeatMove(Use,Clip,Reason))
        {
            if (IsCurrentHome(Request.ArrangementId)) FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,Reason);
            return false;
        }
        if (!Definition->SeatMountAnimation) SetHomePose(nullptr,false,false);
    }
    else if (HomeState.Operation==EDeliveryHomeOperation::PlaceProp)
    {
        auto* Source=ActiveHomeChoice.SourceObject.Get();
        if (!OwnsHomeSource(Source))
        { FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,TEXT("HomeCarriedPropChanged")); return false; }
        FTransform Placement;
        if (!Object->GetPlacementTransform(Claim,Source,Placement,Reason) || !IsUprightHomePlacement(Placement))
        { FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,Reason.IsNone()?FName(TEXT("HomePlacementChanged")):Reason); return false; }
        ClearHomePropTransfer();
        if (!Object->CapturePropTransferPoint(Claim,HomePropTransferPoint,Reason))
        { FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,Reason); return false; }
        // Place 的 Use 是支撑面，真实根位置由这个物件的原尺寸推导
        bPlacingHomeProp=true; PropPlacementTarget=Placement;
        BeginHomePropSegment(HomePropTransferPoint.bEnabled);
        SetHomePhase(EDeliveryCompanionHomePhase::PlaceProp);
    }
    else
    {
        SetHomePhase(EDeliveryCompanionHomePhase::Using);
        const auto* Definition=Object->GetUseDefinition(Claim);
        SetHomePose(Definition?Definition->Animation.Get():nullptr,false,false);
    }
    return true;
}

void UDeliveryCompanionComponent::CancelHomeUse(FGuid ArrangementId,FName Reason)
{
    if (IsCurrentHome(ArrangementId)) CancelHomeArrangement(Reason,false);
}

void UDeliveryCompanionComponent::BeginLeavingHomeUse(FName Reason)
{
    if (!HasActiveHomeArrangement() || HomeState.Phase==EDeliveryCompanionHomePhase::LeaveUse) return;
    StopHomeMovement();
    HomeLeaveReason=Reason;
    bHomeExitAnimationFinished=false;
    SetHomePhase(EDeliveryCompanionHomePhase::LeaveUse);
    const auto* Object=HomeUseClaim.Object.Get();
    const auto* Definition=Object?Object->GetUseDefinition(HomeUseClaim):nullptr;
    // 仍在攀上时先完成本次受扫掠约束的移动，再站起离开，不能在半空瞬移到出口
    if (!HomeState.SeatMotion.bMoving) SetHomePose(Definition?Definition->ExitAnimation.Get():nullptr,false,false);
}

void UDeliveryCompanionComponent::CancelHomeArrangement(FName Reason,bool bForce)
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority()) return;
    PendingHomeChoice={}; PendingHomeArrangementId.Invalidate();
    if (!bForce && NeedsHomeExit())
    { BeginLeavingHomeUse(Reason); return; }
    if (HasActiveHomeArrangement()) FinishHomeArrangement(EDeliveryCompanionHomePhase::Cancelled,Reason);
    else ReleaseHomeProp(Reason);
}

void UDeliveryCompanionComponent::FinishHomeArrangement(EDeliveryCompanionHomePhase Phase,FName Reason,bool bKeepProp)
{
    const FGuid OldId=HomeState.ArrangementId;
    const bool bWasRunning=HasActiveHomeArrangement();
    const bool bSucceeded=Phase==EDeliveryCompanionHomePhase::Completed;
    const FString SourceName=HomeObjectName(ActiveHomeChoice.SourceObject.Get());
    const FString DestinationName=HomeObjectName(ActiveHomeChoice.DestinationObject.Get());
    const auto& NamingClaim=HomeUseClaim.IsValid()?HomeUseClaim:HomeTakeClaim;
    const auto* NamingObject=NamingClaim.Object.Get();
    const auto* Definition=NamingObject?NamingObject->GetUseDefinition(NamingClaim):nullptr;
    const FString UseName=Definition?Definition->DisplayName.ToString().Left(64):TEXT("指定位置");
    FString Description;
    if (bSucceeded && HomeState.Operation==EDeliveryHomeOperation::TakeProp)
        Description=FString::Printf(TEXT("我已经实际拿起%s，现在仍拿着它，可以继续跟随或等候；还没有把它放到别处。"),*SourceName);
    else if (bSucceeded && HomeState.Operation==EDeliveryHomeOperation::PlaceProp)
        Description=FString::Printf(TEXT("我已经把%s放在%s的%s，松手后实物稳定，游戏确认摆放完成。"),*SourceName,*DestinationName,*UseName);
    else if (bSucceeded && HomeState.Operation==EDeliveryHomeOperation::PlayAudio)
        Description=FString::Printf(TEXT("我已经走近%s，实际打开了它的%s，游戏确认声音开关已经打开。"),*SourceName,*UseName);
    else if (bSucceeded && HomeState.Operation==EDeliveryHomeOperation::StopAudio)
        Description=FString::Printf(TEXT("我已经走近%s，实际关闭了它的声音，游戏确认开关已经关闭。"),*SourceName);
    else
        Description=FString::Printf(TEXT("我对%s的家居安排已经停止：%s。没有完成的搬动或摆放仍留在现场，不能算作成功。"),*SourceName,*HomeStopDescription(Reason));
    TGuardValue<bool> Changing(bChangingHomeArrangement,true);
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    StopHomeMovement();
    // 先改变本次状态，释放 SO、物理和能力时的旧回调只能看到已经结束的安排
    HomeState.Phase=Phase; HomeState.Reason=Reason;
    ReleaseHomeSeatMove(true);
    if (!WeakThis.IsValid() || !IsValid(GetOwner()) || HomeState.ArrangementId!=OldId) return;
    bHomeUseStarted=false; bPlacingHomeProp=false;
    ClearHomePropTransfer();
    ReleasedPlacementProp.Reset(); PlacementReleasedAt=0; PlacementStableSince=0;
    ActiveHomeChoice={}; HomeRequester.Reset();
    ReleaseHomeClaim(HomeUseClaim,Reason);
    if (!bKeepProp) ReleaseHomeProp(Reason);
    RefreshHomeCarryPose();
    if (bWasRunning && IsValid(GetOwner()) && !bHomeEndingPlay)
    {
        if (bSucceeded) RecordMemory(OldId,TEXT("HomeUseCompleted"),Description,0);
        GetCompanion()->ObserveFact(bSucceeded?TEXT("HomeArrangementCompleted"):TEXT("HomeArrangementStopped"),HomeState.Object,
            Description,EDeliveryNpcEvidenceSource::ActionResult);
    }
    if (bWasRunning) HomeUseFinished.Broadcast(OldId,bSucceeded,Reason);
    if (IsValid(GetOwner())) GetOwner()->ForceNetUpdate();
}

bool UDeliveryCompanionComponent::GetHomeHandPose(FVector& OutLocation,FVector& OutForward) const
{
    const auto* Npc=GetCompanion();
    if (!Npc || PropCarryOffset.ContainsNaN()) return false;
    FTransform Anchor=Npc->GetActorTransform();
    if (!PropCarrySocket.IsNone())
    {
        if (!Npc->GetMesh() || !Npc->GetMesh()->DoesSocketExist(PropCarrySocket)) return false;
        Anchor=Npc->GetMesh()->GetSocketTransform(PropCarrySocket,RTS_World);
    }
    OutLocation=Anchor.TransformPositionNoScale(PropCarryOffset);
    OutForward=Npc->GetActorForwardVector().GetSafeNormal2D();
    return !OutLocation.ContainsNaN() && !OutForward.IsNearlyZero();
}

bool UDeliveryCompanionComponent::IsHomePropLagging() const
{
    FVector Hand,Forward;
    return IsValid(HomeState.CarriedProp) && !bPlacingHomeProp && GetHomeHandPose(Hand,Forward) &&
        FVector::Dist(HomeState.CarriedProp->GetActorLocation(),Hand)>FMath::Clamp(PropCarryPauseDistance,20.f,100.f);
}

bool UDeliveryCompanionComponent::IsHomePropAtHand() const
{
    const auto* Prop=HomeState.CarriedProp.Get();
    FVector Hand,Forward;
    return IsValid(Prop) && GetHomeHandPose(Hand,Forward) &&
        FVector::Dist(Prop->GetActorLocation(),Hand)<=FMath::Min(4.f,PropCarryReachTolerance) &&
        FMath::Abs(FMath::FindDeltaAngleDegrees(Prop->GetActorRotation().Yaw,Forward.Rotation().Yaw))<=FMath::Min(5.f,HomeUseYawTolerance);
}

bool UDeliveryCompanionComponent::ValidateHomePropTransfer(FName& OutReason) const
{
    if (!HomePropTransferPoint.bEnabled) { OutReason=NAME_None; return true; }
    const auto* Object=HomePropTransferPoint.Object.Get();
    if (!Object) { OutReason=TEXT("HomePropTransferChanged"); return false; }
    return Object->ValidatePropTransferPoint(HomePropTransferPoint,OutReason);
}

void UDeliveryCompanionComponent::ClearHomePropTransfer()
{
    HomePropTransferPoint={};
    bMovingToPropTransferPoint=false;
}

void UDeliveryCompanionComponent::BeginHomePropSegment(bool bToTransferPoint)
{
    bMovingToPropTransferPoint=bToTransferPoint;
    if (const auto* Prop=HomeState.CarriedProp.Get())
    {
        // 每一段从实物当前位置开始，Grabbable 继续负责唯一的位移、转向和扫掠
        PropBlendFrom=Prop->GetActorLocation();
        PropBlendForward=Prop->GetActorForwardVector().GetSafeNormal2D();
        PropBlendStartedAt=GetWorld()->GetTimeSeconds();
    }
}

bool UDeliveryCompanionComponent::HasReachedHomePropTransferPoint() const
{
    const auto* Prop=HomeState.CarriedProp.Get();
    return bMovingToPropTransferPoint && HomePropTransferPoint.bEnabled && IsValid(Prop) &&
        GetWorld()->GetTimeSeconds()-PropBlendStartedAt>=FMath::Clamp(static_cast<double>(PropCarryBlendSeconds),.1,5.0) &&
        FVector::Dist(Prop->GetActorLocation(),HomePropTransferPoint.WorldPoint)<=FMath::Min(4.f,PropCarryReachTolerance) &&
        FMath::Abs(FMath::FindDeltaAngleDegrees(Prop->GetActorRotation().Yaw,PropBlendForward.Rotation().Yaw))<=FMath::Min(5.f,HomeUseYawTolerance);
}

bool UDeliveryCompanionComponent::GetObjectCarryPose(const AActor* Object,const FGuid& CarryId,FVector& OutLocation,FVector& OutForward) const
{
    const auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority() || bHomeEndingPlay || !IsValid(Object) || Object!=HomeState.CarriedProp ||
        CarryId!=HomePropCarryId || !CarryId.IsValid() || !CanCommand(OwnerPlayerState) || Npc->IsKnockedDown() ||
        IsUsingErrandVehicle() || GetCarriedItem() || (Npc->Combat && Npc->Combat->IsReactingToHit())) return false;
    FName TransferReason;
    if (!ValidateHomePropTransfer(TransferReason)) return false;
    FVector Target,Forward;
    if (!GetHomeHandPose(Target,Forward)) return false;
    if (bPlacingHomeProp) { Target=PropPlacementTarget.GetLocation(); Forward=PropPlacementTarget.GetRotation().GetForwardVector().GetSafeNormal2D(); }
    if (bMovingToPropTransferPoint)
    { Target=HomePropTransferPoint.WorldPoint; Forward=PropBlendForward; }
    const double BlendSeconds=FMath::Clamp(static_cast<double>(PropCarryBlendSeconds),.1,5.0);
    const float Alpha=static_cast<float>(FMath::Clamp((GetWorld()->GetTimeSeconds()-PropBlendStartedAt)/BlendSeconds,0.0,1.0));
    const float Smooth=Alpha*Alpha*(3.f-2.f*Alpha);
    OutLocation=FMath::Lerp(PropBlendFrom,Target,Smooth);
    OutForward=FMath::Lerp(PropBlendForward,Forward,Smooth).GetSafeNormal2D();
    if (OutForward.IsNearlyZero()) OutForward=Forward;
    return !OutLocation.ContainsNaN() && !OutForward.ContainsNaN();
}

bool UDeliveryCompanionComponent::BeginHomePropCarry(FName& OutReason)
{
    auto* Source=HomeTakeClaim.Object.Get();
    auto* Prop=Source?Source->GetOwner():nullptr;
    auto* Grabbable=IsValid(Prop)?Prop->FindComponentByClass<UDeliveryGrabbableComponent>():nullptr;
    if (!Source || !HomePropCarryId.IsValid() || !Grabbable || HomeState.CarriedProp || GetCarriedItem())
    { OutReason=TEXT("HomePropUnavailable"); return false; }
    HomeState.CarriedProp=Prop;
    HomeGrabbable=Grabbable;
    PropBlendFrom=Prop->GetActorLocation(); PropBlendForward=Prop->GetActorForwardVector().GetSafeNormal2D();
    PropBlendStartedAt=GetWorld()->GetTimeSeconds(); bPlacingHomeProp=false;
    const FGuid CarryId=HomePropCarryId;
    const FGuid ArrangementId=HomeState.ArrangementId;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<UDeliveryHomeObjectComponent> WeakSource(Source);
    const bool bCarryProviderRegistered=Grabbable->RegisterCarryProvider(this,CarryId);
    if (!WeakThis.IsValid()) return false;
    if (!bCarryProviderRegistered)
    {
        if (HomePropCarryId==CarryId) { HomeState.CarriedProp=nullptr; HomeGrabbable.Reset(); }
        OutReason=TEXT("HomePropCarryRejected"); return false;
    }
    if (WeakSource.IsValid() && IsCurrentHome(ArrangementId) && HomePropCarryId==CarryId && OwnsHomeSource(Source))
    {
        // 先建立真实搬运，再由物件保存原尺寸并缩小；任务包裹不经过此接口
        const bool bScaled=Source->BeginCarryScale(HomeTakeClaim,this,CarryId,OutReason);
        if (!WeakThis.IsValid()) return false;
        if (bScaled && WeakSource.IsValid() && IsCurrentHome(ArrangementId) &&
            HomePropCarryId==CarryId && OwnsHomeSource(Source)) return true;
        if (OutReason.IsNone()) OutReason=TEXT("HomePropScaleRejected");
        if (HomePropCarryId==CarryId) ReleaseHomeProp(OutReason);
        return false;
    }
    OutReason=TEXT("HomePropClaimChanged");
    return false;
}

void UDeliveryCompanionComponent::ReleaseHomeProp(FName Reason)
{
    ClearHomePropTransfer();
    if (!HomePropCarryId.IsValid() && !HomeGrabbable.IsValid() && !HomeState.CarriedProp && !HomeTakeClaim.Token.IsValid()) return;
    const FGuid OldId=HomePropCarryId;
    const TWeakObjectPtr<UDeliveryGrabbableComponent> Grabbable=HomeGrabbable;
    const TWeakObjectPtr<UDeliveryHomeObjectComponent> Source=HomeTakeClaim.Object;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    // 装饰物与任务包裹各有释放接口，这里从不调用 DropCarriedItem
    HomePropCarryId.Invalidate(); HomeGrabbable.Reset(); HomeState.CarriedProp=nullptr; bPlacingHomeProp=false;
    ReleaseHomeClaim(HomeTakeClaim,Reason);
    if (WeakThis.IsValid() && Grabbable.IsValid() && OldId.IsValid()) Grabbable->UnregisterCarryProvider(this,OldId);
    // 放大受阻时保留真实小物体，由物件等无搬运者且空间安全后恢复
    if (Source.IsValid()) Source->QueueScaleRestore();
    if (!WeakThis.IsValid()) return;
    if (!HasActiveHomeArrangement()) RefreshHomeCarryPose();
    if (IsValid(GetOwner())) GetOwner()->ForceNetUpdate();
}

void UDeliveryCompanionComponent::OnObjectCarryReleased(AActor* Object,const FGuid& CarryId,EDeliveryObjectCarryReleaseReason Reason)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || CarryId!=HomePropCarryId || !CarryId.IsValid()) return;
    const TWeakObjectPtr<UDeliveryHomeObjectComponent> Source=HomeTakeClaim.Object;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    FName TransferReason;
    const FName StopReason=ValidateHomePropTransfer(TransferReason)?FName(TEXT("HomePropCarryLost")):FName(TEXT("HomePropTransferChanged"));
    ClearHomePropTransfer();
    HomePropCarryId.Invalidate(); HomeGrabbable.Reset(); HomeState.CarriedProp=nullptr; bPlacingHomeProp=false;
    ReleaseHomeClaim(HomeTakeClaim,StopReason);
    if (Source.IsValid()) Source->QueueScaleRestore();
    if (!WeakThis.IsValid()) return;
    if (HasActiveHomeArrangement()) FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,StopReason);
    else
    {
        RefreshHomeCarryPose();
        if (!bHomeEndingPlay) GetCompanion()->ObserveFact(TEXT("HomePropReleased"),Object,
            TEXT("我已停止携带这件装饰物，物体留在实际松手的位置；没有把它算作已经摆放到目标槽。"),EDeliveryNpcEvidenceSource::ActionResult);
    }
}

void UDeliveryCompanionComponent::OnHomeObjectUseEnded(FGuid ActionId,FName Reason)
{
    if (bHomeEndingPlay || (bCommittingHomeUse && (Reason==TEXT("HomeAudioCommitted") || Reason==TEXT("HomePlacementCommitted")))) return;
    if (HomeUseClaim.ActionId==ActionId || HomeTakeClaim.ActionId==ActionId)
    {
        if (HasActiveHomeArrangement()) FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,Reason);
        else ReleaseHomeProp(Reason);
    }
}

void UDeliveryCompanionComponent::TickHomeLifecycle()
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority()) return;
    if (!HasActiveHomeArrangement() && !PendingHomeArrangementId.IsValid() && !HomeState.CarriedProp && !HomeTakeClaim.Token.IsValid()) return;
    if (Npc->IsKnockedDown() || !CanCommand(OwnerPlayerState))
    { CancelHomeArrangement(Npc->IsKnockedDown()?TEXT("HomeUserDown"):TEXT("HomeOwnerLost"),true); return; }
    FName TransferReason;
    if (!ValidateHomePropTransfer(TransferReason))
    {
        if (HasActiveHomeArrangement()) FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,TEXT("HomePropTransferChanged"));
        else ReleaseHomeProp(TEXT("HomePropTransferChanged"));
        return;
    }
    if ((HomeUseClaim.Token.IsValid() && !HomeUseClaim.Object.IsValid()) ||
        (HomeTakeClaim.Token.IsValid() && !HomeTakeClaim.Object.IsValid()))
    {
        if (HasActiveHomeArrangement()) FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,TEXT("HomeObjectDestroyed"));
        else ReleaseHomeProp(TEXT("HomeObjectDestroyed"));
        return;
    }
    if (HomeState.CarriedProp && (!HomeGrabbable.IsValid() || !HomeGrabbable->IsCarriedByProvider(this,HomePropCarryId)))
    {
        if (HasActiveHomeArrangement()) FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,TEXT("HomePropCarryLost"));
        else ReleaseHomeProp(TEXT("HomePropCarryLost"));
    }
    if (HomeTakeClaim.IsValid())
    {
        FName Reason;
        if (!HomeTakeClaim.Object->ValidateClaim(HomeTakeClaim,false,Reason))
        {
            if (HasActiveHomeArrangement()) FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,Reason);
            else ReleaseHomeProp(Reason);
        }
    }
    if (HasActiveHomeArrangement() && HomeUseClaim.IsValid())
    {
        FName Reason;
        if (!HomeUseClaim.Object->ValidateClaim(HomeUseClaim,false,Reason)) FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,Reason);
    }
}

void UDeliveryCompanionComponent::UpdateHomeArrangement()
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority() || bHomeEndingPlay) return;
    if (!HasActiveHomeArrangement()) { StartPendingHomeChoice(); return; }
    TickHomeLifecycle();
    if (!HasActiveHomeArrangement()) return;
    const FGuid Id=HomeState.ArrangementId;
    if (Npc->Agent->ShouldPauseForConversation() ||
        (Npc->Agent->HasActiveAction() && !Npc->Agent->CanContinueCompanionMovement()) ||
        (Npc->Combat && Npc->Combat->IsReactingToHit()))
    { StopHomeMovement(); SetSuspension(TEXT("OtherBehavior")); return; }
    if (!EnsureOnFoot()) { StopHomeMovement(); SetSuspension(TEXT("CompanionDismountBlocked")); return; }
    const double Now=GetWorld()->GetTimeSeconds();
    const float Dt=LastHomeUpdateAt>0?FMath::Clamp(static_cast<float>(Now-LastHomeUpdateAt),0.f,1.f):0.f;
    LastHomeUpdateAt=Now;
    const bool bSeated=HomeState.Operation==EDeliveryHomeOperation::Sit && HomeState.Phase==EDeliveryCompanionHomePhase::Using;
    if (!bSeated && HomeState.Phase!=EDeliveryCompanionHomePhase::LeaveUse) HomeActiveSeconds+=Dt;
    if (HomeActiveSeconds>FMath::Clamp(HomeArrangementTimeout,5.f,300.f) && HomeState.Phase!=EDeliveryCompanionHomePhase::LeaveUse)
    {
        if (NeedsHomeExit()) BeginLeavingHomeUse(TEXT("HomeArrangementTimedOut"));
        else FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,TEXT("HomeArrangementTimedOut"));
        return;
    }
    const bool bAtSource=HomeState.Phase==EDeliveryCompanionHomePhase::GoToObject &&
        (HomeState.Operation==EDeliveryHomeOperation::TakeProp || HomeState.Operation==EDeliveryHomeOperation::PlaceProp);
    const bool bTaking=HomeState.Phase==EDeliveryCompanionHomePhase::TakeProp;
    const auto& Claim=(bAtSource || bTaking)?HomeTakeClaim:HomeUseClaim;
    auto* Object=Claim.Object.Get();
    FTransform Entrance,Use,Exit;
    FName Reason;
    if (!Object || !Object->ValidateClaim(Claim,false,Reason) || !Object->GetUseTransforms(Claim,Entrance,Use,Exit))
    { FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,Reason.IsNone()?FName(TEXT("HomeClaimExpired")):Reason); return; }
    const auto* Definition=Object->GetUseDefinition(Claim);
    if (!Definition) { FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,TEXT("HomeDefinitionMissing")); return; }
    if (HomeState.Phase==EDeliveryCompanionHomePhase::GoToObject || HomeState.Phase==EDeliveryCompanionHomePhase::GoToDestination)
    {
        const bool bReached=MoveForHome(Entrance.GetLocation(),FMath::Max(HomeUseReachTolerance,12.f));
        LastHomeUpdateAt=Now;
        if (!bReached || !IsCurrentHome(Id)) return;
        // 到点后先按家具入口转身，实际手持位置才与已检查的朝向一致
        if ((bAtSource || HomeState.Operation==EDeliveryHomeOperation::PlaceProp || HomeState.Operation==EDeliveryHomeOperation::Sit) && !AlignForHome(Entrance)) return;
        // 转身后先等真实持件回到当前手前，不能拿转身前的位置直接开始桌面路线
        if (!bAtSource && HomeState.Operation==EDeliveryHomeOperation::PlaceProp && !IsHomePropAtHand())
        { SetSuspension(TEXT("HomePropReturningToHand")); return; }
        auto* ASC=HomeAbilitySystem.Get();
        if (!ASC || !HomeAbilityHandle.IsValid()) { FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,TEXT("HomeAbilityUnavailable")); return; }
        auto* Request=NewObject<UDeliveryHomeUseRequest>(Npc); Request->ArrangementId=Id;
        FGameplayEventData Event; Event.EventTag=TAG_Event_Home_Use; Event.Instigator=Npc; Event.Target=Object->GetOwner(); Event.OptionalObject=Request;
        ASC->HandleGameplayEvent(Event.EventTag,&Event);
        if (IsCurrentHome(Id) && !bHomeUseStarted) FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,TEXT("HomeAbilityRejected"));
        return;
    }
    if (HomeState.Phase==EDeliveryCompanionHomePhase::EnterUse)
    {
        if (HomeState.SeatMotion.bMoving)
        {
            if (!UpdateHomeSeatMove(Use,Reason))
            {
                if (IsCurrentHome(Id) && !Reason.IsNone()) FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,Reason);
                return;
            }
            if (!IsCurrentHome(Id)) return;
        }
        if (!AlignForHome(Use) || !IsCurrentHome(Id)) return;
        if (HomeState.PoseAnimation!=Definition->EnterAnimation) SetHomePose(Definition->EnterAnimation,false,false);
        const double Length=Definition->EnterAnimation?Definition->EnterAnimation->GetPlayLength():0;
        if (GetHomeServerTime()-HomeState.PoseStartedServerTime>=Length)
        {
            Npc->GetCharacterMovement()->bUseControllerDesiredRotation=false;
            SetHomePhase(EDeliveryCompanionHomePhase::Using); SetHomePose(Definition->Animation,true,false);
            const FString Description=FString::Printf(TEXT("我走到%s的%s，完成坐下过渡，现在正在这个座位上休息。"),
                *HomeObjectName(Object),*Definition->DisplayName.ToString().Left(64));
            RecordMemory(Id,TEXT("HomeSeated"),Description,0);
            Npc->ObserveFact(TEXT("HomeSeated"),Object->GetOwner(),Description,EDeliveryNpcEvidenceSource::ActionResult);
        }
        return;
    }
    if (HomeState.Phase==EDeliveryCompanionHomePhase::LeaveUse)
    {
        if (!bHomeExitAnimationFinished)
        {
            if (HomeState.SeatMotion.bMoving)
            {
                if (!UpdateHomeSeatMove(Use,Reason))
                {
                    if (IsCurrentHome(Id) && !Reason.IsNone()) FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,Reason);
                    return;
                }
                if (!IsCurrentHome(Id)) return;
                SetHomePose(Definition->ExitAnimation,false,false);
            }
            const double Length=Definition->ExitAnimation?Definition->ExitAnimation->GetPlayLength():0;
            if (Definition->SeatDismountAnimation && GetHomeServerTime()-HomeState.PoseStartedServerTime<Length) return;
            if (!AlignForHome(Exit) || !IsCurrentHome(Id)) return;
            UAnimSequence* Clip=Definition->SeatDismountAnimation?Definition->SeatDismountAnimation.Get():Definition->ExitAnimation.Get();
            if (!BeginHomeSeatMove(Exit,Clip,Reason))
            {
                if (!IsCurrentHome(Id)) return;
                SetSuspension(Reason);
                if (LastHomePause!=Reason)
                {
                    LastHomePause=Reason;
                    Npc->ObserveFact(TEXT("HomeExitBlocked"),Object->GetOwner(),TEXT("我已准备离开座位，但退出路线被挡住；我还保留这个座位，尚未开始下一项安排。"),EDeliveryNpcEvidenceSource::ActionResult);
                }
                return;
            }
            bHomeExitAnimationFinished=true;
        }
        if (UpdateHomeSeatMove(Exit,Reason))
        {
            if (IsCurrentHome(Id)) FinishHomeArrangement(EDeliveryCompanionHomePhase::Cancelled,HomeLeaveReason);
        }
        else if (IsCurrentHome(Id) && !Reason.IsNone()) FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,Reason);
        return;
    }
    if (HomeState.Phase==EDeliveryCompanionHomePhase::Using)
    {
        if (HomeState.Operation==EDeliveryHomeOperation::Sit)
        {
            if (!HomeState.SeatMotion.bActive || Npc->GetCharacterMovement()->MovementMode!=MOVE_Flying ||
                FVector::Dist(Npc->GetActorLocation(),HomeState.SeatMotion.Target)>FMath::Max(15.f,HomeUseReachTolerance*2))
                FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,TEXT("HomeSeatPositionChanged"));
            return;
        }
        if (Now-HomePhaseStartedAt<Definition->UseSeconds) return;
        TGuardValue<bool> Committing(bCommittingHomeUse,true);
        const bool bCommitted=Object->CommitAudioState(Claim,Reason);
        if (IsCurrentHome(Id)) FinishHomeArrangement(bCommitted?EDeliveryCompanionHomePhase::Completed:EDeliveryCompanionHomePhase::Failed,
            bCommitted?FName(TEXT("HomeAudioCommitted")):Reason);
        return;
    }
    if (bTaking)
    {
        FVector Target,Forward;
        auto* Prop=HomeState.CarriedProp.Get();
        if (!Prop || !OwnsHomeSource(Object) || !GetHomeHandPose(Target,Forward))
        { FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,TEXT("HomePropCarryLost")); return; }
        if (bMovingToPropTransferPoint)
        {
            if (HasReachedHomePropTransferPoint()) BeginHomePropSegment(false);
            return;
        }
        if (Now-PropBlendStartedAt<PropCarryBlendSeconds || FVector::Dist(Prop->GetActorLocation(),Target)>PropCarryReachTolerance ||
            FMath::Abs(FMath::FindDeltaAngleDegrees(Prop->GetActorRotation().Yaw,Forward.Rotation().Yaw))>HomeUseYawTolerance) return;
        ClearHomePropTransfer();
        if (HomeState.Operation==EDeliveryHomeOperation::TakeProp)
        { FinishHomeArrangement(EDeliveryCompanionHomePhase::Completed,TEXT("HomePropTaken"),true); return; }
        bHomeUseStarted=false;
        SetHomePhase(EDeliveryCompanionHomePhase::GoToDestination);
        HomeUseFinished.Broadcast(Id,true,TEXT("HomePropTaken"));
        return;
    }
    if (HomeState.Phase==EDeliveryCompanionHomePhase::PlaceProp)
    {
        auto* Source=ActiveHomeChoice.SourceObject.Get();
        auto* Prop=PlacementReleasedAt>0?ReleasedPlacementProp.Get():HomeState.CarriedProp.Get();
        if (!IsValid(Source) || !Prop || Source->GetOwner()!=Prop)
        {
            FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,
                PlacementReleasedAt>0?FName(TEXT("HomeReleasedPropTakenOrLost")):FName(TEXT("HomeCarriedPropChanged")));
            return;
        }
        FTransform Placement;
        // 松手前后都从同一实物的原尺寸重读目标，厚薄不同的抱枕可以共用支撑面槽
        if (!Object->GetPlacementTransform(Claim,Source,Placement,Reason) || !IsUprightHomePlacement(Placement))
        { FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,Reason.IsNone()?FName(TEXT("HomePlacementChanged")):Reason); return; }
        if (PlacementReleasedAt>0)
        {
            auto* Body=Prop?Cast<UPrimitiveComponent>(Prop->GetRootComponent()):nullptr;
            auto* Grabbable=Prop?Prop->FindComponentByClass<UDeliveryGrabbableComponent>():nullptr;
            if (!Prop || !Body || !Grabbable || Grabbable->HasAnyCarrier() || Prop->GetOwner())
            { FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,TEXT("HomeReleasedPropTakenOrLost")); return; }
            const bool bStill=Source && Source->GetOwner()==Prop && !Source->IsCarryScaleActive() &&
                Body->IsSimulatingPhysics() && Body->GetPhysicsLinearVelocity().Size()<=PropPlacementMaxSpeed &&
                Body->GetPhysicsAngularVelocityInDegrees().Size()<=PropPlacementMaxAngularSpeed &&
                FVector::Dist(Prop->GetActorLocation(),Placement.GetLocation())<=Definition->PlacementTolerance &&
                FMath::RadiansToDegrees(Prop->GetActorQuat().AngularDistance(Placement.GetRotation()))<=Definition->PlacementAngleTolerance &&
                Object->CanCommitPlacement(Claim,Prop,Reason);
            if (!bStill) PlacementStableSince=0;
            else if (PlacementStableSince<=0) PlacementStableSince=Now;
            if (PlacementStableSince>0 && Now-PlacementStableSince>=FMath::Clamp(PropPlacementSettleSeconds,.1f,2.f))
            {
                TGuardValue<bool> Committing(bCommittingHomeUse,true);
                const bool bCommitted=Object->CommitPlacement(Claim,Prop,Reason);
                if (IsCurrentHome(Id)) FinishHomeArrangement(bCommitted?EDeliveryCompanionHomePhase::Completed:EDeliveryCompanionHomePhase::Failed,
                    bCommitted?FName(TEXT("HomePlacementCommitted")):Reason);
            }
            else if (Now-PlacementReleasedAt>FMath::Clamp(PropPlacementSettleTimeout,1.f,15.f))
                FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,Reason.IsNone()?FName(TEXT("HomePlacementNotSettled")):Reason);
            return;
        }
        if (!OwnsHomeSource(Source) ||
            FVector::Dist(Placement.GetLocation(),PropPlacementTarget.GetLocation())>1.f ||
            FMath::RadiansToDegrees(Placement.GetRotation().AngularDistance(PropPlacementTarget.GetRotation()))>2.f)
        { FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,TEXT("HomePlacementChanged")); return; }
        if (bMovingToPropTransferPoint)
        {
            if (HasReachedHomePropTransferPoint()) BeginHomePropSegment(false);
            return;
        }
        if (Now-PropBlendStartedAt<PropCarryBlendSeconds ||
            FVector::Dist(Prop->GetActorLocation(),Placement.GetLocation())>FMath::Min(4.f,FMath::Min(PropCarryReachTolerance,Definition->PlacementTolerance)) ||
            FMath::RadiansToDegrees(Prop->GetActorQuat().AngularDistance(Placement.GetRotation()))>FMath::Min(3.f,FMath::Min(HomeUseYawTolerance,Definition->PlacementAngleTolerance))) return;
        FTransform FullSizeGoal=Placement;
        FullSizeGoal.SetScale3D(Source->GetOriginalActorScale());
        // 到完整尺寸的摆放点后先查展开空间，不能把手中的小尺寸当作槽位尺寸
        if (!Source->CanRestoreScaleAt(FullSizeGoal,Reason))
        { SetSuspension(TEXT("HomePropScaleRestoreBlocked")); return; }
        const TWeakObjectPtr<AActor> ActualProp(Prop);
        const TWeakObjectPtr<UDeliveryHomeObjectComponent> ActualSource(Source);
        ReleaseHomeProp(TEXT("HomePropPlacedForCheck"));
        if (!IsCurrentHome(Id)) return;
        // Grabbable 先释放原搬运与物理模式，恢复大小后才开始真实稳定观察
        const bool bRestored=ActualProp.IsValid() && ActualSource.IsValid() &&
            ActualSource->GetOwner()==ActualProp.Get() && ActualSource->TryRestoreScaleAt(FullSizeGoal,Reason);
        if (!IsCurrentHome(Id)) return;
        if (!bRestored)
        {
            if (ActualSource.IsValid()) ActualSource->QueueScaleRestore();
            FinishHomeArrangement(EDeliveryCompanionHomePhase::Failed,TEXT("HomePropScaleRestoreBlocked"));
            return;
        }
        ReleasedPlacementProp=ActualProp; PlacementReleasedAt=Now; PlacementStableSince=0;
        SetHomePose(nullptr,false,false);
    }
}

void UDeliveryCompanionComponent::ShutdownHomeExecution()
{
    bHomeEndingPlay=true;
    CancelHomeArrangement(TEXT("CompanionEndPlay"),true);
    ReleaseHomeSeatMove(true);
    const auto Handle=HomeAbilityHandle;
    HomeAbilityHandle={};
    auto* ASC=HomeAbilitySystem.Get(); HomeAbilitySystem.Reset();
    if (ASC && GetOwner() && GetOwner()->HasAuthority() && Handle.IsValid())
    { ASC->CancelAbilityHandle(Handle); ASC->ClearAbility(Handle); }
    HomeUseFinished.Clear();
}
