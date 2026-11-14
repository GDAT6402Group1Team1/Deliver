#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Conversation/DeliveryNpcConversationTrace.h"
#include "Character/DeliveryCharacter.h"
#include "Carry/DeliveryPackageCarrierInterface.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogDeliveryCompanion,Log,All);

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
    else BindTaskCompletion();
    UpdateHomeSeatPresentation();
}
void UDeliveryCompanionComponent::BindTaskCompletion()
{
    if (BoundTaskManager.IsValid()) return;
    if (auto* Manager=UDeliveryTaskManagerComponent::Get(this))
    {
        BoundTaskManager=Manager;
        Manager->OnTaskCompleted.AddUniqueDynamic(this,&UDeliveryCompanionComponent::OnTaskCompleted);
    }
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
    UDeliveryTaskDefinition* ExpectedTask,ADeliveryPackageItem* ExpectedPackage,EDeliveryCompanionTravelMode RequestedTravelMode)
{
    if (!GetOwner()->HasAuthority() || bChangingHomeArrangement || ExpectedRevision!=CommandRevision || !DeliveryNpc::IsCompanionCommand(Action)) return false;
    // 家居必须携带当次真实候选，不能把空 HomeAction 当成通用等候命令
    if (Action==EDeliveryNpcAction::HomeAction || DeliveryNpc::IsCompanionItemErrand(Action)) return false;
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
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<APlayerState> WeakRequester(Requester);
    int32 WorkingRevision=ExpectedRevision;
    const auto StillCurrent=[&]()
    {
        return WeakThis.IsValid() && IsValid(GetOwner()) && !GetOwner()->IsActorBeingDestroyed() &&
            WeakRequester.IsValid() && CanCommand(WeakRequester.Get()) && CommandRevision==WorkingRevision;
    };
    if (DeliveryNpc::IsCompanionErrand(Action))
    {
        const bool bStarted=StartErrand(Action,Requester,ExpectedRevision,ExpectedTask,ExpectedPackage,RequestedTravelMode);
        if (bStarted)
        {
            if (!WeakThis.IsValid() || !WeakRequester.IsValid() || !CanCommand(WeakRequester.Get())) return false;
            WorkingRevision=CommandRevision;
            CancelAssist(TEXT("OwnerChangedArrangement"));
            if (!StillCurrent()) return false;
            bCaterpillarOffered=false; CaterpillarOpportunityId.Invalidate(); bAnnouncedCaterpillarOpportunity=true;
        }
        return bStarted;
    }
    // 新模式先记入，仍在座位上时由持续分支完成真实离座后再开始移动
    const bool bKeepTakenProp=(Action==EDeliveryNpcAction::Follow || Action==EDeliveryNpcAction::Stay) &&
        !HasActiveHomeArrangement() && OwnsHomeSource(HomeTakeClaim.Object.Get());
    if (!bKeepTakenProp) CancelHomeArrangement(TEXT("OwnerChangedArrangement"));
    if (!StillCurrent()) return false;
    bCaterpillarOffered=false;
    CaterpillarOpportunityId.Invalidate();
    bAnnouncedCaterpillarOpportunity=true;
    CancelAssist(TEXT("OwnerChangedArrangement"));
    if (!StillCurrent()) return false;
    if (HasActiveErrand())
    {
        FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("OwnerChangedArrangement"));
        // 只接受这次结束旧委托增加的一次修订，回调里的后来安排优先
        ++WorkingRevision;
        if (!StillCurrent() || HasActiveErrand()) return false;
    }
    StopFollowMovement();
    if (!StillCurrent() || (Action==EDeliveryNpcAction::GoHome && !CanGoHome(WeakRequester.Get()))) return false;
    // 等候时停止跟车，召回时恢复主人上次明确指定的跟随方式
    if (Action==EDeliveryNpcAction::Follow && RequestedTravelMode!=EDeliveryCompanionTravelMode::Unspecified)
        bRideWhileFollowing=RequestedTravelMode==EDeliveryCompanionTravelMode::OwnVehicle;
    Mode=Action==EDeliveryNpcAction::Follow?EDeliveryCompanionMode::Follow:
        Action==EDeliveryNpcAction::GoHome?EDeliveryCompanionMode::GoHome:EDeliveryCompanionMode::Stay;
    WaitLocation=GetCompanion()->GetNavAgentLocation();
    bCaptureWaitLocationOnFoot=Mode==EDeliveryCompanionMode::Stay && IsUsingErrandVehicle();
    ++CommandRevision;
    bHasFollowDestination=false;
    ApplyTravelInstruction(RequestedTravelMode);
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
    CopyCompanionMemories(Request.CompanionMemories);
    Request.bRequesterIsCompanionOwner=CanCommand(Requester);
    Request.CompanionCommandRevision=CommandRevision;
    Request.CompanionMode=StaticEnum<EDeliveryCompanionMode>()->GetNameStringByValue(static_cast<int64>(Mode));
    Request.CompanionSuspensionReason=SuspensionReason.IsNone()?FString():SuspensionReason.ToString();
    Request.CompanionTask=GetCommandTask();
    Request.CompanionPackage=GetCommandPackage();
    Request.bCompanionHasPackage=GetCarriedTaskPackage()!=nullptr;
    Request.CompanionErrandAction=DeliveryNpc::ActionName(Errand.Action);
    Request.CompanionErrandPhase=StaticEnum<EDeliveryCompanionErrandPhase>()->GetNameStringByValue(static_cast<int64>(Errand.Phase));
    Request.CompanionErrandStatus=StaticEnum<EDeliveryNpcActionStatus>()->GetNameStringByValue(static_cast<int64>(Errand.Status));
    // 暂停和结束是不同状态，模型需要看到当前真实卡点以及运行中的原委托
    const FName ErrandReason=HasActiveErrand() && !SuspensionReason.IsNone()?SuspensionReason:Errand.Reason;
    Request.CompanionErrandReason=ErrandReason.IsNone()?FString():ErrandReason.ToString();
    if (const auto* Task=GetCommandTask())
    {
        Request.CompanionTaskId=Task->TaskId.ToString();
        Request.CompanionTaskTitle=Task->DisplayName.ToString().Left(128);
        // 由持件接口核对主人手中或已登记车辆的实物，不从共享订单进度猜测
        const auto* OwnerCarrier=Cast<IDeliveryPackageCarrierInterface>(GetOwnerPackageCarrier());
        const auto* Package=OwnerCarrier?OwnerCarrier->GetCarriedTaskPackage():nullptr;
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
    if (!GetOwner() || !GetOwner()->HasAuthority()) return;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(GetCompanion());
    const TWeakObjectPtr<APlayerState> ExpectedOwner(OwnerPlayerState);
    const int32 Revision=CommandRevision;
    const auto StillCurrent=[&]()
    {
        return WeakThis.IsValid() && WeakNpc.IsValid() && GetCompanion()==WeakNpc.Get() &&
            CommandRevision==Revision && TWeakObjectPtr<APlayerState>(OwnerPlayerState).HasSameIndexAndSerialNumber(ExpectedOwner);
    };
    // 取消移动可能同步触发新安排，后续清理只能继续处理同一版命令
    StopHomeMovement();
    if (!StillCurrent()) return;
    StopVehicleTravel(EDeliveryCompanionTravelPurpose::Follow);
    if (!StillCurrent()) return;
    StopVehicleTravel(EDeliveryCompanionTravelPurpose::ReturnToStay);
    if (!StillCurrent()) return;
    StopVehicleTravel(EDeliveryCompanionTravelPurpose::GoHome);
    if (!StillCurrent()) return;
    CancelCatchUp();
    if (!StillCurrent() || !bOwnsNavigation) return;
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
    UE_LOG(LogDeliveryCompanion,Log,TEXT("CompanionSuspension npc=%s revision=%d mode=%d reason=%s"),
        *GetNameSafe(GetOwner()),CommandRevision,int32(Mode),*Reason.ToString());
    GetOwner()->ForceNetUpdate();
}
void UDeliveryCompanionComponent::ReleaseOwner()
{
    ShutdownItemExecution();
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
    TravelMode=EDeliveryCompanionTravelMode::Unspecified;
    bRideWhileFollowing=false;
    bCaptureWaitLocationOnFoot=false;
    ++CommandRevision; bHasFollowDestination=false; bReportedBlocked=false;
    SetSuspension(NAME_None);
    GetCompanion()->ResetForNewOwner();
    GetOwner()->ForceNetUpdate();
}
void UDeliveryCompanionComponent::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn)
{
    Super::TickComponent(Dt,Type,Fn);
    UpdateHomeSeatPresentation();
    if (!GetOwner()->HasAuthority()) return;
    // GameState 可能晚于二狗开始运行，服务器重试绑定直到任务管理器可用
    BindTaskCompletion();
    if (Mode!=EDeliveryCompanionMode::Unrecruited && (!IsValid(OwnerPlayerState) || OwnerPlayerState->IsInactive() ||
        OwnerPlayerState->GetWorld()!=GetWorld() || !IsValid(OwnerPlayerState->GetPlayerController()))) ReleaseOwner();
    RefreshOwnerCombatSubscription();
    RefreshItemObservations();
    TickHomeLifecycle();
    RefreshPublicPlaceMemories();
    UpdateAssist();
    UpdateCaterpillarOpportunity();
    if (IsValid(OwnerPlayerState))
    {
        const auto* Pawn=GetOwnerPawn();
        if (GetCompanion()->IsKnockedDown()) SetSuspension(TEXT("Down"));
        else if (GetCompanion()->Agent->ShouldPauseForConversation() || (GetCompanion()->Agent->HasActiveAction() && !GetCompanion()->Agent->CanContinueCompanionMovement())) SetSuspension(TEXT("OtherBehavior"));
        // 控制权切换期间保留归属，恢复 Pawn 后按原出行安排继续跟随
        else if (!HasActiveErrand() && !HasActiveHomeArrangement() && Mode==EDeliveryCompanionMode::Follow && !Pawn) SetSuspension(TEXT("OwnerChangingPawn"));
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
    // Follow 的位置只围绕主人，订单入口只作为避让限制
    if (Mode!=EDeliveryCompanionMode::Follow) return false;
    auto* Npc=GetCompanion();
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!Npc || !Nav || !Pawn) return false;
    const auto& Config=GetTuning();
    const FVector OwnerPosition=Pawn->GetActorLocation();
    const FVector Velocity=Pawn->GetVelocity();
    FVector Forward=Velocity.SizeSquared2D()>=FMath::Square(30.f)?
        Velocity.GetSafeNormal2D():Pawn->GetActorForwardVector().GetSafeNormal2D();
    if (Forward.IsNearlyZero()) Forward=FVector::ForwardVector;
    const FVector Right=FVector::CrossProduct(FVector::UpVector,Forward).GetSafeNormal();
    const float FollowDistance=FMath::Max(Config.LeadDistance,Config.YieldDistance+25.f);
    const FVector Behind=OwnerPosition-Forward*FollowDistance;
    const float Side=FVector::DotProduct(Npc->GetActorLocation()-OwnerPosition,Right)>=0.f?1.f:-1.f;
    TArray<FVector> Candidates;
    // 优先留在当前一侧，窄路改为后方，入口被挡时才选择主人侧面
    for (float Scale:{1.f,.5f,-1.f,-.5f}) Candidates.Add(Behind+Right*Config.SideDistance*Side*Scale);
    Candidates.Add(Behind);
    Candidates.Add(OwnerPosition+Right*FollowDistance*Side);
    Candidates.Add(OwnerPosition-Right*FollowDistance*Side);
    for (const FVector& Desired:Candidates)
    {
        FNavLocation Projected;
        if (!Nav->ProjectPointToNavigation(Desired,Projected,FVector(80,80,250),&Npc->GetNavAgentPropertiesRef()) ||
            !IsDestinationClear(Projected.Location) ||
            FVector::Dist2D(Projected.Location,OwnerPosition)<Config.YieldDistance ||
            !IsOutsideTaskEntrance(Projected.Location)) continue;
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
    else if (Npc->Agent->ShouldPauseForConversation() || (Npc->Agent->HasActiveAction() && !Npc->Agent->CanContinueCompanionMovement())) Block=TEXT("OtherBehavior");
    if (!Block.IsNone() || !AI)
    { StopFollowMovement(); bHasFollowDestination=false; SetSuspension(Block); return; }
    const int32 Revision=CommandRevision;
    const auto ExpectedMode=Mode;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const TWeakObjectPtr<AAIController> WeakAI(AI);
    const TWeakObjectPtr<APawn> ExpectedPawn(Pawn);
    const auto IsCurrent=[&]()
    {
        return WeakThis.IsValid() && WeakNpc.IsValid() && !Npc->IsActorBeingDestroyed() &&
            WeakAI.IsValid() && Npc->GetController()==AI && CommandRevision==Revision && Mode==ExpectedMode &&
            TWeakObjectPtr<APawn>(GetOwnerPawn()).HasSameIndexAndSerialNumber(ExpectedPawn);
    };
    // 远处同行与返程沿真实路径取回自己的车，不用隐藏补位跳过旅程
    CancelCatchUp();
    // 装饰物保持由步行的真实手前目标搬运，不随取车分支附到车辆
    if (HomeState.CarriedProp && !EnsureOnFoot()) { SetSuspension(TEXT("CompanionDismountBlocked")); return; }
    if (IsHomePropLagging()) { StopFollowMovement(); bHasFollowDestination=false; SetSuspension(TEXT("HomePropBlocked")); return; }
    if (!HomeState.CarriedProp && Mode==EDeliveryCompanionMode::Follow &&
        !UpdateVehicleTravel(Pawn->GetActorLocation(),GetTuning().YieldDistance,EDeliveryCompanionTravelPurpose::Follow)) return;
    if (!IsCurrent()) return;
    const double Now=GetWorld()->GetTimeSeconds();
    if (Mode==EDeliveryCompanionMode::Stay)
    {
        if (bCaptureWaitLocationOnFoot)
        {
            const bool bOnFoot=EnsureOnFoot();
            if (!IsCurrent()) return;
            if (!bOnFoot) { SetSuspension(TEXT("CompanionDismountBlocked")); return; }
            WaitLocation=Npc->GetNavAgentLocation();
            bCaptureWaitLocationOnFoot=false;
        }
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
            {
                StopFollowMovement();
                if (!IsCurrent()) return;
                SetSuspension(TEXT("WaitReturnBlocked"));
                NextNavigationAt=Now+GetTuning().BlockedRetrySeconds;
                return;
            }
            FAIMoveRequest Return(WaitPoint.Location);
            Return.SetAcceptanceRadius(40); Return.SetAllowPartialPath(false);
            Return.SetReachTestIncludesAgentRadius(false); Return.SetReachTestIncludesGoalRadius(false);
            auto* Following=AI->GetPathFollowingComponent();
            if (Following && Following->HasReached(Return))
            {
                StopFollowMovement();
                if (!IsCurrent()) return;
                ReleaseErrandVehicle();
                if (IsCurrent()) SetSuspension(NAME_None);
                return;
            }
            if (bOwnsNavigation && Following && Following->GetCurrentRequestId()==FollowMoveId && AI->GetMoveStatus()!=EPathFollowingStatus::Idle) return;
            const auto Result=AI->MoveTo(Return);
            if (!IsCurrent()) return;
            bOwnsNavigation=Result.Code==EPathFollowingRequestResult::RequestSuccessful;
            FollowMoveId=bOwnsNavigation?Result.MoveId:FAIRequestID::InvalidRequest;
            SetSuspension(Result.Code==EPathFollowingRequestResult::Failed?FName(TEXT("WaitReturnBlocked")):NAME_None);
            if (Result.Code==EPathFollowingRequestResult::Failed) NextNavigationAt=Now+GetTuning().BlockedRetrySeconds;
            return;
        }
        if (VehicleTravelPurpose==EDeliveryCompanionTravelPurpose::ReturnToStay)
        {
            StopFollowMovement();
            if (!IsCurrent()) return;
            ReleaseErrandVehicle();
            if (!IsCurrent()) return;
        }
        const bool bOnFoot=EnsureOnFoot();
        if (!IsCurrent()) return;
        if (!bOnFoot) { SetSuspension(TEXT("CompanionDismountBlocked")); return; }
    }
    if (Now<NextNavigationAt) return;
    NextNavigationAt=Now+GetTuning().NavigationInterval;
    FVector Destination;
    const bool bTooClose=Pawn && FVector::DistSquared2D(Pawn->GetActorLocation(),Npc->GetActorLocation())<FMath::Square(GetTuning().YieldDistance);
    const bool bBlocksEntrance=!IsOutsideTaskEntrance(Npc->GetNavAgentLocation());
    const float FollowDistance=FMath::Max(GetTuning().LeadDistance,GetTuning().YieldDistance+25.f);
    const float NearbyDistance=FVector2D(FollowDistance,GetTuning().SideDistance).Size()+25.f;
    // 主人已停下且身边有空间时就地陪伴，避免为了固定队形绕到另一侧
    if (Mode==EDeliveryCompanionMode::Follow && Pawn && !bTooClose && !bBlocksEntrance &&
        Pawn->GetVelocity().SizeSquared2D()<FMath::Square(30.f) &&
        FVector::DistSquared2D(Pawn->GetActorLocation(),Npc->GetActorLocation())<=FMath::Square(NearbyDistance) &&
        FMath::Abs(Pawn->GetActorLocation().Z-Npc->GetActorLocation().Z)<=180.f &&
        IsDestinationClear(Npc->GetNavAgentLocation()) && AI->LineOfSightTo(Pawn))
    {
        StopFollowMovement();
        if (!IsCurrent()) return;
        bHasFollowDestination=false;
        bReportedBlocked=false;
        SetSuspension(NAME_None);
        return;
    }
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
        if (!IsCurrent()) return;
        bReportedBlocked=false;
        SetSuspension(NAME_None);
        return;
    }
    if (!bFound && Mode==EDeliveryCompanionMode::Follow && bHasFollowDestination &&
        FVector::DistSquared(Pawn->GetActorLocation(),LastOwnerPosition)<FMath::Square(GetTuning().RepathDistance) &&
        FMath::Abs(Pawn->GetActorLocation().Z-LastOwnerPosition.Z)<=75.f)
    {
        const auto* Path=AI->GetPathFollowingComponent();
        const bool bStillMoving=bOwnsNavigation && Path && Path->GetCurrentRequestId()==FollowMoveId && AI->GetMoveStatus()!=EPathFollowingStatus::Idle;
        const bool bAtLastDestination=FVector::Dist(Npc->GetNavAgentLocation(),LastDestination)<65 &&
            IsDestinationClear(LastDestination) && AI->LineOfSightTo(Pawn);
        if (bStillMoving || bAtLastDestination) return;
        bHasFollowDestination=false;
    }
    if (!bFound && Mode==EDeliveryCompanionMode::Follow) bFound=FindFollowDestination(Pawn,Destination);
    if (!bFound)
    {
        const bool bWaiting=Mode==EDeliveryCompanionMode::Stay;
        StopFollowMovement();
        if (!IsCurrent()) return;
        SetSuspension(bWaiting?TEXT("WaitSpaceBlocked"):TEXT("PathBlocked"));
        bHasFollowDestination=false;
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
    const float GoalChangeTolerance=Mode==EDeliveryCompanionMode::Follow?40.f:60.f;
    const auto* Path=AI->GetPathFollowingComponent();
    if (bOwnsNavigation && Path && Path->GetCurrentRequestId()==FollowMoveId &&
        FVector::DistSquared(Destination,LastDestination)<FMath::Square(GoalChangeTolerance) && AI->GetMoveStatus()!=EPathFollowingStatus::Idle) return;
    FAIMoveRequest Move(Destination);
    Move.SetAcceptanceRadius(40); Move.SetAllowPartialPath(false);
    Move.SetReachTestIncludesAgentRadius(false); Move.SetReachTestIncludesGoalRadius(false);
    const auto Result=AI->MoveTo(Move);
    if (!IsCurrent()) return;
    UE_LOG(LogDeliveryCompanion,Log,TEXT("CompanionFollowMove npc=%s revision=%d result=%d move=%u from=%s goal=%s owner=%s travel=%s"),
        *GetNameSafe(Npc),CommandRevision,int32(Result.Code),Result.MoveId.GetID(),
        *Npc->GetNavAgentLocation().ToCompactString(),*Destination.ToCompactString(),
        Pawn?*Pawn->GetActorLocation().ToCompactString():TEXT("none"),
        *StaticEnum<EDeliveryCompanionTravelMode>()->GetNameStringByValue(int64(TravelMode)));
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
    const auto* Manager=UDeliveryTaskManagerComponent::Get(this);
    const bool bDeliveredByCompanion=Manager && Manager->GetCompletingPackageCarrier(Task)==GetOwner();
    // 记忆与即时陪伴事实共用实际交付、三维距离和无遮挡检查，也共用订单去重
    if (!RecordDeliveryMemory(Task,Deliverer,bDeliveredByCompanion) || bDeliveredByCompanion || !Pawn) return;
    GetCompanion()->ObserveFact(TEXT("OwnerDeliveryCompleted"),Pawn,
        FString::Printf(TEXT("主人刚完成了订单 %s，我在附近陪着；包裹交付和奖励已经由游戏确认，我没有替他交件。"),*Task->TaskId.ToString()),EDeliveryNpcEvidenceSource::ActionResult);
}
void UDeliveryCompanionComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    Memories.RemoveAll([](const FDeliveryNpcCompanionMemory& Memory)
        { return Memory.Kind==TEXT("PublicPlaceSeen"); });
    RememberedPublicPlaces.Reset();
    NextPublicPlaceObservationAt=0;
    ShutdownItemExecution();
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
    DOREPLIFETIME(UDeliveryCompanionComponent,TravelMode);
    DOREPLIFETIME(UDeliveryCompanionComponent,HomeState);
}
