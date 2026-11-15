#include "AI/Characters/DeliveryNpcAIController.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISense_Sight.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Character/DeliveryCharacter.h"
#include "Engine/World.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BehaviorTree.h"
#include "Navigation/PathFollowingComponent.h"
#include "Engine/OverlapResult.h"

ADeliveryNpcAIController::ADeliveryNpcAIController()
{
    PrimaryActorTick.bCanEverTick=true;
    auto* Senses=CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("ResidentPerception")); SetPerceptionComponent(*Senses);
    Sight=CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("Sight"));
    Sight->SightRadius=1200; Sight->LoseSightRadius=1400; Sight->PeripheralVisionAngleDegrees=100; Sight->SetMaxAge(6);
    Sight->DetectionByAffiliation.bDetectEnemies=true; Sight->DetectionByAffiliation.bDetectFriendlies=true; Sight->DetectionByAffiliation.bDetectNeutrals=true;
    Hearing=CreateDefaultSubobject<UAISenseConfig_Hearing>(TEXT("Hearing")); Hearing->HearingRange=900; Hearing->SetMaxAge(6);
    Hearing->DetectionByAffiliation=Sight->DetectionByAffiliation;
    Senses->ConfigureSense(*Sight); Senses->ConfigureSense(*Hearing); Senses->SetDominantSense(Sight->GetSenseImplementation());
}
void ADeliveryNpcAIController::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (HasAuthority())
    {
        UpdateObstacleTraversal();
        UpdateStandingSpace();
    }
}
void ADeliveryNpcAIController::StopSpacingMove()
{
    const FAIRequestID OldId=SpacingMoveId;
    SpacingMoveId=FAIRequestID::InvalidRequest;
    SpacingNeighbor.Reset();
    SpacingMoveEndsAt=0;
    auto* Path=GetPathFollowingComponent();
    // 新动作可能已经替换这条路径，只取消自己发起的那一次移动
    if (OldId.IsValid() && Path && Path->GetCurrentRequestId()==OldId)
        Path->AbortMove(*this,FPathFollowingResultFlags::UserAbort,OldId);
}
bool ADeliveryNpcAIController::HasRoomToStand(const ADeliveryNpcCharacter* Npc,const FVector& Destination) const
{
    const float Gap=FMath::Clamp(MinStandingGap,20.f,150.f);
    TArray<FOverlapResult> Nearby;
    const FVector Center=Destination+FVector(0,0,Npc->GetSimpleCollisionHalfHeight());
    GetWorld()->OverlapMultiByObjectType(Nearby,Center,FQuat::Identity,FCollisionObjectQueryParams(ECC_Pawn),
        FCollisionShape::MakeSphere(Npc->GetSimpleCollisionRadius()+Gap),FCollisionQueryParams(SCENE_QUERY_STAT(NpcStandingSpace),false,Npc));
    for (const auto& Hit:Nearby)
    {
        const auto* Other=Cast<ADeliveryNpcCharacter>(Hit.GetActor());
        if (IsValid(Other) && Other!=Npc && !Other->IsHidden() &&
            FMath::Abs(Center.Z-Other->GetActorLocation().Z)<Npc->GetSimpleCollisionHalfHeight()+Other->GetSimpleCollisionHalfHeight() &&
            FVector::Dist2D(Center,Other->GetActorLocation())<Npc->GetSimpleCollisionRadius()+Other->GetSimpleCollisionRadius()+Gap)
            return false;
    }
    return true;
}
void ADeliveryNpcAIController::UpdateStandingSpace()
{
    auto* Npc=Cast<ADeliveryNpcCharacter>(GetPawn());
    auto* Path=GetPathFollowingComponent();
    const double Now=GetWorld()->GetTimeSeconds();
    if (!bKeepDistanceWhenIdle || !Npc || !Path || Npc->IsHidden() || !Npc->CanAdjustStandingPosition())
    { StopSpacingMove(); return; }
    const float StartGap=FMath::Clamp(MinStandingGap,20.f,150.f);
    const float StopGap=FMath::Max(StartGap+30.f,FMath::Clamp(ComfortableStandingGap,40.f,200.f));
    if (SpacingMoveId.IsValid())
    {
        const auto* Other=SpacingNeighbor.Get();
        const bool bFinished=Path->GetCurrentRequestId()!=SpacingMoveId || GetMoveStatus()==EPathFollowingStatus::Idle;
        const bool bOtherBusy=Other && !Other->IsKnockedDown() && !Other->CanAdjustStandingPosition();
        const bool bFarEnough=Other && FVector::Dist2D(Npc->GetActorLocation(),Other->GetActorLocation())>=
            Npc->GetSimpleCollisionRadius()+Other->GetSimpleCollisionRadius()+StopGap-8.f;
        if (bFinished || !Other || bOtherBusy || bFarEnough || Now>=SpacingMoveEndsAt)
        {
            StopSpacingMove();
            NextSpacingCheck=Now+1.2;
        }
        return;
    }
    if (Now<NextSpacingCheck || GetMoveStatus()!=EPathFollowingStatus::Idle || Npc->GetVelocity().SizeSquared2D()>FMath::Square(20.f)) return;
    NextSpacingCheck=Now+.4;
    TArray<FOverlapResult> Nearby;
    GetWorld()->OverlapMultiByObjectType(Nearby,Npc->GetActorLocation(),FQuat::Identity,FCollisionObjectQueryParams(ECC_Pawn),
        FCollisionShape::MakeSphere(Npc->GetSimpleCollisionRadius()+StartGap),FCollisionQueryParams(SCENE_QUERY_STAT(NpcStandingNeighbors),false,Npc));
    ADeliveryNpcCharacter* Neighbor=nullptr;
    float NearestGap=StartGap;
    for (const auto& Hit:Nearby)
    {
        auto* Other=Cast<ADeliveryNpcCharacter>(Hit.GetActor());
        if (!IsValid(Other) || Other==Npc || Other->IsHidden()) continue;
        const auto& Action=Other->Agent->GetPublicState();
        // 对方还在向自己出拳或追打时，不用空闲避让替模型决定逃跑
        if (Other->Agent->HasActiveAction() && Action.Target==Npc &&
            (Action.Action==EDeliveryNpcAction::Fight || Action.Action==EDeliveryNpcAction::StrikeOnce)) return;
        if (!Other->IsKnockedDown() && !Other->CanAdjustStandingPosition()) continue;
        const auto* OtherController=Cast<AAIController>(Other->GetController());
        if (Other->GetVelocity().SizeSquared2D()>FMath::Square(20.f) ||
            (OtherController && OtherController->GetMoveStatus()!=EPathFollowingStatus::Idle)) continue;
        const float Gap=FVector::Dist2D(Npc->GetActorLocation(),Other->GetActorLocation())-
            Npc->GetSimpleCollisionRadius()-Other->GetSimpleCollisionRadius();
        if (Gap>=NearestGap || FMath::Abs(Npc->GetActorLocation().Z-Other->GetActorLocation().Z)>=
            Npc->GetSimpleCollisionHalfHeight()+Other->GetSimpleCollisionHalfHeight()) continue;
        FHitResult Obstacle;
        if (GetWorld()->LineTraceSingleByChannel(Obstacle,Npc->GetActorLocation(),Other->GetActorLocation(),ECC_Visibility,
            FCollisionQueryParams(SCENE_QUERY_STAT(NpcStandingSight),false,Npc)) && Obstacle.GetActor()!=Other) continue;
        NearestGap=Gap; Neighbor=Other;
    }
    if (!Neighbor) return;
    FVector Destination;
    if (!Npc->FindStandingSpace(Neighbor,StopGap,Destination) || !HasRoomToStand(Npc,Destination))
    { NextSpacingCheck=Now+1.5; return; }
    FAIMoveRequest Request(Destination);
    Request.SetNavigationFilter(GetDefaultNavigationFilterClass());
    Request.SetAcceptanceRadius(8.f);
    Request.SetReachTestIncludesAgentRadius(false);
    Request.SetReachTestIncludesGoalRadius(false);
    Request.SetAllowPartialPath(false);
    const auto Result=MoveTo(Request);
    if (Result.Code==EPathFollowingRequestResult::RequestSuccessful)
    {
        SpacingMoveId=Result.MoveId;
        SpacingNeighbor=Neighbor;
        SpacingMoveEndsAt=Now+3;
    }
    else NextSpacingCheck=Now+1.5;
}
void ADeliveryNpcAIController::OnPossess(APawn* InPawn)
{
    Super::OnPossess(InPawn);
    if (!HasAuthority()) return;
    GetPerceptionComponent()->OnTargetPerceptionUpdated.AddUniqueDynamic(this,&ADeliveryNpcAIController::OnTargetSeenOrHeard);
    if (const auto* Resident=Cast<ADeliveryNpcCharacter>(InPawn))
        if (Resident->BehaviorTree) RunBehaviorTree(Resident->BehaviorTree);
    RefreshBlackboard();
}
void ADeliveryNpcAIController::OnUnPossess()
{
    StopObstacleTraversal();
    ObstacleRetreatAttempts.Reset();
    if (auto* Npc=Cast<ADeliveryNpcCharacter>(GetPawn()); Npc && Npc->Combat) Npc->Combat->ResetHitFacing();
    StopSpacingMove();
    GetPerceptionComponent()->OnTargetPerceptionUpdated.RemoveDynamic(this,&ADeliveryNpcAIController::OnTargetSeenOrHeard);
    if (auto* Resident=Cast<ADeliveryNpcCharacter>(GetPawn())) Resident->Agent->CancelPlan(TEXT("ControllerLost"));
    Super::OnUnPossess();
}
void ADeliveryNpcAIController::EndPlay(const EEndPlayReason::Type Reason)
{
    StopObstacleTraversal();
    ObstacleRetreatAttempts.Reset();
    StopSpacingMove();
    Super::EndPlay(Reason);
}
void ADeliveryNpcAIController::OnTargetSeenOrHeard(AActor* Target,FAIStimulus Stimulus)
{
    auto* Resident=Cast<ADeliveryNpcCharacter>(GetPawn());
    if (!Resident || !HasAuthority() || !IsValid(Target) || Target==Resident || !Stimulus.Type.IsValid()) return;
    const bool bSight=Stimulus.Type==Sight->GetSenseID();
    const bool bHearing=Stimulus.Type==Hearing->GetSenseID();
    const bool bSensed=Stimulus.WasSuccessfullySensed();
    // 听觉记录到期不能说明说话的人离开了视线
    if ((!bSight && !bHearing) || (bHearing && !bSensed)) return;

    FDeliveryNpcObservation Event;
    Event.Kind=bSensed?TEXT("Perceived"):TEXT("LostSight");
    Event.Subject=Target;
    Event.Source=bHearing?EDeliveryNpcEvidenceSource::Heard:EDeliveryNpcEvidenceSource::Seen;
    // 视觉失效回调保留 UE 上次看见的位置，不读取对象当前的坐标
    Event.bHasObservedLocation=FAISystem::IsValidLocation(Stimulus.StimulusLocation);
    if (Event.bHasObservedLocation) Event.ObservedLocation=Stimulus.StimulusLocation;
    Event.Lifetime=45;
    const auto* OtherResident=Cast<ADeliveryNpcCharacter>(Target);
    const FString Subject=OtherResident?OtherResident->DisplayName.ToString():Target->IsA<ADeliveryMotorbike>()?TEXT("电瓶车"):Target->IsA<ADeliveryCharacter>()?TEXT("快递员"):TEXT("行人");
    if (bSensed)
    {
        Event.Description=bSight?FString::Printf(TEXT("看见附近的%s"),*Subject):FString::Printf(TEXT("听见附近的%s发出声音"),*Subject);
        if (Event.bHasObservedLocation && FAISystem::IsValidLocation(Stimulus.ReceiverLocation))
            Event.Description+=FString::Printf(TEXT("，当时距离约 %.0f 厘米"),FVector::Dist(Stimulus.StimulusLocation,Stimulus.ReceiverLocation));
    }
    else
    {
        Event.Description=FString::Printf(TEXT("暂时看不见%s，可能只是转头或被遮挡，不能据此判断对方已经走远"),*Subject);
        if (Event.bHasObservedLocation) Event.Description+=TEXT("；记录的位置是最后一次看见的位置");
    }
    if (Resident->Agent->ObserveEvent(Event)) Resident->WakeDecision();
}
void ADeliveryNpcAIController::RefreshBlackboard()
{
    const auto* Resident=Cast<ADeliveryNpcCharacter>(GetPawn()); auto* BB=GetBlackboardComponent();
    if (!Resident || !BB) return;
    const auto State=Resident->Agent->GetPublicState();
    BB->SetValueAsBool(TEXT("Unavailable"),Resident->IsKnockedDown() && !Resident->Combat->IsDeliberatelyProne());
    if (BB->GetKeyID(TEXT("Listening"))!=FBlackboard::InvalidKey)
        BB->SetValueAsBool(TEXT("Listening"),Resident->Agent->ShouldPauseForConversation());
    // 旧黑板资产仍使用这个键名，函数重命名时保留它以兼容已有行为树
    BB->SetValueAsBool(TEXT("HasApprovedAction"),Resident->Agent->HasActiveAction());
    // 二狗使用独立黑板，普通居民不增加同伴状态；暂停不抹掉已经确认的委托
    if (BB->GetKeyID(TEXT("HasCompanionErrand"))!=FBlackboard::InvalidKey)
    {
        const auto* Companion=Resident->FindComponentByClass<UDeliveryCompanionComponent>();
        BB->SetValueAsBool(TEXT("HasCompanionErrand"),Companion && Companion->HasActiveErrand());
        if (BB->GetKeyID(TEXT("HasNearbyItemSearch"))!=FBlackboard::InvalidKey)
            BB->SetValueAsBool(TEXT("HasNearbyItemSearch"),Companion && Companion->HasNearbyItemSearch());
    }
    BB->SetValueAsObject(TEXT("ActionTarget"),State.Target);
    BB->SetValueAsInt(TEXT("PlanRevision"),State.PlanRevision);
    // 定时更新视野内的角色，让 NPC 记得仍在眼前的人
    // 这里只更新可见时间，是否请求模型由后续交谈或事件决定
    const double Now=GetWorld()->GetTimeSeconds();
    if (Now>=NextSightRefresh && !Resident->Agent->IsDecisionPending())
    {
        NextSightRefresh=Now+15;
        TArray<AActor*> Visible;
        GetPerceptionComponent()->GetCurrentlyPerceivedActors(UAISense_Sight::StaticClass(),Visible);
        for (int32 Index=0;Index<FMath::Min(Visible.Num(),8);++Index)
        {
            auto* Target=Visible[Index]; if (!IsValid(Target) || Target==Resident) continue;
            FDeliveryNpcObservation Event; Event.Kind=TEXT("VisibleNow"); Event.Subject=Target;
            Event.Source=EDeliveryNpcEvidenceSource::Seen; Event.Lifetime=45;
            Event.bHasObservedLocation=true; Event.ObservedLocation=Target->GetActorLocation();
            const auto* Other=Cast<ADeliveryNpcCharacter>(Target);
            Event.Description=Other?FString::Printf(TEXT("%s 仍在视线中，距离约 %.0f 厘米"),*Other->DisplayName.ToString(),FVector::Dist(Target->GetActorLocation(),Resident->GetActorLocation())):TEXT("此前看见的对象仍在视线中");
            Resident->Agent->ObserveEvent(Event);
        }
    }
}
