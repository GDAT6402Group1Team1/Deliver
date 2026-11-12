#include "AI/Residents/DeliveryResidentAIController.h"
#include "AI/Residents/DeliveryResidentCharacter.h"
#include "AI/DeliveryNpcAgentComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISense_Sight.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Character/DeliveryCharacter.h"
#include "Engine/World.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BehaviorTree.h"

ADeliveryResidentAIController::ADeliveryResidentAIController()
{
    auto* Senses=CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("ResidentPerception")); SetPerceptionComponent(*Senses);
    Sight=CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("Sight"));
    Sight->SightRadius=1200; Sight->LoseSightRadius=1400; Sight->PeripheralVisionAngleDegrees=100; Sight->SetMaxAge(6);
    Sight->DetectionByAffiliation.bDetectEnemies=true; Sight->DetectionByAffiliation.bDetectFriendlies=true; Sight->DetectionByAffiliation.bDetectNeutrals=true;
    Hearing=CreateDefaultSubobject<UAISenseConfig_Hearing>(TEXT("Hearing")); Hearing->HearingRange=900; Hearing->SetMaxAge(6);
    Hearing->DetectionByAffiliation=Sight->DetectionByAffiliation;
    Senses->ConfigureSense(*Sight); Senses->ConfigureSense(*Hearing); Senses->SetDominantSense(Sight->GetSenseImplementation());
}
void ADeliveryResidentAIController::OnPossess(APawn* InPawn)
{
    Super::OnPossess(InPawn);
    if (!HasAuthority()) return;
    GetPerceptionComponent()->OnTargetPerceptionUpdated.AddUniqueDynamic(this,&ADeliveryResidentAIController::OnTargetPerceived);
    if (const auto* Resident=Cast<ADeliveryResidentCharacter>(InPawn))
        if (Resident->BehaviorTree) RunBehaviorTree(Resident->BehaviorTree);
    RefreshBlackboard();
}
void ADeliveryResidentAIController::OnUnPossess()
{
    GetPerceptionComponent()->OnTargetPerceptionUpdated.RemoveDynamic(this,&ADeliveryResidentAIController::OnTargetPerceived);
    if (auto* Resident=Cast<ADeliveryResidentCharacter>(GetPawn())) Resident->Agent->CancelPlan(TEXT("ControllerLost"));
    Super::OnUnPossess();
}
void ADeliveryResidentAIController::OnTargetPerceived(AActor* Target,FAIStimulus Stimulus)
{
    auto* Resident=Cast<ADeliveryResidentCharacter>(GetPawn());
    if (!Resident || !HasAuthority() || !Target || Target==Resident) return;
    FDeliveryNpcObservation Event;
    Event.Kind=Stimulus.WasSuccessfullySensed()?TEXT("Perceived"):TEXT("LostSight");
    Event.Subject=Target; Event.Source=Stimulus.Type==Hearing->GetSenseID()?EDeliveryNpcEvidenceSource::Heard:EDeliveryNpcEvidenceSource::Seen;
    const auto* OtherResident=Cast<ADeliveryResidentCharacter>(Target);
    const FString Subject=OtherResident?OtherResident->DisplayName.ToString():Target->IsA<ADeliveryMotorbike>()?TEXT("电瓶车"):Target->IsA<ADeliveryCharacter>()?TEXT("快递员"):TEXT("行人");
    Event.Description=Stimulus.WasSuccessfullySensed()?FString::Printf(TEXT("感知到附近的%s，距离约 %.0f 厘米"),*Subject,FVector::Dist(Target->GetActorLocation(),Resident->GetActorLocation())):TEXT("对象已经离开视线，不能断言其当前行为");
    Event.bHasObservedLocation=true; Event.ObservedLocation=Stimulus.StimulusLocation; Event.Lifetime=45;
    if (Resident->Agent->ObserveEvent(Event)) Resident->WakeDecision();
}
void ADeliveryResidentAIController::RefreshBlackboard()
{
    const auto* Resident=Cast<ADeliveryResidentCharacter>(GetPawn()); auto* BB=GetBlackboardComponent();
    if (!Resident || !BB) return;
    const auto State=Resident->Agent->GetPublicState();
    BB->SetValueAsBool(TEXT("Unavailable"),Resident->IsKnockedDown());
    BB->SetValueAsBool(TEXT("HasApprovedAction"),Resident->Agent->HasActiveAction());
    BB->SetValueAsObject(TEXT("ActionTarget"),State.Target);
    BB->SetValueAsInt(TEXT("PlanRevision"),State.PlanRevision);
    // 只刷新仍被内置视野感知到的对象，避免站着不动就被 45 秒证据期限遗忘
    // 刷新事实不主动唤醒模型，也不在请求途中改变版本
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
            const auto* Other=Cast<ADeliveryResidentCharacter>(Target);
            Event.Description=Other?FString::Printf(TEXT("%s 仍在视线中，距离约 %.0f 厘米"),*Other->DisplayName.ToString(),FVector::Dist(Target->GetActorLocation(),Resident->GetActorLocation())):TEXT("此前看见的对象仍在视线中");
            Resident->Agent->ObserveEvent(Event);
        }
    }
}
