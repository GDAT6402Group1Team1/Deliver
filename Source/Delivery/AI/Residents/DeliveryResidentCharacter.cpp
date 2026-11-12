#include "AI/Residents/DeliveryResidentCharacter.h"
#include "AI/Residents/DeliveryResidentAIController.h"
#include "AI/Residents/DeliveryResidentCombatComponent.h"
#include "AI/Residents/DeliveryNpcPresentationComponent.h"
#include "AI/Residents/DeliveryVehicleHoldComponent.h"
#include "AI/Residents/DeliveryConversationComponent.h"
#include "AI/DeliveryNpcAgentComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Combat/DeliveryCollisionChannels.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"

ADeliveryResidentCharacter::ADeliveryResidentCharacter()
{
    bReplicates=true; SetReplicateMovement(true); PrimaryActorTick.bCanEverTick=true;
    AIControllerClass=ADeliveryResidentAIController::StaticClass(); AutoPossessAI=EAutoPossessAI::PlacedInWorldOrSpawned;
    Agent=CreateDefaultSubobject<UDeliveryNpcAgentComponent>(TEXT("Agent"));
    Combat=CreateDefaultSubobject<UDeliveryResidentCombatComponent>(TEXT("Combat"));
    Presentation=CreateDefaultSubobject<UDeliveryNpcPresentationComponent>(TEXT("Presentation"));
    ConversationInteractable=CreateDefaultSubobject<UDeliveryInteractableComponent>(TEXT("Conversation"));
    ConversationInteractable->PromptText=FText::FromString(TEXT("交谈")); ConversationInteractable->InteractRadius=300;
    GetCapsuleComponent()->InitCapsuleSize(34,88); GetCapsuleComponent()->SetCollisionObjectType(ECC_Pawn);
    GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Camera,ECR_Ignore);
    // Pawn 胶囊负责贴地；专用查询体只供车撞和出拳发现，不能用 Ignore 地面的通道行走
    ImpactSensor=CreateDefaultSubobject<UCapsuleComponent>(TEXT("ImpactSensor")); ImpactSensor->SetupAttachment(GetRootComponent());
    ImpactSensor->InitCapsuleSize(34,88); ImpactSensor->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    ImpactSensor->SetCollisionObjectType(DeliveryCollision::Pedestrian); ImpactSensor->SetCollisionResponseToAllChannels(ECR_Overlap);
    ImpactSensor->SetGenerateOverlapEvents(false);
    GetMesh()->SetRelativeLocationAndRotation(FVector(0,0,-88),FRotator(0,-90,0)); GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    GetCharacterMovement()->MaxWalkSpeed=180;
    // 由 AI 的关注对象和导航方向提供朝向，停步时 LookAt 也能真正转身
    GetCharacterMovement()->bOrientRotationToMovement=false;
    GetCharacterMovement()->bUseControllerDesiredRotation=true;
    GetCharacterMovement()->RotationRate=FRotator(0,240,0);
    bUseControllerRotationYaw=false; DisplayName=FText::FromString(TEXT("居民"));
}
void ADeliveryResidentCharacter::BeginPlay()
{
    Super::BeginPlay();
    GetCapsuleComponent()->SetCollisionObjectType(ECC_Pawn);
    // 居民靠近和拽车不压缩玩家镜头，场景墙壁仍按原 Camera 通道防穿透
    GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Camera,ECR_Ignore);
    ImpactSensor->SetCapsuleSize(GetCapsuleComponent()->GetUnscaledCapsuleRadius(),GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight());
    ConversationInteractable->bPromptAtTarget=true;
    ConversationInteractable->bUseConversationIcon=true;
    ConversationInteractable->PromptOffset=FVector(0,0,GetCapsuleComponent()->GetScaledCapsuleHalfHeight()+20);
    ConversationInteractable->CanUse.BindUObject(this,&ADeliveryResidentCharacter::CanTalk);
    ConversationInteractable->Use.BindUObject(this,&ADeliveryResidentCharacter::OpenConversation);
    Agent->OnDecisionFinished.AddUniqueDynamic(this,&ADeliveryResidentCharacter::OnDecisionFinished);
    NextDecision=GetWorld()->GetTimeSeconds()+2;
}
bool ADeliveryResidentCharacter::CanTalk(APawn* Interactor,bool bCheckAvailability)
{ return Interactor && !IsKnockedDown() && FVector::DistSquared(Interactor->GetActorLocation(),GetActorLocation())<=FMath::Square(350.f); }
bool ADeliveryResidentCharacter::OpenConversation(APawn* Interactor)
{
    if (!HasAuthority() || !CanTalk(Interactor,true) || !Interactor->GetController()) return false;
    if (auto* Conversation=Interactor->GetController()->FindComponentByClass<UDeliveryConversationComponent>())
    { Conversation->ClientOpenConversation(this); return true; }
    return false;
}
void ADeliveryResidentCharacter::ObserveFact(FName Kind,AActor* Subject,const FString& Text,EDeliveryNpcEvidenceSource Source)
{
    if (!HasAuthority()) return;
    FDeliveryNpcObservation Event; Event.Kind=Kind; Event.Subject=Subject; Event.Description=Text; Event.Source=Source; Event.Lifetime=45;
    Event.bHasObservedLocation=Subject && Source!=EDeliveryNpcEvidenceSource::ToldBy;
    if (Event.bHasObservedLocation) Event.ObservedLocation=Subject->GetActorLocation();
    Agent->ObserveEvent(Event); WakeDecision();
}
bool ADeliveryResidentCharacter::HearSpeech(AActor* Speaker,const FString& Text,FGuid TurnId,int32 Depth)
{
    if (!HasAuthority() || !IsValid(Speaker) || Speaker->GetWorld()!=GetWorld() || Depth>3 ||
        Text.IsEmpty() || Text.Len()>256 || FVector::DistSquared(Speaker->GetActorLocation(),GetActorLocation())>FMath::Square(900.f)) return false;
    FHitResult Hit; FCollisionQueryParams Params(SCENE_QUERY_STAT(ResidentSpeech),false,this);
    if (GetWorld()->LineTraceSingleByChannel(Hit,GetActorLocation(),Speaker->GetActorLocation(),ECC_Visibility,Params) && Hit.GetActor()!=Speaker) return false;
    const auto* SpeakingPawn=Cast<APawn>(Speaker);
    FDeliveryNpcObservation Event; Event.EventId=TurnId;
    Event.Kind=SpeakingPawn && SpeakingPawn->IsPlayerControlled()?TEXT("PlayerSpeech"):TEXT("Speech"); Event.Subject=Speaker;
    Event.Source=EDeliveryNpcEvidenceSource::Heard; Event.Description=Text; Event.Lifetime=45;
    if (!Agent->ObserveEvent(Event)) return false;
    ConversationDepth=Depth; WakeDecision();
    // 新话进入最新上下文，但不取消仍占后端模型槽的 HTTP 请求
    // 旧回复会因版本变化被拒绝，再用合并后的事实重新决策
    if (Agent->IsDecisionPending()) Agent->RequestDecision();
    return true;
}
bool ADeliveryResidentCharacter::CanExecute(EDeliveryNpcAction Action,AActor* Target) const
{
    if (Action==EDeliveryNpcAction::GetUp) return Combat->IsDown();
    if (IsKnockedDown()) return false;
    if (Action==EDeliveryNpcAction::ReleaseVehicle) return GetHeldVehicle()!=nullptr;
    if (GetHeldVehicle()) return Action==EDeliveryNpcAction::Wait || Action==EDeliveryNpcAction::LookAt;
    if (Action==EDeliveryNpcAction::StrikeOnce) return Combat->CanStrike(Target);
    if (Action==EDeliveryNpcAction::HoldVehicle)
    { const auto* Bike=Cast<ADeliveryMotorbike>(Target); return Bike && Bike->ResidentHold && Bike->ResidentHold->CanHold(this); }
    if (Action==EDeliveryNpcAction::MoveTo) return IsValid(Target) && Target!=this;
    return Action==EDeliveryNpcAction::Wait || Action==EDeliveryNpcAction::LookAt || Action==EDeliveryNpcAction::LieDown;
}
bool ADeliveryResidentCharacter::BeginApprovedAction(const FDeliveryNpcPublicState& State)
{
    if (!HasAuthority() || !CanExecute(State.Action,State.Target)) return false;
    ExecutingAction=State.ActionId;
    bool bStarted=true;
    if (State.Action==EDeliveryNpcAction::StrikeOnce) bStarted=Combat->StartStrike(State.Target,State.ActionId);
    else if (State.Action==EDeliveryNpcAction::HoldVehicle) bStarted=CastChecked<ADeliveryMotorbike>(State.Target)->ResidentHold->TryHold(this,State.ActionId);
    else if (State.Action==EDeliveryNpcAction::ReleaseVehicle) { if (GetHeldVehicle()) GetHeldVehicle()->Release(TEXT("VoluntaryRelease")); }
    else if (State.Action==EDeliveryNpcAction::LieDown) bStarted=Combat->SetDeliberateProne(true);
    else if (State.Action==EDeliveryNpcAction::GetUp) bStarted=Combat->SetDeliberateProne(false);
    if (bStarted && !State.Speech.IsEmpty() &&
        !Presentation->PublishSpeech(State.Speech,State.Emotion,ConversationDepth,State.ReplyToEventId))
    {
        UE_LOG(LogTemp,Warning,TEXT("ResidentSpeech rejected: npc=%s action=%s"),*GetName(),*State.ActionId.ToString());
        return false;
    }
    return bStarted;
}
void ADeliveryResidentCharacter::EndApprovedAction(FGuid Id)
{
    if (Id!=ExecutingAction) return;
    Combat->CancelStrike(); if (GetHeldVehicle()) GetHeldVehicle()->Release(TEXT("ActionEnded")); ExecutingAction.Invalidate();
}
void ADeliveryResidentCharacter::SetHeldVehicle(UDeliveryVehicleHoldComponent* Hold)
{
    HeldVehicle=Hold;
    GetCharacterMovement()->SetMovementMode(Hold?MOVE_None:MOVE_Walking);
}
bool ADeliveryResidentCharacter::IsKnockedDown() const { return Combat && Combat->IsDown(); }
void ADeliveryResidentCharacter::ReceiveImpact(const FVector& Impulse,const FVector& Point) { ReceiveImpactFrom(Impulse,Point,nullptr); }
void ADeliveryResidentCharacter::ReceiveImpactFrom(const FVector& Impulse,const FVector& Point,AActor* Source)
{ if (HasAuthority()) { Presentation->CancelSpeech(); Combat->ReceiveImpact(Impulse,Source); } }
void ADeliveryResidentCharacter::ReceiveHorn(float Multiplier) { ReceiveHornFrom(Multiplier,nullptr,nullptr); }
void ADeliveryResidentCharacter::ReceiveHornFrom(float Multiplier,AActor* Vehicle,AActor* Driver)
{
    if (!HasAuthority()) return;
    const double Now=GetWorld()->GetTimeSeconds(); if (Now-LastHorn>2) HornPressure=0;
    ++HornPressure; LastHorn=Now;
    if (Now-LastHornObservation<1.5 || Agent->IsDecisionPending()) return;
    LastHornObservation=Now;
    ObserveFact(TEXT("Horn"),Vehicle,FString::Printf(TEXT("附近车辆正在鸣笛，近期累计感知 %d 次鸣笛扫描"),HornPressure),EDeliveryNpcEvidenceSource::Heard);
    if (Driver) ObserveFact(TEXT("HornDriver"),Driver,TEXT("鸣笛时这名玩家正在驾驶该车辆"),EDeliveryNpcEvidenceSource::Seen);
}
void ADeliveryResidentCharacter::Tick(float Dt)
{
    Super::Tick(Dt);
    // 对话字幕和思考省略号优先，隐藏交互图标但不禁用交谈操作
    ConversationInteractable->bShowPrompt=!Presentation->GetSpeech().bActive && !Agent->IsDecisionPending();
    if (!HasAuthority()) return;
    if (auto* AI=Cast<ADeliveryResidentAIController>(GetController())) AI->RefreshBlackboard();
    const auto State=Agent->GetPublicState();
    if (DeliveryNpc::IsTerminal(State.Status) && State.ActionId!=LastFinishedAction)
    {
        LastFinishedAction=State.ActionId;
        if (State.Status!=EDeliveryNpcActionStatus::Succeeded || (State.Action!=EDeliveryNpcAction::Wait && State.Action!=EDeliveryNpcAction::LookAt)) bDecisionNeeded=true;
    }
    if (bDecisionNeeded && !Presentation->GetSpeech().bActive && !IsKnockedDown() && !Agent->HasActiveAction() && !Agent->IsDecisionPending() && GetWorld()->GetTimeSeconds()>=NextDecision)
    { NextDecision=GetWorld()->GetTimeSeconds()+4; if (Agent->RequestDecision()) bDecisionNeeded=false; }
}
void ADeliveryResidentCharacter::EndPlay(const EEndPlayReason::Type Reason)
{
    if (GetHeldVehicle()) GetHeldVehicle()->Release(TEXT("ResidentEndPlay"));
    Agent->OnDecisionFinished.RemoveDynamic(this,&ADeliveryResidentCharacter::OnDecisionFinished);
    ConversationInteractable->CanUse.Unbind(); ConversationInteractable->Use.Unbind(); Super::EndPlay(Reason);
}

void ADeliveryResidentCharacter::WakeDecision()
{
    bDecisionNeeded=true; DecisionRetryCount=0;
    NextDecision=FMath::Min(NextDecision,GetWorld()->GetTimeSeconds()+.25);
}

void ADeliveryResidentCharacter::OnDecisionFinished(FName Reason)
{
    if (!HasAuthority()) return;
    if (Reason.IsNone()) { bDecisionNeeded=false; DecisionRetryCount=0; return; }
    // 忙碌、超时和过期回复最多补两次，失败不静默吞掉玩家已经说出的消息
    if ((Reason==TEXT("TransportFailure") || Reason==TEXT("RequestTimeout") || Reason==TEXT("StaleProposal") || Reason==TEXT("MissingPlayerReply")) && DecisionRetryCount<2)
    {
        ++DecisionRetryCount; bDecisionNeeded=true;
        NextDecision=GetWorld()->GetTimeSeconds()+2*DecisionRetryCount;
    }
}
