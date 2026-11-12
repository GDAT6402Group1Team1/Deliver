#include "AI/Characters/DeliveryNpcAIController.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISense_Sight.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Character/DeliveryCharacter.h"
#include "Engine/World.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BehaviorTree.h"

ADeliveryNpcAIController::ADeliveryNpcAIController()
{
    auto* Senses=CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("ResidentPerception")); SetPerceptionComponent(*Senses);
    Sight=CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("Sight"));
    Sight->SightRadius=1200; Sight->LoseSightRadius=1400; Sight->PeripheralVisionAngleDegrees=100; Sight->SetMaxAge(6);
    Sight->DetectionByAffiliation.bDetectEnemies=true; Sight->DetectionByAffiliation.bDetectFriendlies=true; Sight->DetectionByAffiliation.bDetectNeutrals=true;
    Hearing=CreateDefaultSubobject<UAISenseConfig_Hearing>(TEXT("Hearing")); Hearing->HearingRange=900; Hearing->SetMaxAge(6);
    Hearing->DetectionByAffiliation=Sight->DetectionByAffiliation;
    Senses->ConfigureSense(*Sight); Senses->ConfigureSense(*Hearing); Senses->SetDominantSense(Sight->GetSenseImplementation());
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
    GetPerceptionComponent()->OnTargetPerceptionUpdated.RemoveDynamic(this,&ADeliveryNpcAIController::OnTargetSeenOrHeard);
    if (auto* Resident=Cast<ADeliveryNpcCharacter>(GetPawn())) Resident->Agent->CancelPlan(TEXT("ControllerLost"));
    Super::OnUnPossess();
}
void ADeliveryNpcAIController::OnTargetSeenOrHeard(AActor* Target,FAIStimulus Stimulus)
{
    auto* Resident=Cast<ADeliveryNpcCharacter>(GetPawn());
    if (!Resident || !HasAuthority() || !Target || Target==Resident) return;
    FDeliveryNpcObservation Event;
    Event.Kind=Stimulus.WasSuccessfullySensed()?TEXT("Perceived"):TEXT("LostSight");
    Event.Subject=Target; Event.Source=Stimulus.Type==Hearing->GetSenseID()?EDeliveryNpcEvidenceSource::Heard:EDeliveryNpcEvidenceSource::Seen;
    const auto* OtherResident=Cast<ADeliveryNpcCharacter>(Target);
    const FString Subject=OtherResident?OtherResident->DisplayName.ToString():Target->IsA<ADeliveryMotorbike>()?TEXT("电瓶车"):Target->IsA<ADeliveryCharacter>()?TEXT("快递员"):TEXT("行人");
    Event.Description=Stimulus.WasSuccessfullySensed()?FString::Printf(TEXT("感知到附近的%s，距离约 %.0f 厘米"),*Subject,FVector::Dist(Target->GetActorLocation(),Resident->GetActorLocation())):TEXT("对象已经离开视线，不能断言其当前行为");
    Event.bHasObservedLocation=true; Event.ObservedLocation=Stimulus.StimulusLocation; Event.Lifetime=45;
    if (Resident->Agent->ObserveEvent(Event)) Resident->WakeDecision();
}
void ADeliveryNpcAIController::RefreshBlackboard()
{
    const auto* Resident=Cast<ADeliveryNpcCharacter>(GetPawn()); auto* BB=GetBlackboardComponent();
    if (!Resident || !BB) return;
    const auto State=Resident->Agent->GetPublicState();
    BB->SetValueAsBool(TEXT("Unavailable"),Resident->IsKnockedDown() && !Resident->Combat->IsDeliberatelyProne());
    if (BB->GetKeyID(TEXT("Listening"))!=FBlackboard::InvalidKey)
        BB->SetValueAsBool(TEXT("Listening"),Resident->Agent->IsListening());
    // 旧黑板资产仍使用这个键名，函数重命名时保留它以兼容已有行为树
    BB->SetValueAsBool(TEXT("HasApprovedAction"),Resident->Agent->HasActiveAction());
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
