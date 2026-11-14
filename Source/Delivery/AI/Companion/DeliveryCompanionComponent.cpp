#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Conversation/DeliveryNpcConversationTrace.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Task/Tracking/DeliveryTaskTrackerComponent.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "AIController.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "Navigation/PathFollowingComponent.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

UDeliveryCompanionComponent::UDeliveryCompanionComponent()
{
    SetIsReplicatedByDefault(true);
    PrimaryComponentTick.bCanEverTick=true;
    PrimaryComponentTick.TickInterval=.25f;
}
ADeliveryCompanionCharacter* UDeliveryCompanionComponent::GetCompanion() const { return Cast<ADeliveryCompanionCharacter>(GetOwner()); }
const UDeliveryCompanionTuning& UDeliveryCompanionComponent::GetTuning() const
{
    return Tuning?*Tuning:*GetDefault<UDeliveryCompanionTuning>();
}
void UDeliveryCompanionComponent::BeginPlay()
{
    Super::BeginPlay();
    if (!GetOwner()->HasAuthority()) SetComponentTickEnabled(false);
}
bool UDeliveryCompanionComponent::CanRecruit(APawn* Helper) const
{
    const auto* Npc=GetCompanion();
    const auto* PC=IsValid(Helper)?Cast<APlayerController>(Helper->GetController()):nullptr;
    if (!Npc || !Npc->IsWaitingForHelp() || !Npc->HasFinishedEncounterFall() || !PC || !IsValid(PC->PlayerState) ||
        Helper->GetWorld()!=GetWorld() || PC->PlayerState->GetWorld()!=GetWorld() || OwnerPlayerState ||
        FVector::DistSquared(Helper->GetActorLocation(),Npc->GetActorLocation())>FMath::Square(240.f)) return false;
    // 扶起不能沿用会忽略所有行人和车辆的交谈射线，只排除本次交互的双方
    FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionHelp),false,Npc);
    Params.AddIgnoredActor(Helper);
    FCollisionObjectQueryParams Objects;
    for (const auto Channel:{ECC_WorldStatic,ECC_WorldDynamic,ECC_Pawn,ECC_Vehicle,ECC_PhysicsBody}) Objects.AddObjectTypesToQuery(Channel);
    FHitResult Obstacle;
    if (GetWorld()->LineTraceSingleByObjectType(Obstacle,Npc->GetActorLocation(),Helper->GetActorLocation(),Objects,Params)) return false;
    for (TActorIterator<ADeliveryCompanionCharacter> It(GetWorld());It;++It)
        if (*It!=Npc && It->Companion->GetOwnerPlayerState()==PC->PlayerState) return false;
    return true;
}
bool UDeliveryCompanionComponent::TryRecruit(APawn* Helper)
{
    if (!GetOwner()->HasAuthority() || !CanRecruit(Helper)) return false;
    OwnerPlayerState=Helper->GetController()->PlayerState;
    Mode=EDeliveryCompanionMode::Follow;
    Affinity=GetTuning().InitialAffinity;
    RecordRecruitMemory();
    ++CommandRevision;
    GetCompanion()->BeginHelpUp();
    RefreshOwnerCombatSubscription();
    GetOwner()->ForceNetUpdate();
    return true;
}
bool UDeliveryCompanionComponent::CanCommand(APlayerState* Requester) const
{
    return IsValid(Requester) && Requester==OwnerPlayerState && Requester->GetWorld()==GetWorld() && !Requester->IsInactive();
}
APawn* UDeliveryCompanionComponent::GetOwnerPawn() const
{
    const auto* PC=IsValid(OwnerPlayerState)?OwnerPlayerState->GetPlayerController():nullptr;
    return PC && PC->GetWorld()==GetWorld()?PC->GetPawn():nullptr;
}
bool UDeliveryCompanionComponent::ApplyCommand(EDeliveryNpcAction Action,APlayerState* Requester,int32 ExpectedRevision,
    UDeliveryTaskDefinition* ExpectedTask,ADeliveryPackageItem* ExpectedPackage)
{
    if (!GetOwner()->HasAuthority() || bChangingHomeArrangement || ExpectedRevision!=CommandRevision || !DeliveryNpc::IsCompanionCommand(Action)) return false;
    // 家居必须携带当次真实候选，不能把空 HomeAction 当成通用等候命令
    if (Action==EDeliveryNpcAction::HomeAction) return false;
    if (Action==EDeliveryNpcAction::OfferCaterpillar)
    {
        if (!CanRequestCaterpillarOffer(Requester)) return false;
        bCaterpillarOffered=true;
        CaterpillarOpportunityId.Invalidate();
        ++CommandRevision;
        GetOwner()->ForceNetUpdate();
        return true;
    }
    if (!CanCommand(Requester)) return false;
    if (Action==EDeliveryNpcAction::GoHome && !CanGoHome(Requester)) return false;
    if (DeliveryNpc::IsCompanionErrand(Action))
    {
        const bool bStarted=StartErrand(Action,Requester,ExpectedRevision,ExpectedTask,ExpectedPackage);
        if (bStarted)
        {
            CancelAssist(TEXT("OwnerChangedArrangement"));
            bCaterpillarOffered=false; CaterpillarOpportunityId.Invalidate(); bAnnouncedCaterpillarOpportunity=true;
        }
        return bStarted;
    }
    // 新模式先记入，仍在座位上时由持续分支完成真实离座后再开始移动
    const bool bKeepTakenProp=(Action==EDeliveryNpcAction::Follow || Action==EDeliveryNpcAction::Stay) &&
        !HasActiveHomeArrangement() && OwnsHomeSource(HomeTakeClaim.Object.Get());
    if (!bKeepTakenProp) CancelHomeArrangement(TEXT("OwnerChangedArrangement"));
    bCaterpillarOffered=false;
    CaterpillarOpportunityId.Invalidate();
    bAnnouncedCaterpillarOpportunity=true;
    CancelAssist(TEXT("OwnerChangedArrangement"));
    if (HasActiveErrand()) FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("OwnerChangedArrangement"));
    StopFollowMovement();
    Mode=Action==EDeliveryNpcAction::Follow?EDeliveryCompanionMode::Follow:
        Action==EDeliveryNpcAction::GoHome?EDeliveryCompanionMode::GoHome:EDeliveryCompanionMode::Stay;
    WaitLocation=GetCompanion()->GetNavAgentLocation();
    ++CommandRevision;
    bHasFollowDestination=false;
    NextNavigationAt=0;
    SetSuspension(NAME_None);
    GetOwner()->ForceNetUpdate();
    return true;
}
void UDeliveryCompanionComponent::ExtendDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const
{
    Request.bHasCompanionContext=true;
    ExtendWorldDecisionRequest(Request);
    Request.CompanionOwnerName=IsValid(OwnerPlayerState)?OwnerPlayerState->GetPlayerName().Left(64):TEXT("");
    Request.CompanionRelationshipStage=GetRelationshipStage();
    Request.bCanOfferCaterpillar=HasCaterpillarOpportunity();
    Request.CompanionMemories=Memories;
    Request.bRequesterIsCompanionOwner=CanCommand(Requester);
    Request.CompanionCommandRevision=CommandRevision;
    Request.CompanionMode=StaticEnum<EDeliveryCompanionMode>()->GetNameStringByValue(static_cast<int64>(Mode));
    Request.CompanionSuspensionReason=SuspensionReason.ToString();
    Request.CompanionTask=GetCommandTask();
    Request.CompanionPackage=GetCommandPackage();
    Request.bCompanionHasPackage=GetCarriedTaskPackage()!=nullptr;
    Request.CompanionErrandAction=DeliveryNpc::ActionName(Errand.Action);
    Request.CompanionErrandPhase=StaticEnum<EDeliveryCompanionErrandPhase>()->GetNameStringByValue(static_cast<int64>(Errand.Phase));
    Request.CompanionErrandStatus=StaticEnum<EDeliveryNpcActionStatus>()->GetNameStringByValue(static_cast<int64>(Errand.Status));
    Request.CompanionErrandReason=Errand.Reason.ToString();
    if (const auto* Task=GetCommandTask())
    {
        Request.CompanionTaskId=Task->TaskId.ToString();
        Request.CompanionTaskTitle=Task->DisplayName.ToString().Left(128);
        // 只查询主人当前手中或正在驾驶车辆上的包裹，不由共享订单阶段推断持有人
        const auto* Pawn=GetOwnerPawn();
        const auto* Bike=Cast<ADeliveryMotorbike>(Pawn);
        const auto* Character=Bike?Bike->GetDriver():Cast<ADeliveryCharacter>(Pawn);
        const auto* Package=Character?Character->GetCarriedTaskPackage():nullptr;
        const auto* Item=IsValid(Package)?Package->GetDeliveryItemComponent():nullptr;
        Request.bCompanionOwnerHasPackage=Item && Item->OwningTask==Task;
        if (const auto* Manager=UDeliveryTaskManagerComponent::Get(this))
            Request.CompanionTaskPhase=StaticEnum<EDeliveryTaskStatus>()->GetNameStringByValue(static_cast<int64>(Manager->GetTaskStatus(Task)));
    }
    if (!GetCompanion()->IsKnockedDown() && CanRequestCaterpillarOffer(Requester))
    {
        Request.AllowedActions.AddUnique(EDeliveryNpcAction::OfferCaterpillar);
        FDeliveryNpcCandidate Candidate; Candidate.Action=EDeliveryNpcAction::OfferCaterpillar;
        if (!Requester) Candidate.EvidenceIds.Add(CaterpillarOpportunityId);
        Request.Candidates.Add(Candidate);
    }
    if (!CanCommand(Requester) || GetCompanion()->IsKnockedDown()) return;
    for (const auto Action:{EDeliveryNpcAction::Follow,EDeliveryNpcAction::Stay,EDeliveryNpcAction::CancelCompanionTask})
    {
        Request.AllowedActions.AddUnique(Action);
        FDeliveryNpcCandidate Candidate; Candidate.Action=Action;
        Request.Candidates.Add(Candidate);
    }
    for (const auto Action:{EDeliveryNpcAction::FetchPackage,EDeliveryNpcAction::TakePackage,EDeliveryNpcAction::DeliverPackage,EDeliveryNpcAction::ReturnPackage})
        if (CanArrangeErrand(Action,Requester))
        {
            Request.AllowedActions.AddUnique(Action);
            FDeliveryNpcCandidate Candidate; Candidate.Action=Action;
            Request.Candidates.Add(Candidate);
        }
    if (CanGoHome(Requester))
    {
        Request.AllowedActions.AddUnique(EDeliveryNpcAction::GoHome);
        FDeliveryNpcCandidate Candidate; Candidate.Action=EDeliveryNpcAction::GoHome;
        Request.Candidates.Add(Candidate);
    }
}
void UDeliveryCompanionComponent::StopFollowMovement()
{
    if (!GetOwner()->HasAuthority()) return;
    StopHomeMovement();
    StopVehicleTravel(EDeliveryCompanionTravelPurpose::Follow);
    StopVehicleTravel(EDeliveryCompanionTravelPurpose::ReturnToStay);
    StopVehicleTravel(EDeliveryCompanionTravelPurpose::GoHome);
    CancelCatchUp();
    if (!bOwnsNavigation) return;
    const FAIRequestID OldId=FollowMoveId;
    FollowMoveId=FAIRequestID::InvalidRequest;
    bOwnsNavigation=false;
    if (const auto* AI=Cast<AAIController>(GetCompanion()->GetController()))
        if (auto* Path=AI->GetPathFollowingComponent(); Path && OldId.IsValid() && Path->GetCurrentRequestId()==OldId)
            Path->AbortMove(*this,FPathFollowingResultFlags::UserAbort,OldId);
}
void UDeliveryCompanionComponent::SetSuspension(FName Reason)
{
    if (SuspensionReason==Reason) return;
    SuspensionReason=Reason;
    GetOwner()->ForceNetUpdate();
}
void UDeliveryCompanionComponent::ReleaseOwner()
{
    CancelHomeArrangement(TEXT("OwnerLost"),true);
    GetCompanion()->CancelAssistedHelp();
    CancelAssist(TEXT("OwnerLost"));
    UnbindOwnerCombat();
    RecentAssistHits.Reset();
    ClearCompanionMemories();
    ReleaseErrandVehicle();
    if (HasActiveErrand()) FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("OwnerLost"));
    Errand={};
    StopFollowMovement();
    DropCarriedItem();
    GetCompanion()->Agent->ResetContext(TEXT("CompanionOwnerLost"));
    GetCompanion()->Presentation->CancelSpeech();
    OwnerPlayerState=nullptr; Mode=EDeliveryCompanionMode::Unrecruited;
    ++CommandRevision; bHasFollowDestination=false; bReportedBlocked=false;
    SetSuspension(NAME_None);
    GetCompanion()->ResetForNewOwner();
    GetOwner()->ForceNetUpdate();
}
void UDeliveryCompanionComponent::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn)
{
    Super::TickComponent(Dt,Type,Fn);
    if (!GetOwner()->HasAuthority()) return;
    if (!BoundTaskManager.IsValid())
        if (auto* Manager=UDeliveryTaskManagerComponent::Get(this))
        {
            BoundTaskManager=Manager;
            Manager->OnTaskCompleted.AddUniqueDynamic(this,&UDeliveryCompanionComponent::OnTaskCompleted);
        }
    if (Mode!=EDeliveryCompanionMode::Unrecruited && (!IsValid(OwnerPlayerState) || OwnerPlayerState->IsInactive() ||
        OwnerPlayerState->GetWorld()!=GetWorld() || !IsValid(OwnerPlayerState->GetPlayerController()))) ReleaseOwner();
    RefreshOwnerCombatSubscription();
    TickHomeLifecycle();
    UpdateAssist();
    UpdateCaterpillarOpportunity();
    if (IsValid(OwnerPlayerState))
    {
        const auto* Pawn=GetOwnerPawn();
        if (GetCompanion()->IsKnockedDown()) SetSuspension(TEXT("Down"));
        else if (GetCompanion()->Agent->ShouldPauseForConversation() || GetCompanion()->Agent->HasActiveAction()) SetSuspension(TEXT("OtherBehavior"));
        // 只有跟随等主人恢复步行，独立送取、返回等候点和回家保持自己的安排
        else if (!HasActiveErrand() && !HasActiveHomeArrangement() && Mode==EDeliveryCompanionMode::Follow && !Pawn) SetSuspension(TEXT("OwnerChangingPawn"));
        else if (!HasActiveErrand() && !HasActiveHomeArrangement() && Mode==EDeliveryCompanionMode::Follow && Pawn && Pawn->IsA<ADeliveryMotorbike>()) SetSuspension(TEXT("OwnerRiding"));
    }
}
bool UDeliveryCompanionComponent::IsDestinationClear(const FVector& Destination) const
{
    const auto* Npc=GetCompanion();
    FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionSlot),false,Npc);
    const auto* Capsule=Npc->GetCapsuleComponent();
    const float Half=Capsule->GetScaledCapsuleHalfHeight();
    // 导航点在地面，检测胶囊时略抬高底部，避免把地板当成堵路物
    return !GetWorld()->OverlapBlockingTestByChannel(Destination+FVector(0,0,Half+3),FQuat::Identity,ECC_Pawn,
        FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius()+8,Half),Params);
}
bool UDeliveryCompanionComponent::IsOutsideTaskEntrance(const FVector& Destination) const
{
    const auto* Tracker=UDeliveryTaskTrackerComponent::FindTracker(OwnerPlayerState);
    FVector TaskDestination; FName LocationId;
    return !Tracker || !Tracker->GetTrackedTaskDestination(TaskDestination,LocationId) ||
        FVector::Dist2D(Destination,TaskDestination)>=GetTuning().DestinationClearance;
}
bool UDeliveryCompanionComponent::FindFollowDestination(APawn* Pawn,FVector& Out) const
{
    // 等候不能借用主人前方的候选，否则主人走远后会被避让逻辑自动召回
    if (Mode!=EDeliveryCompanionMode::Follow) return false;
    auto* Npc=GetCompanion();
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!Nav || !Pawn) return false;
    const auto& Config=GetTuning();
    FVector Forward=Pawn->GetVelocity().GetSafeNormal2D();
    if (Forward.IsNearlyZero()) Forward=Pawn->GetActorForwardVector().GetSafeNormal2D();
    const bool bFollowingBehind=Affinity<50;
    FVector Lead=Pawn->GetActorLocation()+Forward*(bFollowingBehind?-FMath::Max(Config.YieldDistance+80.f,180.f):Config.LeadDistance);
    FVector TaskDestination; FName LocationId;
    const auto* Tracker=UDeliveryTaskTrackerComponent::FindTracker(OwnerPlayerState);
    const bool bHasTask=Tracker && Tracker->GetTrackedTaskDestination(TaskDestination,LocationId);
    if (bHasTask && !bFollowingBehind)
    {
        auto* Route=Nav->FindPathToLocationSynchronously(GetWorld(),Pawn->GetActorLocation(),TaskDestination,Npc);
        if (Route && Route->IsValid() && !Route->IsPartial() && Route->PathPoints.Num()>1)
        {
            float Remaining=Config.LeadDistance;
            Lead=Route->PathPoints[0];
            for (int32 Index=1;Index<Route->PathPoints.Num();++Index)
            {
                const FVector Segment=Route->PathPoints[Index]-Lead;
                const float Length=Segment.Size();
                if (Length>Remaining) { Forward=Segment.GetSafeNormal2D(); Lead+=Segment.GetSafeNormal()*Remaining; break; }
                Remaining-=Length; Lead=Route->PathPoints[Index]; Forward=Segment.GetSafeNormal2D();
            }
        }
    }
    const FVector Right=FVector::CrossProduct(FVector::UpVector,Forward).GetSafeNormal();
    TArray<FVector> Candidates;
    // 主人在入口附近时绕入口选点，侧偏必须大于入口净空，不能固定150厘米后又按220厘米拒绝
    if (bHasTask && FVector::Dist2D(Lead,TaskDestination)<Config.DestinationClearance)
    {
        FVector Away=(Pawn->GetActorLocation()-TaskDestination).GetSafeNormal2D();
        if (Away.IsNearlyZero()) Away=-Forward;
        const float ClearRadius=Config.DestinationClearance+Npc->GetSimpleCollisionRadius()+40;
        for (float Angle:{60.f,-60.f,0.f,100.f,-100.f})
            Candidates.Add(TaskDestination+Away.RotateAngleAxis(Angle,FVector::UpVector)*ClearRadius);
    }
    else
    {
        for (float Scale:{1.f,-1.f,.5f,-.5f}) Candidates.Add(Lead+Right*Config.SideDistance*Scale);
        // 窄路两侧放不下身体时，仍可沿路线保持前方距离
        Candidates.Add(Lead);
    }
    for (const FVector& Desired:Candidates)
    {
        FNavLocation Projected;
        if (!Nav->ProjectPointToNavigation(Desired,Projected,FVector(80,80,250),&Npc->GetNavAgentPropertiesRef()) ||
            !IsDestinationClear(Projected.Location) ||
            FVector::Dist2D(Projected.Location,Pawn->GetActorLocation())<Config.YieldDistance ||
            (bHasTask && FVector::Dist2D(Projected.Location,TaskDestination)<Config.DestinationClearance)) continue;
        auto* Path=Nav->FindPathToLocationSynchronously(GetWorld(),Npc->GetNavAgentLocation(),Projected.Location,Npc);
        if (!Path || !Path->IsValid() || Path->IsPartial()) continue;
        Out=Projected.Location; return true;
    }
    return false;
}
bool UDeliveryCompanionComponent::FindWaitDestination(APawn* Pawn,FVector& Out) const
{
    if (Mode!=EDeliveryCompanionMode::Stay || !Pawn) return false;
    const auto* Npc=GetCompanion();
    const auto& Config=GetTuning();
    const FVector Start=Npc->GetNavAgentLocation();
    FVector Away=(Start-Pawn->GetActorLocation()).GetSafeNormal2D();
    if (Away.IsNearlyZero()) Away=Npc->GetActorForwardVector().GetSafeNormal2D();
    // 从身边找空位，路程与最终离等候锚点的距离都受限，不能绕远路追随主人
    for (const float Radius:{120.f,240.f,360.f})
        for (const float Angle:{0.f,45.f,-45.f,90.f,-90.f,135.f,-135.f,180.f})
        {
            const float Step=FMath::Min(Radius,Config.WaitYieldRadius);
            const FVector Desired=Start+Away.RotateAngleAxis(Angle,FVector::UpVector)*Step;
            FVector Destination;
            if (FVector::Dist2D(Desired,WaitLocation)>Config.WaitYieldRadius ||
                !Npc->FindNearbyWalkDestination(Desired,Step*1.5f,Destination) ||
                FVector::Dist2D(Destination,WaitLocation)>Config.WaitYieldRadius ||
                FVector::Dist2D(Destination,Pawn->GetActorLocation())<Config.YieldDistance ||
                !IsDestinationClear(Destination) || !IsOutsideTaskEntrance(Destination)) continue;
            Out=Destination;
            return true;
        }
    return false;
}
void UDeliveryCompanionComponent::UpdateFollow()
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority()) return;
    if (HasActiveHomeArrangement() || PendingHomeArrangementId.IsValid()) { UpdateHomeArrangement(); return; }
    if (HasActiveErrand()) { StopFollowMovement(); return; }
    if (Mode==EDeliveryCompanionMode::GoHome) { UpdateGoHome(); return; }
    auto* AI=Cast<AAIController>(Npc->GetController());
    auto* Pawn=GetOwnerPawn();
    FName Block;
    if (!IsValid(OwnerPlayerState)) Block=TEXT("NoOwner");
    else if (Npc->IsKnockedDown()) Block=TEXT("Down");
    else if (Mode==EDeliveryCompanionMode::Follow && !Pawn) Block=TEXT("OwnerChangingPawn");
    else if (Mode==EDeliveryCompanionMode::Follow && Pawn->IsA<ADeliveryMotorbike>()) Block=TEXT("OwnerRiding");
    else if (Npc->Agent->ShouldPauseForConversation() || Npc->Agent->HasActiveAction()) Block=TEXT("OtherBehavior");
    if (!Block.IsNone() || !AI)
    { StopFollowMovement(); bHasFollowDestination=false; SetSuspension(Block); return; }
    // 远处同行与返程沿真实路径取回自己的车，不用隐藏补位跳过旅程
    CancelCatchUp();
    // 装饰物保持由步行的真实手前目标搬运，不随取车分支附到车辆
    if (HomeState.CarriedProp && !EnsureOnFoot()) { SetSuspension(TEXT("CompanionDismountBlocked")); return; }
    if (IsHomePropLagging()) { StopFollowMovement(); bHasFollowDestination=false; SetSuspension(TEXT("HomePropBlocked")); return; }
    if (!HomeState.CarriedProp && Mode==EDeliveryCompanionMode::Follow &&
        !UpdateVehicleTravel(Pawn->GetActorLocation(),GetTuning().YieldDistance,EDeliveryCompanionTravelPurpose::Follow)) return;
    const double Now=GetWorld()->GetTimeSeconds();
    if (Mode==EDeliveryCompanionMode::Stay)
    {
        const float DistanceToWait=FVector::Dist2D(Npc->GetNavAgentLocation(),WaitLocation);
        const bool bReturning=DistanceToWait>GetTuning().WaitYieldRadius ||
            (VehicleTravelPurpose==EDeliveryCompanionTravelPurpose::ReturnToStay && DistanceToWait>60.f);
        if (bReturning)
        {
            if (!HomeState.CarriedProp && !UpdateVehicleTravel(WaitLocation,60.f,EDeliveryCompanionTravelPurpose::ReturnToStay)) return;
            if (Now<NextNavigationAt) return;
            NextNavigationAt=Now+GetTuning().NavigationInterval;
            auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
            FNavLocation WaitPoint;
            if (!Nav || !Nav->ProjectPointToNavigation(WaitLocation,WaitPoint,FVector(90,90,250),&Npc->GetNavAgentPropertiesRef()) ||
                !IsDestinationClear(WaitPoint.Location))
            { StopFollowMovement(); SetSuspension(TEXT("WaitReturnBlocked")); NextNavigationAt=Now+GetTuning().BlockedRetrySeconds; return; }
            FAIMoveRequest Return(WaitPoint.Location);
            Return.SetAcceptanceRadius(40); Return.SetAllowPartialPath(false);
            Return.SetReachTestIncludesAgentRadius(false); Return.SetReachTestIncludesGoalRadius(false);
            auto* Following=AI->GetPathFollowingComponent();
            if (Following && Following->HasReached(Return))
            {
                StopFollowMovement(); ReleaseErrandVehicle(); SetSuspension(NAME_None);
                return;
            }
            if (bOwnsNavigation && Following && Following->GetCurrentRequestId()==FollowMoveId && AI->GetMoveStatus()!=EPathFollowingStatus::Idle) return;
            const auto Result=AI->MoveTo(Return);
            bOwnsNavigation=Result.Code==EPathFollowingRequestResult::RequestSuccessful;
            FollowMoveId=bOwnsNavigation?Result.MoveId:FAIRequestID::InvalidRequest;
            SetSuspension(Result.Code==EPathFollowingRequestResult::Failed?FName(TEXT("WaitReturnBlocked")):NAME_None);
            if (Result.Code==EPathFollowingRequestResult::Failed) NextNavigationAt=Now+GetTuning().BlockedRetrySeconds;
            return;
        }
        if (VehicleTravelPurpose==EDeliveryCompanionTravelPurpose::ReturnToStay)
        { StopFollowMovement(); ReleaseErrandVehicle(); }
        if (!EnsureOnFoot()) { SetSuspension(TEXT("CompanionDismountBlocked")); return; }
    }
    if (Now<NextNavigationAt) return;
    NextNavigationAt=Now+GetTuning().NavigationInterval;
    FVector Destination;
    const bool bTooClose=Pawn && FVector::DistSquared2D(Pawn->GetActorLocation(),Npc->GetActorLocation())<FMath::Square(GetTuning().YieldDistance);
    const bool bBlocksEntrance=!IsOutsideTaskEntrance(Npc->GetNavAgentLocation());
    bool bFound=false;
    if (bTooClose || bBlocksEntrance)
    {
        bFound=Pawn && Npc->FindSideStepDestination(Pawn,Destination) &&
            IsDestinationClear(Destination) && IsOutsideTaskEntrance(Destination) &&
            (Mode!=EDeliveryCompanionMode::Stay || FVector::Dist2D(Destination,WaitLocation)<=GetTuning().WaitYieldRadius);
        if (!bFound) bFound=Mode==EDeliveryCompanionMode::Stay?FindWaitDestination(Pawn,Destination):FindFollowDestination(Pawn,Destination);
    }
    if (!bFound && Mode==EDeliveryCompanionMode::Stay && !bTooClose && !bBlocksEntrance)
    {
        if (!bOwnsNavigation || AI->GetMoveStatus()==EPathFollowingStatus::Idle) StopFollowMovement();
        bReportedBlocked=false;
        SetSuspension(NAME_None);
        return;
    }
    if (!bFound && Mode==EDeliveryCompanionMode::Follow && bHasFollowDestination && FVector::DistSquared2D(Pawn->GetActorLocation(),LastOwnerPosition)<FMath::Square(GetTuning().RepathDistance))
    {
        if (AI->GetMoveStatus()!=EPathFollowingStatus::Idle || FVector::Dist2D(Npc->GetNavAgentLocation(),LastDestination)<65) return;
        bHasFollowDestination=false;
    }
    if (!bFound && Mode==EDeliveryCompanionMode::Follow) bFound=FindFollowDestination(Pawn,Destination);
    if (!bFound)
    {
        const bool bWaiting=Mode==EDeliveryCompanionMode::Stay;
        StopFollowMovement(); SetSuspension(bWaiting?TEXT("WaitSpaceBlocked"):TEXT("PathBlocked")); bHasFollowDestination=false;
        NextNavigationAt=Now+GetTuning().BlockedRetrySeconds;
        if (!bReportedBlocked)
        {
            bReportedBlocked=true;
            Npc->ObserveFact(bWaiting?TEXT("CompanionWaitSpaceBlocked"):TEXT("CompanionPathBlocked"),Pawn,
                bWaiting?TEXT("主人让我在这里等候，但身边暂时找不到可让开入口或来人的空位；我仍留在附近等候，没有自动追随主人。"):
                TEXT("当前跟随路线被挡住，我停在可走的位置等待；没有到达主人身边，也没有穿过障碍。"),EDeliveryNpcEvidenceSource::ActionResult);
        }
        return;
    }
    if (bOwnsNavigation && FVector::DistSquared2D(Destination,LastDestination)<FMath::Square(60.f) && AI->GetMoveStatus()!=EPathFollowingStatus::Idle) return;
    FAIMoveRequest Move(Destination);
    Move.SetAcceptanceRadius(40); Move.SetAllowPartialPath(false);
    Move.SetReachTestIncludesAgentRadius(false); Move.SetReachTestIncludesGoalRadius(false);
    const auto Result=AI->MoveTo(Move);
    bOwnsNavigation=Result.Code==EPathFollowingRequestResult::RequestSuccessful;
    FollowMoveId=bOwnsNavigation?Result.MoveId:FAIRequestID::InvalidRequest;
    bHasFollowDestination=Result.Code!=EPathFollowingRequestResult::Failed;
    if (!bHasFollowDestination) { SetSuspension(TEXT("PathBlocked")); NextNavigationAt=Now+GetTuning().BlockedRetrySeconds; return; }
    LastDestination=Destination; LastOwnerPosition=Pawn?Pawn->GetActorLocation():FVector::ZeroVector;
    // 让路不改原等候锚点，避免多次挪动逐渐离开玩家指定的区域
    bReportedBlocked=false; SetSuspension(NAME_None);
}
void UDeliveryCompanionComponent::OnTaskCompleted(UDeliveryTaskDefinition* Task,const FDeliveryRewardBreakdown& Reward,APlayerState* Deliverer)
{
    auto* Pawn=GetOwnerPawn();
    if (!GetOwner()->HasAuthority() || Deliverer!=OwnerPlayerState || !Task) return;
    // 记忆与即时陪伴事实共用实际交付、三维距离和无遮挡检查，也共用订单去重
    if (!RecordDeliveryMemory(Task,Deliverer)) return;
    const auto* Package=GetCarriedTaskPackage();
    // 完成通知先于持件清理，自己帮送的结果留给实际调用交付的执行者确认
    if (Package && Package->GetDeliveryItemComponent()->OwningTask==Task) return;
    if (!Pawn) return;
    GetCompanion()->ObserveFact(TEXT("OwnerDeliveryCompleted"),Pawn,
        FString::Printf(TEXT("主人刚完成了订单 %s，我在附近陪着；包裹交付和奖励已经由游戏确认，我没有替他交件。"),*Task->TaskId.ToString()),EDeliveryNpcEvidenceSource::ActionResult);
}
void UDeliveryCompanionComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    ShutdownHomeExecution();
    if (auto* Npc=GetCompanion()) Npc->CancelAssistedHelp();
    CancelAssist(TEXT("CompanionEndPlay"));
    UnbindOwnerCombat();
    RecentAssistHits.Reset();
    ReleaseErrandVehicle();
    StopErrandMovement();
    Errand={};
    if (GetOwner()->HasAuthority() && Reason==EEndPlayReason::Destroyed) DropCarriedItem();
    if (IsValid(CarriedPackage)) CarriedPackage->OnDestroyed.RemoveDynamic(this,&UDeliveryCompanionComponent::OnCarriedPackageDestroyed);
    if (BoundTaskManager.IsValid()) BoundTaskManager->OnTaskCompleted.RemoveDynamic(this,&UDeliveryCompanionComponent::OnTaskCompleted);
    StopFollowMovement(); BoundTaskManager.Reset();
    Super::EndPlay(Reason);
}
void UDeliveryCompanionComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UDeliveryCompanionComponent,OwnerPlayerState);
    DOREPLIFETIME(UDeliveryCompanionComponent,Mode);
    DOREPLIFETIME(UDeliveryCompanionComponent,CommandRevision);
    DOREPLIFETIME(UDeliveryCompanionComponent,SuspensionReason);
    DOREPLIFETIME(UDeliveryCompanionComponent,Affinity);
    DOREPLIFETIME(UDeliveryCompanionComponent,AssistTarget);
    DOREPLIFETIME(UDeliveryCompanionComponent,AssistRevision);
    DOREPLIFETIME(UDeliveryCompanionComponent,bCaterpillarOffered);
    DOREPLIFETIME(UDeliveryCompanionComponent,bCaterpillarGiven);
    DOREPLIFETIME(UDeliveryCompanionComponent,CarriedPackage);
    DOREPLIFETIME(UDeliveryCompanionComponent,Errand);
    DOREPLIFETIME(UDeliveryCompanionComponent,ErrandVehicle);
    DOREPLIFETIME(UDeliveryCompanionComponent,HomeState);
}
