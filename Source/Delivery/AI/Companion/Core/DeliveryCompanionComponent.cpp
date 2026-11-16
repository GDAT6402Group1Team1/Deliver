#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionTuning.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Characters/DeliveryNpcAIController.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Conversation/DeliveryNpcConversationTrace.h"
#include "Character/DeliveryCharacter.h"
#include "Carry/DeliveryPackageCarrierInterface.h"
#include "NPC/Grandpa/DeliveryGrandpa.h"
#include "Vehicle/Companion/DeliveryCompanionMotorbike.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "World/Home/DeliveryHome.h"
#include "Task/Tracking/DeliveryTaskTrackerComponent.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/World/DeliveryBreakableDoor.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Inventory/Items/DeliveryPackageItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "AIController.h"
#include "NavLinkCustomComponent.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "Navigation/PathFollowingComponent.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeliveryCompanion,Log,All);

namespace
{
    constexpr float FollowMoveAcceptanceRadius=40.f;
    constexpr float FollowRejectedStandRadius=70.f;
    const FName FollowWaitingAtGrandpaDoor(TEXT("FollowWaitingAtGrandpaDoor"));
    const FName FollowHitReaction(TEXT("HitReaction"));
    const FName FollowOwnerChangingPawn(TEXT("OwnerChangingPawn"));
    const FName CompanionTaskDelivered(TEXT("CompanionTaskDelivered"));

    UPrimitiveComponent* FindFollowSegmentBlocker(const ADeliveryCompanionCharacter& Npc,const FVector& From,const FVector& To)
    {
        const auto* Capsule=Npc.GetCapsuleComponent();
        const float HalfHeight=Capsule->GetScaledCapsuleHalfHeight();
        const FVector Lift(0,0,HalfHeight+4.f);
        const auto Shape=FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(),HalfHeight);
        FCollisionObjectQueryParams Objects;
        Objects.AddObjectTypesToQuery(ECC_Pawn);
        Objects.AddObjectTypesToQuery(ECC_Vehicle);
        const FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionFollowRecovery),false,&Npc);
        TArray<FHitResult> Hits;
        Npc.GetWorld()->SweepMultiByObjectType(Hits,From+Lift,To+Lift,FQuat::Identity,Objects,Shape,Query);
        for (const auto& Hit:Hits)
        {
            auto* Obstacle=Hit.GetComponent();
            // 忽略擦着起点且正在远离的接触，只返回实际会阻挡角色的行人与车辆
            if (Obstacle && Obstacle->GetCollisionResponseToChannel(Capsule->GetCollisionObjectType())==ECR_Block &&
                Capsule->GetCollisionResponseToChannel(Obstacle->GetCollisionObjectType())==ECR_Block &&
                (!Hit.bStartPenetrating || FVector::DotProduct(To-From,Hit.Normal)<0.)) return Obstacle;
        }
        return nullptr;
    }

    bool IsFollowPathStartClear(const ADeliveryCompanionCharacter& Npc,const UNavigationPath& Path)
    {
        constexpr double MaxDistance=160.;
        constexpr int32 MaxSegments=2;
        double Remaining=MaxDistance;
        FVector From=Npc.GetNavAgentLocation();
        for (int32 Index=1;Index<Path.PathPoints.Num() && Index<=MaxSegments && Remaining>0.;++Index)
        {
            const FVector Delta=Path.PathPoints[Index]-From;
            const double Length=Delta.Size();
            if (Length<=UE_KINDA_SMALL_NUMBER) continue;
            const double Distance=FMath::Min(Remaining,Length);
            const FVector To=From+Delta*(Distance/Length);
            if (FindFollowSegmentBlocker(Npc,From,To)) return false;
            Remaining-=Distance;
            From=To;
        }
        return true;
    }

    struct FFollowFormation
    {
        FVector Forward;
        float Distance;
        float SideDistance;
        bool bPrefersRear;

        bool IsNearby(const FVector& FromOwner) const
        {
            const float NearbyDistance=FVector2f(Distance,SideDistance).Size()+25.f;
            // 允许在导航接受半径内自然站住，受委屈时仍须确实落到主人后侧
            const float MinimumRearDistance=Distance-FollowMoveAcceptanceRadius-10.f;
            return FromOwner.SizeSquared2D()<=FMath::Square(NearbyDistance) &&
                (!bPrefersRear || -FVector::DotProduct(FromOwner,Forward)>=MinimumRearDistance);
        }
    };

    FFollowFormation MakeFollowFormation(const APawn* Pawn,const UDeliveryCompanionTuning& Config,int32 Affinity)
    {
        FVector Forward=FVector::ForwardVector;
        if (Pawn)
        {
            const FVector Velocity=Pawn->GetVelocity();
            Forward=Velocity.SizeSquared2D()>=FMath::Square(30.f)?Velocity.GetSafeNormal2D():Pawn->GetActorForwardVector().GetSafeNormal2D();
            if (Forward.IsNearlyZero()) Forward=FVector::ForwardVector;
        }
        // 受委屈到寒心之间逐步拉开，关系变暖时沿原路线间隔收回
        const float Withdrawal=FMath::Clamp((50.f-Affinity)/30.f,0.f,1.f);
        const float Distance=FMath::Max(Config.LeadDistance,Config.YieldDistance+25.f)+
            Config.WithdrawnFollowExtraDistance*Withdrawal;
        return {Forward,Distance,Config.SideDistance*FMath::Lerp(1.f,.65f,Withdrawal),Withdrawal>0.f};
    }

    struct FFollowDoorway
    {
        FVector DoorCenter;
        FVector TowardOwnerSide;
        FVector StreetAnchor;
        FVector TowardStreet;
        double MinFloorZ;
        double MaxFloorZ;
        bool bOpen;
        bool bOwnerAtStreetEntrance;
        bool bPathCrossesIntactDoor;

        bool IsOnOwnerSide(const FVector& Point,double Clearance) const
        {
            return FMath::IsWithinInclusive(Point.Z,MinFloorZ,MaxFloorZ) &&
                FVector::DotProduct(Point-DoorCenter,TowardOwnerSide)>=Clearance;
        }

        bool IsOnStreetSide(const FVector& Point,double Clearance) const
        {
            return !TowardStreet.IsNearlyZero() && FMath::IsWithinInclusive(Point.Z,MinFloorZ,MaxFloorZ) &&
                FVector::DotProduct(Point-DoorCenter,TowardStreet)>=Clearance;
        }

        bool IsClosedAhead(const FVector& FromFeet,double Clearance) const
        {
            return bPathCrossesIntactDoor && IsOnStreetSide(FromFeet,Clearance) &&
                FVector::DotProduct(TowardOwnerSide,TowardStreet)<0.;
        }
    };

    TOptional<FFollowDoorway> FindFollowDoorway(const APawn* Pawn,const ADeliveryCompanionCharacter* Companion,
        const FVector& FromFeet)
    {
        const auto* Player=Cast<ADeliveryCharacter>(Pawn);
        if (!Player) return {};
        const auto* Home=ADeliveryHome::FindSharedHome(Companion);
        const auto* Link=IsValid(Home)?Home->DoorwayLink.Get():nullptr;
        const auto* Grandpa=IsValid(Home)?Home->FormerResident.Get():nullptr;
        const auto* Door=IsValid(Grandpa)?Grandpa->FrontDoor.Get():nullptr;
        if (!IsValid(Link) || !IsValid(Door) || Door->GetWorld()!=Companion->GetWorld()) return {};
        const bool bOpen=Door->IsBroken();
        if (bOpen && !Link->IsEnabled()) return {};

        const FVector DoorCenter=Door->GetActorLocation();
        const FVector Start=Link->GetStartPoint();
        const FVector End=Link->GetEndPoint();
        if (DoorCenter.ContainsNaN() || Start.ContainsNaN() || End.ContainsNaN()) return {};
        const FVector Axis=(End-Start).GetSafeNormal2D();
        const double DoorAlongLink=FVector::DotProduct(DoorCenter-Start,Axis);
        if (DoorAlongLink<=0. || DoorAlongLink>=FVector::Dist2D(Start,End) ||
            FVector::DistSquared2D(DoorCenter,Start+Axis*DoorAlongLink)>FMath::Square(150.)) return {};

        // 只在房门附近的同一层区分两侧，门平面用实际门体，楼层用导航连接的两端
        constexpr double DoorwayRange=500.;
        constexpr double FloorTolerance=75.;
        const double MinFloorZ=FMath::Min(Start.Z,End.Z)-FloorTolerance;
        const double MaxFloorZ=FMath::Max(Start.Z,End.Z)+FloorTolerance;
        const auto IsNearDoorway=[&](const FVector& Feet,double Range)
        {
            return FMath::IsWithinInclusive(Feet.Z,MinFloorZ,MaxFloorZ) &&
                FVector::DistSquared2D(Feet,DoorCenter)<=FMath::Square(Range);
        };
        const auto* PlayerCapsule=Player->GetCapsuleComponent();
        const FVector PlayerFeet=Player->GetActorLocation()-FVector(0,0,PlayerCapsule->GetScaledCapsuleHalfHeight());
        if (!IsNearDoorway(PlayerFeet,DoorwayRange)) return {};
        const double PlayerSide=FVector::DotProduct(PlayerFeet-DoorCenter,Axis);
        if (FMath::Abs(PlayerSide)<PlayerCapsule->GetScaledCapsuleRadius()) return {};
        const FVector StreetAnchor=IsValid(Home->ReturnAnchor)?Home->ReturnAnchor->GetComponentLocation():FVector::ZeroVector;
        FVector TowardStreet=FVector::ZeroVector;
        double CompanionRange=DoorwayRange;
        if (IsValid(Home->ReturnAnchor) && IsNearDoorway(StreetAnchor,DoorwayRange))
        {
            const double AnchorSide=FVector::DotProduct(StreetAnchor-DoorCenter,Axis);
            if (FMath::Abs(AnchorSide)>20.)
            {
                TowardStreet=AnchorSide>0.?Axis:-Axis;
                // 留位后的再检测要覆盖门外候选、导航投影和到达容差，调大净空时也不会中途取消
                const auto& Config=Companion->Companion->GetTuning();
                const double WaitOffset=FVector2D(Config.DestinationClearance+60.f,Config.YieldDistance).Size();
                CompanionRange=FMath::Max(DoorwayRange,FVector::Dist2D(StreetAnchor,DoorCenter)+
                    WaitOffset+FollowMoveAcceptanceRadius+30.);
            }
        }
        if (!IsNearDoorway(FromFeet,CompanionRange)) return {};
        constexpr double StreetEntranceRange=280.;
        const bool bOwnerAtStreetEntrance=!TowardStreet.IsNearlyZero() &&
            FVector::DotProduct(PlayerFeet-DoorCenter,TowardStreet)>=PlayerCapsule->GetScaledCapsuleRadius() &&
            FVector::DistSquared2D(PlayerFeet,DoorCenter)<=FMath::Square(StreetEntranceRange);
        const bool bPathCrossesIntactDoor=!bOpen && Home->CrossesPedestrianDoorway(FromFeet,PlayerFeet);
        return FFollowDoorway{DoorCenter,PlayerSide>0.?Axis:-Axis,StreetAnchor,TowardStreet,MinFloorZ,MaxFloorZ,
            bOpen,bOwnerAtStreetEntrance,bPathCrossesIntactDoor};
    }
}

// ── 主人与安排 ──
// UDeliveryCompanionComponent 在服务器上保存主人、模式（跟随、等候、回家）和持续安排；模型选的命令由 ApplyCommand 接收，跟随由 UpdateFollow 推进

/// @brief 组件要复制，服务器每 0.25 秒更新一次
UDeliveryCompanionComponent::UDeliveryCompanionComponent()
{
    SetIsReplicatedByDefault(true);
    PrimaryComponentTick.bCanEverTick=true;
    PrimaryComponentTick.TickInterval=.25f;
}
/// @brief 拥有这个组件的二狗角色
ADeliveryCompanionCharacter* UDeliveryCompanionComponent::GetCompanion() const { return Cast<ADeliveryCompanionCharacter>(GetOwner()); }
/// @brief 实例上填了 Tuning 就用它，否则用类默认的调参
const UDeliveryCompanionTuning& UDeliveryCompanionComponent::GetTuning() const
{
    return Tuning?*Tuning:*GetDefault<UDeliveryCompanionTuning>();
}
/// @brief 客户端关掉 Tick；服务器订阅任务完成事件，两端都更新座位表现
void UDeliveryCompanionComponent::BeginPlay()
{
    Super::BeginPlay();
    if (!GetOwner()->HasAuthority()) SetComponentTickEnabled(false);
    else BindTaskCompletion();
    UpdateHomeSeatPresentation();
}
/// @brief 找到任务管理器后订阅 OnTaskCompleted，已经订阅过就直接返回
void UDeliveryCompanionComponent::BindTaskCompletion()
{
    if (BoundTaskManager.IsValid()) return;
    if (auto* Manager=UDeliveryTaskManagerComponent::Get(this))
    {
        BoundTaskManager=Manager;
        Manager->OnTaskCompleted.AddUniqueDynamic(this,&UDeliveryCompanionComponent::OnTaskCompleted);
    }
}
/// @brief 二狗倒在门口等人扶、还没有主人、玩家在 240 cm 内且能直接看到二狗时可以结识
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

/// @brief 仅由服务器长按交互完成入口调用，true 表示归属已经写入
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
/// @brief Requester 就是当前主人且在同一个世界时返回 true
bool UDeliveryCompanionComponent::CanCommand(APlayerState* Requester) const
{
    return IsValid(Requester) && Requester==OwnerPlayerState && Requester->GetWorld()==GetWorld() && !Requester->IsInactive();
}

/// @brief 只用已经在地面登记的等候点计算余量
float UDeliveryCompanionComponent::GetWaitAreaRemaining() const
{
    const auto* Npc=GetCompanion();
    if (!IsValid(Npc) || !Npc->HasAuthority() || Mode!=EDeliveryCompanionMode::Stay || bCaptureWaitLocationOnFoot || bReturningToWaitLocation ||
        VehicleTravelPurpose==EDeliveryCompanionTravelPurpose::ReturnToStay || PendingHomeArrangementId.IsValid()) return 0.f;
    const float Offset=FVector::Dist2D(Npc->GetNavAgentLocation(),WaitLocation);
    return FMath::Max(0.f,GetTuning().WaitYieldRadius-Offset);
}
/// @brief 主人当前控制的 Pawn，主人离线或不在同一世界时返回空
APawn* UDeliveryCompanionComponent::GetOwnerPawn() const
{
    const auto* PC=IsValid(OwnerPlayerState)?OwnerPlayerState->GetPlayerController():nullptr;
    return PC && PC->GetWorld()==GetWorld()?PC->GetPawn():nullptr;
}

/// @brief 已经在跟随且没有其他安排时，主人再说跟着可以沿用当前跟随，不重新排安排
bool UDeliveryCompanionComponent::CanKeepFollowCommand(APlayerState* Requester,int32 ExpectedRevision,
    EDeliveryCompanionTravelMode RequestedTravelMode) const
{
    const auto* Npc=GetCompanion();
    if (!IsValid(Npc) || Npc->IsActorBeingDestroyed() || !Npc->HasAuthority() || !IsValid(Npc->Agent) || Npc->IsKnockedDown() ||
        bChangingHomeArrangement || ExpectedRevision!=CommandRevision || !CanCommand(Requester) ||
        Mode!=EDeliveryCompanionMode::Follow || RequestedTravelMode!=EDeliveryCompanionTravelMode::Unspecified ||
        HasActiveErrand() || HasActiveHomeArrangement() || PendingHomeArrangementId.IsValid()) return false;

    const auto& Action=Npc->Agent->GetPublicState();
    return !Npc->Agent->HasActiveAction() || Action.Origin!=EDeliveryNpcActionOrigin::GameplayRule ||
        Action.RuleRevision!=AssistRevision;
}

/// @brief 编号必须来自本次模型请求，true 表示安排已接收或原委托继续执行
bool UDeliveryCompanionComponent::ApplyCommand(EDeliveryNpcAction Action,APlayerState* Requester,int32 ExpectedRevision,
    UDeliveryTaskDefinition* ExpectedTask,ADeliveryPackageItem* ExpectedPackage,EDeliveryCompanionTravelMode RequestedTravelMode)
{
    if (!GetOwner()->HasAuthority() || bChangingHomeArrangement || ExpectedRevision!=CommandRevision || !DeliveryNpc::IsCompanionCommand(Action)) return false;
    // 家居必须携带当次真实候选，不能把空 HomeAction 当成通用等候命令
    if (Action==EDeliveryNpcAction::HomeAction || Action==EDeliveryNpcAction::JoinAssist ||
        Action==EDeliveryNpcAction::StopAssist || DeliveryNpc::IsCompanionItemErrand(Action)) return false;
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
    // 继续同一次跟随时保留车座、原路径与已记录的受阻原因
    if (Action==EDeliveryNpcAction::Follow && CanKeepFollowCommand(Requester,ExpectedRevision,RequestedTravelMode))
    {
        CancelAssist(TEXT("OwnerChangedArrangement"));
        return StillCurrent() && CanKeepFollowCommand(WeakRequester.Get(),ExpectedRevision,RequestedTravelMode);
    }
    if (DeliveryNpc::IsCompanionErrand(Action))
    {
        const bool bStarted=StartErrand(Action,Requester,ExpectedRevision,ExpectedTask,ExpectedPackage,RequestedTravelMode);
        if (bStarted)
        {
            if (!WeakThis.IsValid() || !WeakRequester.IsValid() || !CanCommand(WeakRequester.Get())) return false;
            if (CommandRevision==WorkingRevision) return true;
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
    const bool bFollowingOnFoot=FollowOnFootRevision==CommandRevision && VehicleFollowOwnerPawn.IsValid() &&
        VehicleFollowOwnerPawn.Get()==GetOwnerPawn();
    const bool bResumePreviousFollow=Action==EDeliveryNpcAction::Follow && Mode==EDeliveryCompanionMode::Follow &&
        RequestedTravelMode==EDeliveryCompanionTravelMode::Unspecified && (HasActiveErrand() || bFollowingOnFoot ||
            (DeferredIndoorFollow.Revision==CommandRevision && DeferredIndoorFollow.Owner.Get()==OwnerPlayerState));
    if (HasActiveErrand())
    {
        FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("OwnerChangedArrangement"));
        // 只接受这次结束旧委托增加的一次修订，回调里的后来安排优先
        ++WorkingRevision;
        if (!StillCurrent() || HasActiveErrand()) return false;
    }
    StopFollowMovement();
    if (!StillCurrent() || (Action==EDeliveryNpcAction::GoHome && !CanGoHome(WeakRequester.Get()))) return false;
    // 等候时停止跟车，召回保留跟随方式，跑腿仍由自己的安排决定
    if (Action==EDeliveryNpcAction::Follow && RequestedTravelMode!=EDeliveryCompanionTravelMode::Unspecified)
    {
        RefreshFollowOwnerTravel(GetOwnerPawn());
        FollowTravelMode=RequestedTravelMode;
        bFollowWalkAfterOwnerRide=false;
    }
    Mode=Action==EDeliveryNpcAction::Follow?EDeliveryCompanionMode::Follow:
        Action==EDeliveryNpcAction::GoHome?EDeliveryCompanionMode::GoHome:EDeliveryCompanionMode::Stay;
    WaitLocation=GetCompanion()->GetNavAgentLocation();
    bReturningToWaitLocation=false;
    bCaptureWaitLocationOnFoot=Mode==EDeliveryCompanionMode::Stay && IsUsingErrandVehicle();
    ++CommandRevision;
    DeferredIndoorFollow={};
    if (bResumePreviousFollow) ResumeIndoorFollowAfterErrand();
    bHasFollowDestination=false;
    if (Action==EDeliveryNpcAction::GoHome) ApplyTravelInstruction(RequestedTravelMode);
    NextNavigationAt=0;
    SetSuspension(NAME_None);
    GetOwner()->ForceNetUpdate();
    return true;
}
/// @brief 把主人名字、关系阶段、记忆、模式、委托、协战和可选命令写进模型请求
void UDeliveryCompanionComponent::ExtendDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const
{
    Request.bHasCompanionContext=true;
    ExtendWorldDecisionRequest(Request);
    Request.CompanionOwnerName=IsValid(OwnerPlayerState)?OwnerPlayerState->GetPlayerName().Left(64):TEXT("");
    Request.CompanionRelationshipStage=GetRelationshipStage();
    Request.bCanOfferCaterpillar=HasCaterpillarOpportunity();
    CopyCompanionMemories(Request.CompanionMemories);
    Request.bRequesterIsCompanionOwner=CanCommand(Requester);
    if (Request.bPlayerRequest && Request.bRequesterIsCompanionOwner)
    {
        // 保留最近一次签收供主人追问，历史压缩仍保留末尾事实
        const int32 Receipt=Request.Evidence.FindLastByPredicate([](const auto& Fact){ return Fact.Kind==CompanionTaskDelivered; });
        if (Receipt!=INDEX_NONE && Receipt<Request.Evidence.Num()-1)
        {
            auto Latest=MoveTemp(Request.Evidence[Receipt]);
            Request.Evidence.RemoveAt(Receipt);
            Request.Evidence.Add(MoveTemp(Latest));
        }
    }
    else
    {
        Request.Evidence.RemoveAll([&Request](const auto& Fact)
        {
            if (Fact.Kind!=CompanionTaskDelivered) return false;
            Request.LastDecisionEventIds.Remove(Fact.EventId);
            return true;
        });
    }
    Request.CompanionCommandRevision=CommandRevision;
    Request.CompanionMode=StaticEnum<EDeliveryCompanionMode>()->GetNameStringByValue(static_cast<int64>(Mode));
    Request.CompanionSuspensionReason=SuspensionReason.IsNone()?FString():SuspensionReason.ToString();
    Request.CompanionTask=GetCommandTask();
    Request.CompanionPackage=GetCommandPackage();
    Request.bCompanionHasPackage=GetCarriedTaskPackage()!=nullptr;
    Request.CompanionErrandAction=DeliveryNpc::ActionName(Errand.Action);
    // 订单跟踪可切到下一单，委托结果仍归原订单
    if (Request.bPlayerRequest && Request.bRequesterIsCompanionOwner && IsValid(Errand.Task) && !Errand.Task->TaskId.IsNone())
        Request.CompanionErrandTaskId=Errand.Task->TaskId.ToString().Left(64);
    Request.CompanionErrandPhase=StaticEnum<EDeliveryCompanionErrandPhase>()->GetNameStringByValue(static_cast<int64>(Errand.Phase));
    Request.CompanionErrandStatus=StaticEnum<EDeliveryNpcActionStatus>()->GetNameStringByValue(static_cast<int64>(Errand.Status));
    // 暂停和结束是不同状态，模型需要看到当前真实卡点以及运行中的原委托
    const FName WaitingReason=OwnerHandoffFollowRevision==CommandRevision?FName(TEXT("OwnerRiding")):Errand.Reason;
    const FName ErrandReason=HasActiveErrand() && !SuspensionReason.IsNone()?SuspensionReason:WaitingReason;
    Request.CompanionErrandReason=ErrandReason.IsNone()?FString():ErrandReason.ToString();
    if (const auto* Task=Request.CompanionTask.Get())
    {
        Request.CompanionTaskId=Task->TaskId.ToString();
        Request.CompanionTaskTitle=Task->DisplayName.ToString().Left(128);
        // 由持件接口核对主人手中或已登记车辆的实物，不从共享订单进度猜测
        const auto* OwnerCarrierPawn=GetOwnerPackageCarrier();
        const auto* OwnerCarrier=Cast<IDeliveryPackageCarrierInterface>(OwnerCarrierPawn);
        const auto* Package=OwnerCarrier?OwnerCarrier->GetCarriedTaskPackage():nullptr;
        const auto* Item=IsValid(Package)?Package->GetDeliveryItemComponent():nullptr;
        Request.bCompanionOwnerHasPackage=Item && Item->OwningTask==Task;
        if (Request.bPlayerRequest && Request.bRequesterIsCompanionOwner && Request.bCompanionOwnerHasPackage &&
            Package->GetOwner()==OwnerCarrierPawn)
        {
            // 登记车辆的实际货物与玩家手持由同一个持件接口筛出
            if (const auto* Bike=Cast<ADeliveryMotorbike>(OwnerCarrierPawn))
            {
                if (!Bike->IsTrunkBusy() && Bike->GetCargo()==Package && Package->GetAttachParentActor()==Bike)
                    Request.CompanionOwnerPackageLocation=TEXT("RegisteredBikeTrunk");
            }
            else if (const auto* Player=Cast<ADeliveryCharacter>(OwnerCarrierPawn))
            {
                const auto* Inventory=Player->GetInventoryComponent();
                if (Inventory->GetHeldItem()==Package) Request.CompanionOwnerPackageLocation=TEXT("Hand");
                else if (Inventory->IsTaskPackageInBackpack(Package)) Request.CompanionOwnerPackageLocation=TEXT("Backpack");
            }
        }
        if (Request.bPlayerRequest && Request.bRequesterIsCompanionOwner && !Request.bCompanionOwnerHasPackage)
            if (const auto* Dropped=GetOwnerDroppedPackage(); Dropped && Dropped==GetCommandPackage())
                Request.CompanionOwnerPackageLocation=TEXT("Ground");
        if (const auto* Manager=UDeliveryTaskManagerComponent::Get(this))
        {
            const EDeliveryTaskStatus TaskStatus=Manager->GetTaskStatus(Task);
            Request.CompanionTaskPhase=StaticEnum<EDeliveryTaskStatus>()->GetNameStringByValue(static_cast<int64>(TaskStatus));
            if (Request.bPlayerRequest && Request.bRequesterIsCompanionOwner && !Task->TaskId.IsNone() &&
                (TaskStatus==EDeliveryTaskStatus::AwaitingPickup || TaskStatus==EDeliveryTaskStatus::InProgress))
                Request.CompanionTaskDoorState=GetObservedTaskDoorState(Task);
        }
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
    for (const auto Action:{EDeliveryNpcAction::FetchPackage,EDeliveryNpcAction::TakePackage,EDeliveryNpcAction::DeliverPackage,
        EDeliveryNpcAction::ReturnPackage,EDeliveryNpcAction::DropPackage,EDeliveryNpcAction::StorePackageInBackpack})
        if ((Action!=EDeliveryNpcAction::StorePackageInBackpack || Request.bPlayerRequest) && CanArrangeErrand(Action,Requester))
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
/// @brief 对话或受击暂停期间，二狗正在骑专属车出行时可以继续坐在车上
bool UDeliveryCompanionComponent::CanKeepVehicleSeatWhilePaused() const
{
    const auto* Npc=GetCompanion();
    const auto* Controller=CanCommand(OwnerPlayerState)?OwnerPlayerState->GetPlayerController():nullptr;
    if (!IsValid(Npc) || !Npc->HasAuthority() || !IsValid(Cast<AAIController>(Npc->GetController())) || !Npc->Agent ||
        !Npc->Combat || Npc->IsKnockedDown() || !IsValid(Controller) || Controller->GetWorld()!=GetWorld() ||
        !IsUsingErrandVehicle() || Npc->GetAttachParentActor()!=ErrandVehicle ||
        HasActiveHomeArrangement() || PendingHomeArrangementId.IsValid()) return false;
    if (HasActiveErrand())
    {
        // 轻击只暂停当前这段确有包裹的骑程，旧车座和取消中的委托沿用原下车规则
        return HasErrandPackageInTransit() && Npc->Combat->IsReactingToHit() && VehicleTravelPurpose==EDeliveryCompanionTravelPurpose::Errand &&
            VehicleLegRevision==CommandRevision && Errand.CommandRevision==CommandRevision && VehicleLegPhase==Errand.Phase &&
            Npc->GetRootComponent()->GetAttachParent()==ErrandVehicle->CompanionSeat;
    }
    const bool bFollowing=Mode==EDeliveryCompanionMode::Follow && VehicleTravelPurpose==EDeliveryCompanionTravelPurpose::Follow;
    const bool bReturning=Mode==EDeliveryCompanionMode::Stay && VehicleTravelPurpose==EDeliveryCompanionTravelPurpose::ReturnToStay;
    if (!bFollowing && !bReturning) return false;
    return (Npc->Combat->IsReactingToHit() || (bFollowing && !GetOwnerPawn())) &&
        (!Npc->Agent->HasActiveAction() || Npc->Agent->CanContinueCompanionMovement());
}

/// @brief 停止跟随导航；bKeepVehicleSeat 为 true 且仍可坐车时留在车上，否则先下车
void UDeliveryCompanionComponent::StopFollowMovement(bool bKeepVehicleSeat)
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
    StopVehicleTravel(EDeliveryCompanionTravelPurpose::Follow,bKeepVehicleSeat);
    if (!StillCurrent()) return;
    StopVehicleTravel(EDeliveryCompanionTravelPurpose::ReturnToStay,bKeepVehicleSeat);
    if (!StillCurrent()) return;
    StopVehicleTravel(EDeliveryCompanionTravelPurpose::GoHome);
    if (!StillCurrent()) return;
    CancelCatchUp();
    if (StillCurrent()) StopFollowNavigation();
}
/// @brief 取消跟随导航的监听，只停止本组件自己发起的那次移动请求
void UDeliveryCompanionComponent::StopFollowNavigation()
{
    ClearFollowMoveWatch();
    if (!bOwnsNavigation) return;
    const FAIRequestID OldId=FollowMoveId;
    FollowMoveId=FAIRequestID::InvalidRequest;
    bOwnsNavigation=false;
    const auto* Npc=GetCompanion();
    if (const auto* AI=Npc?Cast<AAIController>(Npc->GetController()):nullptr)
        if (auto* Path=AI->GetPathFollowingComponent(); Path && OldId.IsValid() && Path->GetCurrentRequestId()==OldId)
            Path->AbortMove(*this,FPathFollowingResultFlags::UserAbort,OldId);
}
/// @brief 解绑跟随导航的完成回调并清空监听记录，可以重复调用
void UDeliveryCompanionComponent::ClearFollowMoveWatch()
{
    if (auto* Path=FollowMovePath.Get(); Path && FollowMoveFinishedHandle.IsValid())
        Path->OnRequestFinished.Remove(FollowMoveFinishedHandle);
    FollowMoveFinishedHandle.Reset();
    FollowMovePath.Reset();
    FollowMoveOwnerPawn.Reset();
    FollowMoveRevision=INDEX_NONE;
    FollowObstructionSample={};
}
/// @brief 记下这次跟随导航，订阅完成回调；路上被挡住时记下这个站位，下次不再选它
void UDeliveryCompanionComponent::WatchFollowMove(UPathFollowingComponent& Path,FAIRequestID MoveId,APawn& FollowedPawn,
    const FVector& Destination,bool bWaitingAtDoor)
{
    ClearFollowMoveWatch();
    FollowMovePath=&Path;
    FollowMoveOwnerPawn=&FollowedPawn;
    FollowMoveRevision=CommandRevision;
    const TWeakObjectPtr<UPathFollowingComponent> ExpectedPath(&Path);
    const TWeakObjectPtr<APawn> ExpectedPawn(&FollowedPawn);
    const TWeakObjectPtr<APlayerState> ExpectedOwner(OwnerPlayerState);
    const int32 Revision=CommandRevision;
    FollowMoveFinishedHandle=Path.OnRequestFinished.AddWeakLambda(this,
        [this,ExpectedPath,ExpectedPawn,ExpectedOwner,Revision,MoveId,Destination,bWaitingAtDoor](FAIRequestID FinishedId,const FPathFollowingResult& Result)
        {
            if (FinishedId!=MoveId || !bOwnsNavigation || FollowMoveId!=MoveId || FollowMovePath!=ExpectedPath) return;
            ClearFollowMoveWatch();
            bOwnsNavigation=false;
            FollowMoveId=FAIRequestID::InvalidRequest;
            if (Result.Code!=EPathFollowingResult::Success) bHasFollowDestination=false;
            const auto* Npc=GetCompanion();
            const auto* AI=Npc?Cast<AAIController>(Npc->GetController()):nullptr;
            auto* Pawn=ExpectedPawn.Get();
            if (!IsValid(Npc) || !Npc->HasAuthority() || Npc->IsActorBeingDestroyed() || Npc->IsKnockedDown() || !AI ||
                !ExpectedPath.IsValid() || AI->GetPathFollowingComponent()!=ExpectedPath.Get() || !IsValid(Pawn) ||
                !CanCommand(ExpectedOwner.Get()) || GetOwnerPawn()!=Pawn || CommandRevision!=Revision || Mode!=EDeliveryCompanionMode::Follow ||
                HasActiveErrand() || HasActiveHomeArrangement() || IsUsingErrandVehicle()) return;
            if (Result.Code==EPathFollowingResult::Success)
            {
                if (SuspensionReason==TEXT("PathBlocked")) { bReportedBlocked=false; SetSuspension(NAME_None); }
                return;
            }
            if (Result.Code!=EPathFollowingResult::Blocked) return;
            RememberFollowObstruction(*Pawn,Destination);
            // 等下一轮行为树换路，避免在路径组件的结束回调里同步发起新请求
            NextNavigationAt=0;
            SetSuspension(bWaitingAtDoor?FollowWaitingAtGrandpaDoor:FName(TEXT("PathBlocked")));
            UE_LOG(LogDeliveryCompanion,Log,TEXT("CompanionFollowPathBlocked npc=%s revision=%d move=%u from=%s goal=%s"),
                *GetNameSafe(Npc),Revision,MoveId.GetID(),*FollowBlockedFeet.ToCompactString(),*Destination.ToCompactString());
        });
}
/// @brief 跟随步行途中检查前方有没有东西挡路，返回挡路的组件，没有返回空
UPrimitiveComponent* UDeliveryCompanionComponent::SampleFollowMoveObstruction(const UPathFollowingComponent& Path,
    const APawn& FollowedPawn,double Now)
{
    const auto* Npc=GetCompanion();
    const bool bActiveWalkingFollow=Npc && Mode==EDeliveryCompanionMode::Follow && bOwnsNavigation &&
        FollowMovePath.Get()==&Path && Path.GetCurrentRequestId()==FollowMoveId && Path.GetStatus()==EPathFollowingStatus::Moving &&
        FollowMoveRevision==CommandRevision && FollowMoveOwnerPawn.Get()==&FollowedPawn && GetOwnerPawn()==&FollowedPawn &&
        !Path.IsFollowingNavLink() && !Path.GetCurrentCustomLinkOb() && Npc->GetCharacterMovement()->IsMovingOnGround();
    if (!bActiveWalkingFollow || Npc->GetVelocity().SizeSquared2D()>FMath::Square(30.f))
    {
        FollowObstructionSample={};
        return nullptr;
    }
    const FVector Feet=Npc->GetNavAgentLocation();
    const FVector Segment=Path.GetCurrentTargetLocation()-Feet;
    const double Distance=Segment.Size2D();
    // 当前段已经接近时交给原到达规则，只探测眼前可以平地步行的距离
    auto* Blocker=Distance>FollowMoveAcceptanceRadius && FMath::Abs(Segment.Z)<=60.f?
        FindFollowSegmentBlocker(*Npc,Feet,Feet+Segment*FMath::Min(1.,80./Distance)):nullptr;
    if (!IsValid(Blocker))
    {
        FollowObstructionSample={};
        return nullptr;
    }
    auto& Sample=FollowObstructionSample;
    const uint32 PathIndex=Path.GetCurrentPathIndex();
    if (Sample.Blocker.Get()!=Blocker || Sample.PathIndex!=PathIndex || FVector::DistSquared2D(Sample.Feet,Feet)>FMath::Square(22.f))
    {
        Sample={Blocker,Feet,PathIndex,Now};
        return nullptr;
    }
    return Now-Sample.Since>=.75?Blocker:nullptr;
}
/// @brief 把挡住的站位加进 RejectedFollowStands，主人走开或时间过了就整组清空
void UDeliveryCompanionComponent::RememberFollowObstruction(APawn& FollowedPawn,const FVector& Destination)
{
    if (!HasRecentFollowObstruction(FollowedPawn))
    {
        RejectedFollowStands.Reset();
        RejectedFollowOwnerPawn=&FollowedPawn;
        RejectedFollowRevision=CommandRevision;
        RejectedFollowOwnerPosition=FollowedPawn.GetActorLocation();
    }
    if (!IsRejectedFollowStand(FollowedPawn,Destination))
    {
        if (RejectedFollowStands.Num()==4) RejectedFollowStands.RemoveAt(0,EAllowShrinking::No);
        RejectedFollowStands.Add(Destination);
    }
    RejectedFollowUntil=GetWorld()->GetTimeSeconds()+2.*GetTuning().BlockedRetrySeconds;
    FollowBlockedFeet=GetCompanion()->GetNavAgentLocation();
    LastOwnerPosition=FollowedPawn.GetActorLocation();
}
/// @brief 已有被挡站位记录，并且记录时的命令编号和主人 Pawn 都和现在相同
bool UDeliveryCompanionComponent::IsSameFollowObstructionContext(const APawn& FollowedPawn) const
{
    return !RejectedFollowStands.IsEmpty() && RejectedFollowRevision==CommandRevision && RejectedFollowOwnerPawn.Get()==&FollowedPawn;
}
/// @brief 同一安排下的挡路记录还没过期，主人也还在记录时的位置附近
bool UDeliveryCompanionComponent::HasRecentFollowObstruction(const APawn& FollowedPawn) const
{
    return IsSameFollowObstructionContext(FollowedPawn) && GetWorld()->GetTimeSeconds()<RejectedFollowUntil &&
        FVector::DistSquared(FollowedPawn.GetActorLocation(),RejectedFollowOwnerPosition)<FMath::Square(GetTuning().RepathDistance);
}
/// @brief Stand 离最近一次被挡的某个站位很近时返回 true
bool UDeliveryCompanionComponent::IsRejectedFollowStand(const APawn& FollowedPawn,const FVector& Stand) const
{
    return HasRecentFollowObstruction(FollowedPawn) && RejectedFollowStands.ContainsByPredicate([&Stand](const FVector& Blocked)
        { return FVector::DistSquared(Stand,Blocked)<FMath::Square(FollowRejectedStandRadius); });
}
/// @brief 写入新的暂停原因并立即同步，原因没变就不写
void UDeliveryCompanionComponent::SetSuspension(FName Reason)
{
    if (SuspensionReason==Reason) return;
    SuspensionReason=Reason;
    UE_LOG(LogDeliveryCompanion,Log,TEXT("CompanionSuspension npc=%s revision=%d mode=%d reason=%s"),
        *GetNameSafe(GetOwner()),CommandRevision,int32(Mode),*Reason.ToString());
    GetOwner()->ForceNetUpdate();
}
/// @brief 主人离开或失效时调用：结束物品、家居、扶起、协战和委托，清空归属与记忆，回到未结识
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
    RejectedFollowStands.Reset();
    RejectedFollowOwnerPawn.Reset();
    DropCarriedItem();
    GetCompanion()->Agent->ResetContext(TEXT("CompanionOwnerLost"));
    GetCompanion()->Presentation->CancelSpeech();
    OwnerPlayerState=nullptr; Mode=EDeliveryCompanionMode::Unrecruited;
    TravelMode=EDeliveryCompanionTravelMode::Unspecified;
    FollowTravelMode=EDeliveryCompanionTravelMode::Unspecified;
    bFollowOwnerWasRiding=false;
    bFollowWalkAfterOwnerRide=false;
    bReturningToWaitLocation=false;
    bCaptureWaitLocationOnFoot=false;
    ++CommandRevision; bHasFollowDestination=false; bReportedBlocked=false;
    DeferredIndoorFollow={};
    SetSuspension(NAME_None);
    GetCompanion()->ResetForNewOwner();
    GetOwner()->ForceNetUpdate();
}
/// @brief 服务器每 0.25 秒检查主人是否还在、订阅任务事件，并更新记忆、物品观察和协战
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
    RefreshHomeObjectMemories();
    UpdateAssist();
    UpdateCaterpillarOpportunity();
    if (IsValid(OwnerPlayerState))
    {
        const auto* Pawn=GetOwnerPawn();
        RefreshFollowOwnerTravel(Pawn);
        if (GetCompanion()->IsKnockedDown()) SetSuspension(TEXT("Down"));
        else if (!HasActiveHomeArrangement() && GetCompanion()->Combat->IsReactingToHit() &&
            (HasActiveErrand()?HasErrandPackageInTransit():
                Mode==EDeliveryCompanionMode::Follow || Mode==EDeliveryCompanionMode::Stay))
            SetSuspension(FollowHitReaction);
        else if (GetCompanion()->Agent->ShouldPauseForConversation() || (GetCompanion()->Agent->HasActiveAction() && !GetCompanion()->Agent->CanContinueCompanionMovement())) SetSuspension(TEXT("OtherBehavior"));
        // 控制权切换期间保留归属，恢复 Pawn 后按原出行安排继续跟随
        else if (!HasActiveErrand() && !HasActiveHomeArrangement() && Mode==EDeliveryCompanionMode::Follow && !Pawn) SetSuspension(FollowOwnerChangingPawn);
    }
}
/// @brief 二狗的胶囊放在 Destination 时不会和别的东西重叠
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
/// @brief Destination 离主人当前任务目的地的距离不小于 DestinationClearance
bool UDeliveryCompanionComponent::IsOutsideTaskEntrance(const FVector& Destination) const
{
    const auto* Tracker=UDeliveryTaskTrackerComponent::FindTracker(OwnerPlayerState);
    FVector TaskDestination; FName LocationId;
    return !Tracker || !Tracker->GetTrackedTaskDestination(TaskDestination,LocationId) ||
        FVector::Dist2D(Destination,TaskDestination)>=GetTuning().DestinationClearance;
}
/// @brief 主人和二狗之间隔着一道开着的门时返回 true
bool UDeliveryCompanionComponent::IsAcrossOpenFollowDoorway(const APawn* Pawn,const FVector& FromFeet) const
{
    const auto* Npc=GetCompanion();
    if (!Npc) return false;
    const auto Doorway=FindFollowDoorway(Pawn,Npc,FromFeet);
    return Doorway.IsSet() && Doorway->bOpen && !Doorway->IsOnOwnerSide(FromFeet,Npc->GetCapsuleComponent()->GetScaledCapsuleRadius());
}
/// @brief 门关着且挡在二狗前面时返回 true，OutStreetAnchor 写入门外的等候点
bool UDeliveryCompanionComponent::IsAcrossClosedFollowDoorway(const APawn* Pawn,const FVector& FromFeet,FVector* OutStreetAnchor) const
{
    const auto* Npc=GetCompanion();
    const auto Doorway=Npc?FindFollowDoorway(Pawn,Npc,FromFeet):TOptional<FFollowDoorway>();
    if (!Doorway.IsSet() || !Doorway->IsClosedAhead(FromFeet,Npc->GetCapsuleComponent()->GetScaledCapsuleRadius())) return false;
    if (OutStreetAnchor) *OutStreetAnchor=Doorway->StreetAnchor;
    return true;
}
/// @brief 跑腿结束后，如果主人在屋里且要求骑车跟随，先把二狗交还步行跟随
void UDeliveryCompanionComponent::ResumeIndoorFollowAfterErrand()
{
    DeferredIndoorFollow={};
    const auto* Npc=GetCompanion();
    if (!IsValid(Npc) || !Npc->HasAuthority() || !CanCommand(OwnerPlayerState) || Mode!=EDeliveryCompanionMode::Follow ||
        FollowTravelMode!=EDeliveryCompanionTravelMode::OwnVehicle || IsUsingErrandVehicle() ||
        !IsValid(ErrandVehicle) || ErrandVehicle->IsActorBeingDestroyed() || ErrandVehicle->GetWorld()!=GetWorld() ||
        ErrandVehicle->GetReservedCompanion()!=Npc) return;
    auto* Pawn=GetOwnerPawn();
    if (!IsValid(Pawn) || Pawn->IsActorBeingDestroyed())
    {
        // 主人暂时没有控制对象时，等同一主人恢复后再检查门内外站位
        const auto* Home=ADeliveryHome::FindSharedHome(Npc);
        if (IsValid(Home) && IsValid(Home->FormerResident) && Home->FormerResident->IsDoorBroken() &&
            IsValid(Home->DoorwayLink) && Home->DoorwayLink->IsEnabled())
            DeferredIndoorFollow={TWeakObjectPtr<APlayerState>(OwnerPlayerState),CommandRevision};
        return;
    }
    auto* OwnerPawn=Cast<ADeliveryCharacter>(Pawn);
    if (!OwnerPawn) return;
    const FVector Feet=Npc->GetNavAgentLocation();
    const auto Doorway=FindFollowDoorway(OwnerPawn,Npc,Feet);
    if (!Doorway.IsSet() || !Doorway->bOpen || FVector::DotProduct(Doorway->TowardOwnerSide,Doorway->TowardStreet)>=0. ||
        !Doorway->IsOnOwnerSide(Feet,Npc->GetCapsuleComponent()->GetScaledCapsuleRadius())) return;
    const FVector BikeFeet=ErrandVehicle->GetActorLocation()-FVector(0,0,ErrandVehicle->GetSimpleCollisionHalfHeight());
    if (!Doorway->IsOnStreetSide(BikeFeet,ErrandVehicle->GetSimpleCollisionRadius()) ||
        FVector::DistSquared2D(BikeFeet,Doorway->StreetAnchor)>FMath::Square(1600.)) return;

    // 两人已在收件人屋内时继续同行，主人再次上车或明确要求骑车时重新取车
    FollowOnFootRevision=CommandRevision;
    VehicleFollowOwnerPawn=OwnerPawn;
}
/// @brief 在主人周围找一个能走到、不挡任务入口、在主人这一侧的跟随站位
bool UDeliveryCompanionComponent::FindFollowDestination(APawn* Pawn,FVector& Out,
    TFunctionRef<bool(const FVector&)> IsOnOwnerSide) const
{
    // Follow 的位置只围绕主人，订单入口只作为避让限制
    if (Mode!=EDeliveryCompanionMode::Follow) return false;
    auto* Npc=GetCompanion();
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!Npc || !Nav || !Pawn) return false;
    const auto& Config=GetTuning();
    const FVector OwnerPosition=Pawn->GetActorLocation();
    const auto Formation=MakeFollowFormation(Pawn,Config,Affinity);
    const FVector Right=FVector::CrossProduct(FVector::UpVector,Formation.Forward).GetSafeNormal();
    const float Side=FVector::DotProduct(Npc->GetActorLocation()-OwnerPosition,Right)>=0.f?1.f:-1.f;
    // 优先留在当前一侧，窄路改为后方，入口被挡时才选择主人侧面
    const FVector Behind=-Formation.Forward*Formation.Distance;
    const FVector SideOffset=Right*Formation.SideDistance*Side;
    const FVector Across=Right*Formation.Distance*Side;
    const FVector CloseOffsets[]={Behind+SideOffset,Behind+SideOffset*.5,Behind-SideOffset,
        Behind-SideOffset*.5,Behind,Across,-Across};
    const bool bRecoveringRoute=HasRecentFollowObstruction(*Pawn);
    const FVector Feet=Npc->GetNavAgentLocation();
    // 记下每类落选原因的次数，整轮都找不到站位时写进日志，方便看出是导航、门口避让还是路径不通
    int32 OffNav=0,WrongSide=0,Occupied=0,Rejected=0,TooNear=0,AtEntrance=0,NoPath=0;
    const auto TryPosition=[&](const FVector& Desired,double MinMoveDistance=0.)
    {
        FNavLocation Projected;
        if (!Nav->ProjectPointToNavigation(Desired,Projected,FVector(80,80,250),&Npc->GetNavAgentPropertiesRef())) { ++OffNav; return false; }
        if (!IsOnOwnerSide(Projected.Location)) { ++WrongSide; return false; }
        if (!IsDestinationClear(Projected.Location)) { ++Occupied; return false; }
        if (bRecoveringRoute && IsRejectedFollowStand(*Pawn,Projected.Location)) { ++Rejected; return false; }
        if (FVector::DistSquared2D(Projected.Location,Feet)<FMath::Square(MinMoveDistance) ||
            FVector::Dist2D(Projected.Location,OwnerPosition)<Config.YieldDistance) { ++TooNear; return false; }
        if (!IsOutsideTaskEntrance(Projected.Location)) { ++AtEntrance; return false; }
        auto* Path=Nav->FindPathToLocationSynchronously(GetWorld(),Feet,Projected.Location,Npc);
        if (!Path || !Path->IsValid() || Path->IsPartial() || (bRecoveringRoute && !IsFollowPathStartClear(*Npc,*Path))) { ++NoPath; return false; }
        Out=Projected.Location; return true;
    };
    for (const FVector& Offset:CloseOffsets)
        if (TryPosition(OwnerPosition+Offset)) return true;

    // 常用队形受阻后从二狗所在一侧绕主人采样，先找近处，再绕开窄路或入口
    FVector TowardNpc=(Npc->GetActorLocation()-OwnerPosition).GetSafeNormal2D();
    if (TowardNpc.IsNearlyZero()) TowardNpc=-Formation.Forward;
    const float ClearDistance=FMath::Max(Formation.Distance+100.f,Config.DestinationClearance+100.f);
    for (const float Distance:{ClearDistance,ClearDistance+120.f})
    {
        for (const float Angle:{0.f,45.f,-45.f,90.f,-90.f,135.f,-135.f,180.f})
            if (TryPosition(OwnerPosition+TowardNpc.RotateAngleAxis(Angle,FVector::UpVector)*Distance)) return true;
    }
    // 临时物体可能堵住所有去队形位置的前段，先侧移腾出重新寻路的空间
    if (bRecoveringRoute)
    {
        const double Step=FMath::Max(160.,4.*Npc->GetCapsuleComponent()->GetScaledCapsuleRadius());
        const FVector TowardOwner=-TowardNpc;
        for (const float Angle:{55.f,-55.f,90.f,-90.f})
            if (TryPosition(Feet+TowardOwner.RotateAngleAxis(Angle,FVector::UpVector)*Step,60.)) return true;
    }
    const double Now=GetWorld()->GetTimeSeconds();
    if (Now>=NextFollowBlockedLogAt)
    {
        NextFollowBlockedLogAt=Now+2.;
        UE_LOG(LogDeliveryCompanion,Log,TEXT("CompanionFollowNoStand npc=%s from=%s owner=%s off_nav=%d wrong_side=%d occupied=%d rejected=%d too_near=%d at_entrance=%d no_path=%d"),
            *GetNameSafe(Npc),*Feet.ToCompactString(),*OwnerPosition.ToCompactString(),OffNav,WrongSide,Occupied,Rejected,TooNear,AtEntrance,NoPath);
    }
    return false;
}
/// @brief 等候模式下在离主人一段距离的地方找一个站位
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
/// @brief 等候安排要求在步行状态下记录等候点时，下车后记下当前位置；不需要记录时直接返回 true
bool UDeliveryCompanionComponent::CapturePendingWaitLocation()
{
    if (!bCaptureWaitLocationOnFoot) return true;
    auto* Npc=GetCompanion();
    if (!IsValid(Npc) || !Npc->HasAuthority() || Mode!=EDeliveryCompanionMode::Stay) return false;
    const int32 Revision=CommandRevision;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const bool bOnFoot=EnsureOnFoot();
    if (!WeakThis.IsValid() || !WeakNpc.IsValid() || Npc->IsActorBeingDestroyed() || CommandRevision!=Revision ||
        Mode!=EDeliveryCompanionMode::Stay) return false;
    if (!bOnFoot || !Npc->GetCharacterMovement()->IsMovingOnGround())
    {
        SetSuspension(bOnFoot?TEXT("WaitLandingPending"):TEXT("CompanionDismountBlocked"));
        return false;
    }

    // 先落实主人上次让二狗等候的真实落脚点，再允许新委托开始移动
    WaitLocation=Npc->GetNavAgentLocation();
    bCaptureWaitLocationOnFoot=false;
    if (SuspensionReason==TEXT("WaitLandingPending") || SuspensionReason==TEXT("CompanionDismountBlocked")) SetSuspension(NAME_None);
    return true;
}

/// @brief 只允许 BT 的持续分支调用，其他分支执行时先 StopFollowMovement
void UDeliveryCompanionComponent::UpdateFollow()
{
    auto* Npc=GetCompanion();
    if (!Npc || !Npc->HasAuthority()) return;
    if (HasActiveHomeArrangement() || PendingHomeArrangementId.IsValid()) { UpdateHomeArrangement(); return; }
    if (HasActiveErrand()) { StopFollowMovement(); return; }
    if (Mode==EDeliveryCompanionMode::GoHome) { UpdateGoHome(); return; }
    UpdateFollowMovement();
}
/// @brief 跟随和等候的主体：判断骑车或步行、找站位、发出导航，找不到站位太久时调用 TryLeapToOwner
void UDeliveryCompanionComponent::UpdateFollowMovement()
{
    auto* Npc=GetCompanion();
    auto* AI=Cast<AAIController>(Npc->GetController());
    auto* Pawn=GetOwnerPawn();
    RefreshFollowOwnerTravel(Pawn);
    const int32 Revision=CommandRevision;
    const auto ExpectedMode=Mode;
    const TWeakObjectPtr<UDeliveryCompanionComponent> WeakThis(this);
    const TWeakObjectPtr<ADeliveryCompanionCharacter> WeakNpc(Npc);
    const TWeakObjectPtr<AController> ExpectedController(Npc->GetController());
    const TWeakObjectPtr<APlayerState> ExpectedOwner(OwnerPlayerState);
    const TWeakObjectPtr<APawn> ExpectedPawn(Pawn);
    const auto IsCurrent=[&]()
    {
        return WeakThis.IsValid() && WeakNpc.IsValid() && !Npc->IsActorBeingDestroyed() && (!AI || ExpectedController.IsValid()) &&
            TWeakObjectPtr<AController>(Npc->GetController()).HasSameIndexAndSerialNumber(ExpectedController) &&
            TWeakObjectPtr<APlayerState>(OwnerPlayerState).HasSameIndexAndSerialNumber(ExpectedOwner) && CommandRevision==Revision && Mode==ExpectedMode &&
            TWeakObjectPtr<APawn>(GetOwnerPawn()).HasSameIndexAndSerialNumber(ExpectedPawn);
    };
    FName Block;
    if (!CanCommand(OwnerPlayerState)) Block=TEXT("NoOwner");
    else if (Npc->IsKnockedDown()) Block=TEXT("Down");
    else if (Npc->Combat->IsReactingToHit()) Block=FollowHitReaction;
    else if (Mode==EDeliveryCompanionMode::Follow && !Pawn) Block=FollowOwnerChangingPawn;
    else if (Npc->Agent->ShouldPauseForConversation() || (Npc->Agent->HasActiveAction() && !Npc->Agent->CanContinueCompanionMovement())) Block=TEXT("OtherBehavior");
    if (!Block.IsNone() || !AI)
    {
        const bool bTemporary=Block==FollowHitReaction || Block==FollowOwnerChangingPawn;
        StopFollowMovement(bTemporary && CanKeepVehicleSeatWhilePaused());
        if (!IsCurrent()) return;
        bHasFollowDestination=false;
        if (bTemporary)
        {
            NextNavigationAt=0;
            NextVehicleNavigationAt=0;
        }
        SetSuspension(Block);
        return;
    }
    // 越障期间保留同一条已验证的路径，真正改令仍通过原取消入口结束它
    if (const auto* NpcAI=Cast<ADeliveryNpcAIController>(AI); NpcAI && NpcAI->IsTraversingObstacle(FollowMoveId)) return;
    if (SuspensionReason==FollowHitReaction || SuspensionReason==FollowOwnerChangingPawn)
    {
        // 原安排已可继续，按恢复时的主人位置重新发路，旧的堵路等待到此结束
        NextNavigationAt=0;
        NextVehicleNavigationAt=0;
        SetSuspension(NAME_None);
    }
    if (DeferredIndoorFollow.Revision!=INDEX_NONE)
    {
        if (!DeferredIndoorFollow.Owner.IsValid() || DeferredIndoorFollow.Owner.Get()!=OwnerPlayerState ||
            DeferredIndoorFollow.Revision!=Revision) DeferredIndoorFollow={};
        else ResumeIndoorFollowAfterErrand();
    }
    const TWeakObjectPtr<AAIController> WeakAI(AI);
    // 远处同行与返程沿真实路径取回自己的车，不用隐藏补位跳过旅程
    CancelCatchUp();
    // 装饰物保持由步行的真实手前目标搬运，不随取车分支附到车辆
    if (HomeState.CarriedProp && !EnsureOnFoot()) { SetSuspension(TEXT("CompanionDismountBlocked")); return; }
    if (IsHomePropLagging()) { StopFollowMovement(); bHasFollowDestination=false; SetSuspension(TEXT("HomePropBlocked")); return; }
    const double Now=GetWorld()->GetTimeSeconds();
    if (Mode==EDeliveryCompanionMode::Follow && VehicleTravelPurpose==EDeliveryCompanionTravelPurpose::Follow && IsUsingErrandVehicle())
    {
        const FVector BikeLocation=ErrandVehicle->GetActorLocation();
        const FVector BikeFeet=BikeLocation-FVector(0,0,ErrandVehicle->GetSimpleCollisionHalfHeight());
        FVector StreetAnchor;
        const bool bClosedAhead=IsAcrossClosedFollowDoorway(Pawn,BikeFeet,&StreetAnchor);
        const bool bWasWaiting=SuspensionReason==FollowWaitingAtGrandpaDoor;
        if (bClosedAhead && bWasWaiting && Now<NextVehicleNavigationAt) return;
        const bool bLeavesEntranceClear=bClosedAhead && FVector::DistSquared2D(BikeFeet,StreetAnchor)>=
            FMath::Square(GetTuning().DestinationClearance+ErrandVehicle->GetSimpleCollisionRadius());
        if (bLeavesEntranceClear && FMath::Abs(ErrandVehicle->GetCurrentSpeed())<=30.f && ErrandVehicle->CanParkAt(BikeLocation))
        {
            const bool bWaitOnFoot=IsValid(Pawn) && Pawn->IsA<ADeliveryCharacter>() &&
                (bFollowWalkAfterOwnerRide || FollowTravelMode!=EDeliveryCompanionTravelMode::OwnVehicle);
            ErrandVehicle->StopDriving();
            if (!IsCurrent()) return;
            bVehicleHasDriveGoal=false;
            bVehicleLegComplete=false;
            if (!bWaitOnFoot)
            {
                NextVehicleNavigationAt=Now+GetTuning().NavigationInterval;
                SetSuspension(FollowWaitingAtGrandpaDoor);
                return;
            }
            // 门未开时在街边实际下车，余下的门口等候交给步行跟随
            ErrandVehicle->TryDismountIfClear();
            if (!IsCurrent()) return;
        }
        if (bWasWaiting)
        {
            NextVehicleNavigationAt=0;
            SetSuspension(NAME_None);
        }
    }
    if (!HomeState.CarriedProp && Mode==EDeliveryCompanionMode::Follow &&
        !UpdateVehicleTravel(Pawn->GetActorLocation(),GetTuning().YieldDistance,EDeliveryCompanionTravelPurpose::Follow)) return;
    if (!IsCurrent()) return;
    if (Mode==EDeliveryCompanionMode::Stay)
    {
        if (!CapturePendingWaitLocation() || !IsCurrent()) return;
        const float DistanceToWait=FVector::Dist2D(Npc->GetNavAgentLocation(),WaitLocation);
        if (DistanceToWait>GetTuning().WaitYieldRadius || VehicleTravelPurpose==EDeliveryCompanionTravelPurpose::ReturnToStay)
            bReturningToWaitLocation=true;
        if (bReturningToWaitLocation)
        {
            if (!HomeState.CarriedProp && !UpdateVehicleTravel(WaitLocation,60.f,EDeliveryCompanionTravelPurpose::ReturnToStay)) return;
            if (!IsCurrent()) return;
            if (Now<NextNavigationAt) return;
            NextNavigationAt=Now+GetTuning().NavigationInterval;
            auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
            FNavLocation WaitPoint;
            if (!Nav || !Nav->ProjectPointToNavigation(WaitLocation,WaitPoint,FVector(90,90,60),&Npc->GetNavAgentPropertiesRef()) ||
                FMath::Abs(WaitPoint.Location.Z-WaitLocation.Z)>60.f || !IsDestinationClear(WaitPoint.Location))
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
            if (Following && Following->HasReached(Return) && Npc->GetCharacterMovement()->IsMovingOnGround() &&
                FMath::Abs(Npc->GetNavAgentLocation().Z-WaitLocation.Z)<=60.f)
            {
                StopFollowMovement();
                if (!IsCurrent()) return;
                ReleaseErrandVehicle();
                if (IsCurrent())
                {
                    bReturningToWaitLocation=false;
                    SetSuspension(NAME_None);
                }
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
        const bool bOnFoot=EnsureOnFoot();
        if (!IsCurrent()) return;
        if (!bOnFoot) { SetSuspension(TEXT("CompanionDismountBlocked")); return; }
    }
    const bool bOwnerMovedSincePlan=ExpectedMode==EDeliveryCompanionMode::Follow && Pawn &&
        FVector::DistSquared(Pawn->GetActorLocation(),LastOwnerPosition)>=FMath::Square(GetTuning().RepathDistance);
    const bool bRetryForMovingOwner=bOwnerMovedSincePlan &&
        (SuspensionReason==TEXT("PathBlocked") || (bHasFollowDestination && !bOwnsNavigation));
    const FVector Feet=Npc->GetNavAgentLocation();
    const double CapsuleRadius=Npc->GetCapsuleComponent()->GetScaledCapsuleRadius();
    const bool bWasWaitingAtDoor=SuspensionReason==FollowWaitingAtGrandpaDoor;
    const auto Doorway=Mode==EDeliveryCompanionMode::Follow && (Now>=NextNavigationAt || bRetryForMovingOwner || bWasWaitingAtDoor)?
        FindFollowDoorway(Pawn,Npc,Feet):TOptional<FFollowDoorway>();
    const bool bClosedAhead=Doorway.IsSet() && Doorway->IsClosedAhead(Feet,CapsuleRadius);
    const bool bCanWaitNearEntrance=Doorway.IsSet() && !Doorway->bOpen &&
        Doorway->IsOnStreetSide(Feet,CapsuleRadius) && (bClosedAhead || Doorway->bOwnerAtStreetEntrance);
    if (bWasWaitingAtDoor && !bCanWaitNearEntrance)
    {
        StopFollowNavigation();
        if (!IsCurrent()) return;
        bHasFollowDestination=false;
        NextNavigationAt=0;
        SetSuspension(NAME_None);
    }
    if (Now<NextNavigationAt && !bRetryForMovingOwner) return;
    NextNavigationAt=Now+GetTuning().NavigationInterval;
    const auto* ActiveFollowPath=AI->GetPathFollowingComponent();
    if (Pawn && Mode==EDeliveryCompanionMode::Follow && SuspensionReason==TEXT("PathBlocked") &&
        IsSameFollowObstructionContext(*Pawn) && bOwnsNavigation && ActiveFollowPath &&
        ActiveFollowPath->GetCurrentRequestId()==FollowMoveId && FVector::DistSquared2D(Feet,FollowBlockedFeet)>=FMath::Square(35.f))
    {
        bReportedBlocked=false;
        SetSuspension(NAME_None);
    }
    const bool bOpenDoorway=Doorway.IsSet() && Doorway->bOpen;
    const bool bNeedsDoorwayCrossing=bOpenDoorway && !Doorway->IsOnOwnerSide(Feet,CapsuleRadius);
    const auto IsOnOwnerSide=[&](const FVector& Point)
    {
        // 路径可在接受半径内停下，目标要留出整个胶囊通过门槛的余量
        if (bClosedAhead) return Doorway->IsOnStreetSide(Point,CapsuleRadius+FollowMoveAcceptanceRadius);
        return !bOpenDoorway || Doorway->IsOnOwnerSide(Point,CapsuleRadius+FollowMoveAcceptanceRadius);
    };
    const bool bLastDestinationUsable=bHasFollowDestination && IsOnOwnerSide(LastDestination) &&
        IsOutsideTaskEntrance(LastDestination) && IsDestinationClear(LastDestination);
    if (bHasFollowDestination && !bLastDestinationUsable)
    {
        // 站位刚被占用或成为任务入口时，解除自己发出的旧步行请求
        bHasFollowDestination=false;
        StopFollowNavigation();
        if (!IsCurrent() || AI->GetMoveStatus()!=EPathFollowingStatus::Idle) return;
    }
    FVector Destination;
    const FVector FromOwner=Pawn?Npc->GetActorLocation()-Pawn->GetActorLocation():FVector::ZeroVector;
    const bool bTooClose=Pawn && FromOwner.SizeSquared2D()<FMath::Square(GetTuning().YieldDistance);
    const bool bBlocksEntrance=!IsOutsideTaskEntrance(Feet);
    const bool bOwnerStopped=Pawn && Pawn->GetVelocity().SizeSquared2D()<FMath::Square(30.f);
    const auto Formation=MakeFollowFormation(Pawn,GetTuning(),Affinity);
    // 主人已停下且身边有空间时就地陪伴，避免为了固定队形绕到另一侧
    if (Mode==EDeliveryCompanionMode::Follow && bOwnerStopped && !bClosedAhead && !bNeedsDoorwayCrossing && !bTooClose && !bBlocksEntrance &&
        Formation.IsNearby(FromOwner) && FMath::Abs(FromOwner.Z)<=180.f &&
        IsDestinationClear(Feet) && AI->LineOfSightTo(Pawn))
    {
        StopFollowMovement();
        if (!IsCurrent()) return;
        bHasFollowDestination=false;
        bReportedBlocked=false;
        SetSuspension(NAME_None);
        return;
    }
    if (Mode==EDeliveryCompanionMode::Follow && Pawn && ActiveFollowPath && bLastDestinationUsable && !Doorway.IsSet())
    {
        if (auto* Blocker=SampleFollowMoveObstruction(*ActiveFollowPath,*Pawn,Now))
        {
            const FAIRequestID BlockedMove=FollowMoveId;
            const FVector BlockedDestination=LastDestination;
            const FString BlockerName=GetNameSafe(Blocker->GetOwner());
            bHasFollowDestination=false;
            StopFollowNavigation();
            if (!IsCurrent() || AI->GetMoveStatus()!=EPathFollowingStatus::Idle) return;
            RememberFollowObstruction(*Pawn,BlockedDestination);
            SetSuspension(TEXT("PathBlocked"));
            UE_LOG(LogDeliveryCompanion,Log,TEXT("CompanionFollowObstruction npc=%s revision=%d move=%u blocker=%s"),
                *GetNameSafe(Npc),Revision,BlockedMove.GetID(),*BlockerName);
        }
    }
    else FollowObstructionSample={};
    bool bFound=false;
    if (!bClosedAhead && (bTooClose || bBlocksEntrance))
    {
        bFound=Pawn && Npc->FindSideStepDestination(Pawn,Destination) &&
            IsOnOwnerSide(Destination) && IsDestinationClear(Destination) && IsOutsideTaskEntrance(Destination) &&
            (Mode!=EDeliveryCompanionMode::Follow || !IsRejectedFollowStand(*Pawn,Destination)) &&
            (Mode!=EDeliveryCompanionMode::Stay || FVector::Dist2D(Destination,WaitLocation)<=GetTuning().WaitYieldRadius);
        if (!bFound && Mode==EDeliveryCompanionMode::Stay) bFound=FindWaitDestination(Pawn,Destination);
    }
    if (!bFound && Mode==EDeliveryCompanionMode::Stay && !bTooClose && !bBlocksEntrance)
    {
        if (!bOwnsNavigation || AI->GetMoveStatus()==EPathFollowingStatus::Idle) StopFollowMovement();
        if (!IsCurrent()) return;
        bReportedBlocked=false;
        SetSuspension(NAME_None);
        return;
    }
    if (!bFound && !bTooClose && !bBlocksEntrance && Mode==EDeliveryCompanionMode::Follow && bHasFollowDestination && bLastDestinationUsable &&
        FVector::DistSquared(Pawn->GetActorLocation(),LastOwnerPosition)<FMath::Square(GetTuning().RepathDistance) &&
        FMath::Abs(Pawn->GetActorLocation().Z-LastOwnerPosition.Z)<=75.f)
    {
        const auto* Path=AI->GetPathFollowingComponent();
        const bool bStillMoving=bOwnsNavigation && Path && Path->GetCurrentRequestId()==FollowMoveId && AI->GetMoveStatus()!=EPathFollowingStatus::Idle;
        const bool bAtLastDestination=!bNeedsDoorwayCrossing && Formation.IsNearby(FromOwner) && FVector::Dist(Feet,LastDestination)<65 &&
            AI->LineOfSightTo(Pawn);
        if (bStillMoving || bAtLastDestination) return;
        bHasFollowDestination=false;
    }
    if (!bFound && Mode==EDeliveryCompanionMode::Follow && !bClosedAhead)
        bFound=FindFollowDestination(Pawn,Destination,IsOnOwnerSide);
    const bool bUsingStreetWait=!bFound && bCanWaitNearEntrance;
    if (bUsingStreetWait)
    {
        const auto CanStandAwayFromDoor=[&](const FVector& Point)
        {
            return Doorway->IsOnStreetSide(Point,CapsuleRadius+FollowMoveAcceptanceRadius) &&
                FVector::DistSquared2D(Point,Doorway->StreetAnchor)>=FMath::Square(GetTuning().DestinationClearance) &&
                FVector::DistSquared2D(Point,Pawn->GetActorLocation())>=FMath::Square(GetTuning().YieldDistance) &&
                IsOutsideTaskEntrance(Point) && IsDestinationClear(Point) && !IsRejectedFollowStand(*Pawn,Point);
        };
        if (!CanStandAwayFromDoor(Feet))
        {
            const FVector Away=Doorway->StreetAnchor+Doorway->TowardStreet*(GetTuning().DestinationClearance+60.f);
            const FVector Side=FVector::CrossProduct(FVector::UpVector,Doorway->TowardStreet)*GetTuning().YieldDistance;
            const FVector FirstSide=FVector::DotProduct(Feet-Away,Side)>=0.?Side:-Side;
            for (const FVector Offset:{FirstSide,-FirstSide,FVector::ZeroVector})
                if (Npc->FindNearbyWalkDestination(Away+Offset,1000.f,Destination) && CanStandAwayFromDoor(Destination))
                { bFound=true; break; }
        }
        if (!bFound)
        {
            StopFollowNavigation();
            if (!IsCurrent()) return;
            bHasFollowDestination=false;
            bReportedBlocked=false;
            SetSuspension(FollowWaitingAtGrandpaDoor);
            NextNavigationAt=Now+GetTuning().BlockedRetrySeconds;
            return;
        }
    }
    if (!bFound)
    {
        const bool bWaiting=Mode==EDeliveryCompanionMode::Stay;
        StopFollowMovement();
        if (!IsCurrent()) return;
        SetSuspension(bWaiting?TEXT("WaitSpaceBlocked"):TEXT("PathBlocked"));
        if (!bWaiting) LastOwnerPosition=Pawn->GetActorLocation();
        bHasFollowDestination=false;
        NextNavigationAt=Now+GetTuning().BlockedRetrySeconds;
        if (!bWaiting && bLeapToOwnerWhenFollowBlocked)
        {
            // 先确认不是一时采样失败，约一秒后仍没有站位就起跳，落地后回到普通跟随
            if (FollowStuckSince<0) FollowStuckSince=Now;
            // 跳了几次还是接不上路，就直接到主人身边
            if (Now-FollowStuckSince>=FollowTeleportAfterSeconds && TeleportToOwner(*Pawn))
            {
                if (!IsCurrent()) return;
                FollowStuckSince=-1; FollowBlockedSince=-1; bReportedBlocked=false;
                NextNavigationAt=Now+.5;
                SetSuspension(NAME_None);
                return;
            }
            if (FollowBlockedSince<0 || Now-FollowBlockedSince>10.) { FollowBlockedSince=Now; NextNavigationAt=Now+1.; }
            else if (Now-FollowBlockedSince>=.9 && Now>=NextFollowLeapAt)
            {
                const bool bLeapt=TryLeapToOwner(*Pawn,Now);
                if (!IsCurrent()) return;
                NextFollowLeapAt=Now+2.;
                NextNavigationAt=Now+(bLeapt?1.3:2.);
                if (bLeapt) { FollowBlockedSince=-1; bReportedBlocked=false; SetSuspension(NAME_None); return; }
            }
        }
        if (!bReportedBlocked)
        {
            bReportedBlocked=true;
            Npc->ObserveFact(bWaiting?TEXT("CompanionWaitSpaceBlocked"):TEXT("CompanionPathBlocked"),Pawn,
                bWaiting?TEXT("主人让我在这里等候，但身边暂时找不到可让开入口或来人的空位；我仍留在附近等候，没有自动追随主人。"):
                TEXT("当前跟随路线被挡住，我停在可走的位置等待；没有到达主人身边，也没有穿过障碍。"),EDeliveryNpcEvidenceSource::ActionResult);
        }
        return;
    }
    FollowBlockedSince=-1; FollowStuckSince=-1;
    const float GoalChangeTolerance=Mode==EDeliveryCompanionMode::Follow?40.f:60.f;
    const auto* Path=AI->GetPathFollowingComponent();
    if (bLastDestinationUsable && bOwnsNavigation && Path && Path->GetCurrentRequestId()==FollowMoveId &&
        FVector::DistSquared(Destination,LastDestination)<FMath::Square(GoalChangeTolerance) && AI->GetMoveStatus()!=EPathFollowingStatus::Idle) return;
    FAIMoveRequest Move(Destination);
    Move.SetAcceptanceRadius(FollowMoveAcceptanceRadius); Move.SetAllowPartialPath(false);
    Move.SetReachTestIncludesAgentRadius(false); Move.SetReachTestIncludesGoalRadius(false);
    const FAIRequestID PreviousMove=FollowMoveId;
    const auto Result=AI->MoveTo(Move);
    if (!IsCurrent())
    {
        // MoveTo 的同步通知若已改派，只取消这次调用刚建立的旧请求
        if (WeakThis.IsValid() && WeakAI.IsValid() && Result.Code==EPathFollowingRequestResult::RequestSuccessful && Result.MoveId.IsValid())
            if (auto* CurrentPath=AI->GetPathFollowingComponent(); CurrentPath && CurrentPath->GetCurrentRequestId()==Result.MoveId)
                CurrentPath->AbortMove(*this,FPathFollowingResultFlags::UserAbort,Result.MoveId);
        return;
    }
    auto* Following=AI->GetPathFollowingComponent();
    if (Result.Code==EPathFollowingRequestResult::Failed && bLastDestinationUsable && bOwnsNavigation && Following &&
        FollowMoveId==PreviousMove && Following->GetCurrentRequestId()==PreviousMove && AI->GetMoveStatus()!=EPathFollowingStatus::Idle) return;
    UE_LOG(LogDeliveryCompanion,Log,TEXT("CompanionFollowMove npc=%s revision=%d result=%d move=%u from=%s goal=%s owner=%s travel=%s"),
        *GetNameSafe(Npc),CommandRevision,int32(Result.Code),Result.MoveId.GetID(),
        *Npc->GetNavAgentLocation().ToCompactString(),*Destination.ToCompactString(),
        Pawn?*Pawn->GetActorLocation().ToCompactString():TEXT("none"),
        *StaticEnum<EDeliveryCompanionTravelMode>()->GetNameStringByValue(int64(TravelMode)));
    bOwnsNavigation=Result.Code==EPathFollowingRequestResult::RequestSuccessful && Following &&
        Following->GetCurrentRequestId()==Result.MoveId;
    FollowMoveId=bOwnsNavigation?Result.MoveId:FAIRequestID::InvalidRequest;
    if (bOwnsNavigation && ExpectedMode==EDeliveryCompanionMode::Follow && Pawn)
        WatchFollowMove(*Following,FollowMoveId,*Pawn,Destination,bUsingStreetWait);
    else ClearFollowMoveWatch();
    bHasFollowDestination=bOwnsNavigation || Result.Code==EPathFollowingRequestResult::AlreadyAtGoal;
    if (!bHasFollowDestination)
    {
        if (Pawn) LastOwnerPosition=Pawn->GetActorLocation();
        SetSuspension(bUsingStreetWait?FollowWaitingAtGrandpaDoor:
            ExpectedMode==EDeliveryCompanionMode::Stay?FName(TEXT("WaitSpaceBlocked")):FName(TEXT("PathBlocked")));
        NextNavigationAt=Now+GetTuning().BlockedRetrySeconds;
        return;
    }
    // 窄路只能暂避时降低重复采样，主人离开就恢复跟进
    if (Mode==EDeliveryCompanionMode::Follow && bOwnerStopped && Result.Code==EPathFollowingRequestResult::AlreadyAtGoal &&
        !Formation.IsNearby(FromOwner))
        NextNavigationAt=Now+FMath::Max(GetTuning().NavigationInterval,1.f);
    LastDestination=Destination; LastOwnerPosition=Pawn?Pawn->GetActorLocation():FVector::ZeroVector;
    // 让路不改原等候锚点，避免多次挪动逐渐离开玩家指定的区域
    const bool bAwaitingProgress=Pawn && ExpectedMode==EDeliveryCompanionMode::Follow && SuspensionReason==TEXT("PathBlocked") &&
        IsSameFollowObstructionContext(*Pawn) && (bOwnsNavigation || !Formation.IsNearby(FromOwner));
    if (bUsingStreetWait || !bAwaitingProgress)
    {
        bReportedBlocked=false;
        SetSuspension(bUsingStreetWait?FollowWaitingAtGrandpaDoor:NAME_None);
    }
}
/// @brief 跟丢主人且没人看见时，把二狗瞬移到主人附近的空站位
bool UDeliveryCompanionComponent::TeleportToOwner(APawn& OwnerPawn)
{
    auto* Npc=GetCompanion();
    auto* World=GetWorld();
    const auto* Capsule=Npc?Npc->GetCapsuleComponent():nullptr;
    const auto* Movement=Npc?Npc->GetCharacterMovement():nullptr;
    if (!World || !Capsule || !Movement || !Npc->HasAuthority() || Mode!=EDeliveryCompanionMode::Follow || Npc->IsKnockedDown() ||
        Npc->IsMountedOnCompanionBike() || HasActiveHomeArrangement() || GetHomeState().CarriedProp) return false;
    const float Radius=Capsule->GetScaledCapsuleRadius(),HalfHeight=Capsule->GetScaledCapsuleHalfHeight();
    const FVector OwnerFeet=OwnerPawn.GetNavAgentLocation();
    FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionFollowTeleport),false,Npc);
    Query.AddIgnoredActor(&OwnerPawn);
    const FCollisionShape Shape=FCollisionShape::MakeCapsule(Radius,HalfHeight);
    const FCollisionResponseParams Response(Capsule->GetCollisionResponseToChannels());
    // 绕主人一圈找脚下有实地、站得下的位置，先试身后
    const FVector Back=-FRotator(0,OwnerPawn.GetControlRotation().Yaw,0).Vector();
    for (const double Gap:{Radius+110.,Radius+170.,Radius+70.})
        for (const double Yaw:{0.,60.,-60.,120.,-120.,180.})
        {
            const FVector Above=OwnerFeet+Back.RotateAngleAxis(Yaw,FVector::UpVector)*Gap+FVector(0,0,110);
            FHitResult Ground;
            if (!World->LineTraceSingleByChannel(Ground,Above,Above-FVector(0,0,380),ECC_Visibility,Query) ||
                Ground.ImpactNormal.Z<Movement->GetWalkableFloorZ()) continue;
            const FVector Center=Ground.ImpactPoint+FVector(0,0,HalfHeight+4);
            if (World->OverlapBlockingTestByChannel(Center,FQuat::Identity,Capsule->GetCollisionObjectType(),Shape,Query,Response)) continue;
            const FVector From=Npc->GetActorLocation();
            StopFollowNavigation();
            if (!IsValid(Npc) || GetCompanion()!=Npc) return false;
            Npc->SetActorLocation(Center,false,nullptr,ETeleportType::TeleportPhysics);
            UE_LOG(LogDeliveryCompanion,Log,TEXT("CompanionFollowTeleport npc=%s from=%s to=%s"),*GetNameSafe(Npc),*From.ToCompactString(),*Center.ToCompactString());
            return true;
        }
    UE_LOG(LogDeliveryCompanion,Log,TEXT("CompanionFollowTeleportRejected npc=%s owner=%s"),*GetNameSafe(Npc),*OwnerFeet.ToCompactString());
    return false;
}

/// @brief 跟随找不到站位时跳到主人身边：检查距离、落点地面和抛物线上没有墙或关着的门
bool UDeliveryCompanionComponent::TryLeapToOwner(APawn& OwnerPawn,double Now)
{
    auto* Npc=GetCompanion();
    auto* World=GetWorld();
    auto* Movement=Npc?Npc->GetCharacterMovement():nullptr;
    const auto* Capsule=Npc?Npc->GetCapsuleComponent():nullptr;
    if (!World || !Movement || !Capsule || !Npc->HasAuthority() || Mode!=EDeliveryCompanionMode::Follow ||
        !Movement->IsMovingOnGround() || Movement->HasAnimRootMotion() || Npc->IsKnockedDown() || Npc->IsMountedOnCompanionBike() ||
        HasActiveHomeArrangement() || GetHomeState().CarriedProp || !Npc->Combat || Npc->Combat->IsReactingToHit() || Npc->Combat->IsStriking())
        return false;
    const float Radius=Capsule->GetScaledCapsuleRadius(),HalfHeight=Capsule->GetScaledCapsuleHalfHeight();
    const FVector Feet=Npc->GetNavAgentLocation(),OwnerFeet=OwnerPawn.GetNavAgentLocation();
    const FVector ToOwner=OwnerFeet-Feet;
    const double Flat=ToOwner.Size2D();
    const auto Reject=[&](const TCHAR* Reason)
    {
        UE_LOG(LogDeliveryCompanion,Log,TEXT("CompanionFollowLeapRejected npc=%s reason=%s from=%s owner=%s"),
            *GetNameSafe(Npc),Reason,*Feet.ToCompactString(),*OwnerFeet.ToCompactString());
        return false;
    };
    if (Flat<140.) return Reject(TEXT("Distance"));
    // 主人太远一步跳不到时，朝主人方向分几跳靠过去，直到重新接上能走的路
    const bool bHop=Flat>FollowLeapMaxDistance;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(CompanionFollowLeap),false,Npc);
    Query.AddIgnoredActor(&OwnerPawn);
    const FCollisionShape Shape=FCollisionShape::MakeCapsule(Radius,HalfHeight);
    const FCollisionResponseParams Response(Capsule->GetCollisionResponseToChannels());
    const ECollisionChannel Channel=Capsule->GetCollisionObjectType();
    // 落点选在主人身前靠自己这一侧，脚下必须有实地且站得下
    const FVector Direction=ToOwner.GetSafeNormal2D();
    FVector Landing=FVector::ZeroVector;
    bool bHasLanding=false;
    // 分段跳时依次试正前方和左右各偏三十度、由远到近的落点
    TArray<FVector,TInlineAllocator<9>> Aboves;
    if (bHop)
    {
        for (const double Reach:{430.,300.,200.})
            for (const double Yaw:{0.,30.,-30.})
                Aboves.Add(Feet+Direction.RotateAngleAxis(Yaw,FVector::UpVector)*Reach+FVector(0,0,160));
    }
    else
    {
        for (const double Gap:{Radius+95.,Radius+150.,Radius+60.})
            if (Flat-Gap>=70.) Aboves.Add(OwnerFeet-Direction*Gap+FVector(0,0,110));
    }
    for (const FVector& Above:Aboves)
    {
        FHitResult Ground;
        if (!World->LineTraceSingleByChannel(Ground,Above,Above-FVector(0,0,bHop?FollowLeapMaxDrop+200.:380.),ECC_Visibility,Query) ||
            Ground.ImpactNormal.Z<Movement->GetWalkableFloorZ()) continue;
        const FVector Center=Ground.ImpactPoint+FVector(0,0,HalfHeight+4);
        if (World->OverlapBlockingTestByChannel(Center,FQuat::Identity,Channel,Shape,Query,Response)) continue;
        Landing=Ground.ImpactPoint; bHasLanding=true; break;
    }
    if (!bHasLanding) return Reject(TEXT("NoLanding"));
    const double Rise=Landing.Z-Feet.Z;
    if (Rise>FollowLeapMaxRise || -Rise>FollowLeapMaxDrop) return Reject(TEXT("Height"));
    const double Gravity=FMath::Abs(Movement->GetGravityZ());
    if (Gravity<1.) return false;
    const FVector Start=Capsule->GetComponentLocation();
    const FVector FlatTo=FVector(Landing.X-Feet.X,Landing.Y-Feet.Y,0);
    for (const double Apex:{55.,110.,170.})
    {
        // 抛物线顶点高出起点和落点，逐段扫过身体胶囊，墙和关着的门都会挡下这次起跳
        const double Top=FMath::Max(0.,Rise)+Apex;
        const double Up=FMath::Sqrt(2.*Gravity*Top);
        const double FlightTime=Up/Gravity+FMath::Sqrt(2.*(Top-Rise)/Gravity);
        const FVector Launch=FlatTo/FlightTime+FVector(0,0,Up);
        if (Launch.Size2D()>950.) continue;
        bool bClear=true;
        FVector Previous=Start;
        const int32 Samples=FMath::Clamp(FMath::CeilToInt(FlightTime/.035),8,40);
        // 最后一小段贴近地面，由落点站位检查负责
        for (int32 Index=1;Index<=Samples && bClear;++Index)
        {
            const double Time=FlightTime*.92*Index/Samples;
            const FVector Next=Start+Launch*Time-FVector(0,0,.5*Gravity*FMath::Square(Time));
            bClear=!World->SweepTestByChannel(Previous,Next,FQuat::Identity,Channel,Shape,Query,Response);
            Previous=Next;
        }
        if (!bClear) continue;
        StopFollowNavigation();
        if (!IsValid(Npc) || GetCompanion()!=Npc || !Movement->IsMovingOnGround()) return false;
        Npc->SetActorRotation(FRotator(0,Direction.Rotation().Yaw,0));
        Npc->LaunchCharacter(Launch,true,true);
        UE_LOG(LogDeliveryCompanion,Log,TEXT("CompanionFollowLeap npc=%s from=%s landing=%s launch=%s flight=%.2f"),
            *GetNameSafe(Npc),*Feet.ToCompactString(),*Landing.ToCompactString(),*Launch.ToCompactString(),FlightTime);
        return true;
    }
    return Reject(TEXT("ArcBlocked"));
}

/// @brief 主人交付了二狗参与过的任务时，记一次一起送货的经历
void UDeliveryCompanionComponent::OnTaskCompleted(UDeliveryTaskDefinition* Task,const FDeliveryRewardBreakdown& Reward,APlayerState* Deliverer)
{
    auto* Npc=GetCompanion();
    if (!IsValid(Npc) || !Npc->HasAuthority() || Npc->IsActorBeingDestroyed() || !CanCommand(Deliverer) || !IsValid(Task)) return;
    const auto* Manager=UDeliveryTaskManagerComponent::Get(this);
    if (!Manager || Manager->GetTaskStatus(Task)!=EDeliveryTaskStatus::Completed) return;
    const auto* Carrier=Manager->GetCompletingPackageCarrier(Task);
    if (!IsValid(Carrier) || Carrier->GetWorld()!=GetWorld()) return;
    if (Carrier==Npc)
    {
        // 签收独立于委托的收尾，新命令可以在同一次交付通知中生效
        FDeliveryNpcObservation Fact;
        Fact.Kind=CompanionTaskDelivered; Fact.Subject=Npc; Fact.Source=EDeliveryNpcEvidenceSource::ActionResult;
        Fact.Description=FString::Printf(TEXT("游戏确认我已替主人送达订单 %s 的真实包裹，任务系统已确认交付。"),*Task->TaskId.ToString());
        Fact.Lifetime=45; Fact.bNeedsReply=false;
        Npc->Agent->ObserveEvent(Fact);
        RecordDeliveryMemory(Task,Deliverer,true);
        return;
    }
    const auto* Player=Cast<ADeliveryCharacter>(Carrier);
    auto* Pawn=GetOwnerPawn();
    // 手中真实包裹交付且二狗在场时，才记下主人亲自完成的经历
    if (!Player || Player->GetTaskPlayerState()!=Deliverer || !Pawn || !RecordDeliveryMemory(Task,Deliverer,false)) return;
    Npc->ObserveFact(TEXT("OwnerDeliveryCompleted"),Pawn,
        FString::Printf(TEXT("主人刚完成了订单 %s，我在附近陪着；包裹交付和奖励已经由游戏确认，我没有替他交件。"),*Task->TaskId.ToString()),EDeliveryNpcEvidenceSource::ActionResult);
}
/// @brief 清空跟随监听、记忆、物品和家居安排，解绑任务事件和主人战斗事件
void UDeliveryCompanionComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    ClearFollowMoveWatch();
    Memories.RemoveAll([](const FDeliveryNpcCompanionMemory& Memory)
        { return Memory.Kind==TEXT("PublicPlaceSeen"); });
    RememberedPublicPlaces.Reset();
    NextPublicPlaceObservationAt=0;
    RememberedNeighborLines.Reset();
    QuietSpeechUntil=0;
    ObservedHome.Reset();
    RememberedHomeObjects.Reset();
    ShutdownItemExecution();
    ShutdownHomeExecution();
    if (auto* Npc=GetCompanion()) Npc->CancelAssistedHelp();
    CancelAssist(TEXT("CompanionEndPlay"));
    UnbindOwnerCombat();
    RecentAssistHits.Reset();
    ReleaseErrandVehicle();
    CancelPackageHandoff();
    StopErrandMovement();
    Errand={};
    if (GetOwner()->HasAuthority() && Reason==EEndPlayReason::Destroyed) DropCarriedItem();
    if (IsValid(CarriedPackage)) CarriedPackage->OnDestroyed.RemoveDynamic(this,&UDeliveryCompanionComponent::OnCarriedPackageDestroyed);
    if (BoundTaskManager.IsValid()) BoundTaskManager->OnTaskCompleted.RemoveDynamic(this,&UDeliveryCompanionComponent::OnTaskCompleted);
    StopFollowMovement(); BoundTaskManager.Reset();
    Super::EndPlay(Reason);
}
/// @brief 复制主人、模式、命令编号、暂停原因、关系、委托和协战等状态
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
    DOREPLIFETIME(UDeliveryCompanionComponent,bAssistCommitted);
    DOREPLIFETIME(UDeliveryCompanionComponent,bCaterpillarOffered);
    DOREPLIFETIME(UDeliveryCompanionComponent,bCaterpillarGiven);
    DOREPLIFETIME(UDeliveryCompanionComponent,CarriedPackage);
    DOREPLIFETIME(UDeliveryCompanionComponent,Errand);
    DOREPLIFETIME(UDeliveryCompanionComponent,ErrandVehicle);
    DOREPLIFETIME(UDeliveryCompanionComponent,TravelMode);
    DOREPLIFETIME(UDeliveryCompanionComponent,HomeState);
}
