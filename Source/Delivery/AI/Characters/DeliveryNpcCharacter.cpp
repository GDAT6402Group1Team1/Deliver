#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Characters/DeliveryNpcProfile.h"
#include "AI/Characters/DeliveryNpcAIController.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/DeliveryNpcLookComponent.h"
#include "AI/Actions/DeliveryVehicleHoldComponent.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Combat/DeliveryCollisionChannels.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "Engine/World.h"

ADeliveryNpcCharacter::ADeliveryNpcCharacter()
{
    bReplicates=true; SetReplicateMovement(true); PrimaryActorTick.bCanEverTick=true;
    AIControllerClass=ADeliveryNpcAIController::StaticClass(); AutoPossessAI=EAutoPossessAI::PlacedInWorldOrSpawned;
    Agent=CreateDefaultSubobject<UDeliveryNpcAgentComponent>(TEXT("Agent"));
    Combat=CreateDefaultSubobject<UDeliveryNpcCombatComponent>(TEXT("Combat"));
    Presentation=CreateDefaultSubobject<UDeliveryNpcPresentationComponent>(TEXT("Presentation"));
    Look=CreateDefaultSubobject<UDeliveryNpcLookComponent>(TEXT("Look"));
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
void ADeliveryNpcCharacter::BeginPlay()
{
    Super::BeginPlay();
    ApplyProfile();
    GetCapsuleComponent()->SetCollisionObjectType(ECC_Pawn);
    // 居民靠近和拽车不压缩玩家镜头，场景墙壁仍按原 Camera 通道防穿透
    GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Camera,ECR_Ignore);
    ImpactSensor->SetCapsuleSize(GetCapsuleComponent()->GetUnscaledCapsuleRadius(),GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight());
    ConversationInteractable->bPromptAtTarget=true;
    ConversationInteractable->bUseConversationIcon=true;
    ConversationInteractable->PromptOffset=FVector(0,0,GetCapsuleComponent()->GetScaledCapsuleHalfHeight()+20);
    ConversationInteractable->CanUse.BindUObject(this,&ADeliveryNpcCharacter::CanTalk);
    ConversationInteractable->Use.BindUObject(this,&ADeliveryNpcCharacter::OpenConversation);
    Agent->OnDecisionFinished.AddUniqueDynamic(this,&ADeliveryNpcCharacter::OnDecisionFinished);
    NextDecision=GetWorld()->GetTimeSeconds()+2;
}
bool ADeliveryNpcCharacter::ApplyProfile()
{
    if (!Profile.DataTable || Profile.RowName.IsNone() || Profile.DataTable->GetRowStruct()!=FDeliveryNpcProfileRow::StaticStruct()) return false;
    const auto* Row=Profile.DataTable->FindRow<FDeliveryNpcProfileRow>(Profile.RowName,TEXT("NpcProfile"),false);
    if (!Row || Row->DisplayName.IsEmpty() || Row->Persona.IsEmpty() || Row->Persona.Len()>1024) return false;
    const FString VoiceName=Row->TtsVoice.ToString().ToLower();
    if (!Row->TtsVoice.IsNone() && VoiceName!=TEXT("male") && VoiceName!=TEXT("female"))
    {
        UE_LOG(LogTemp,Warning,TEXT("NPC profile %s has unsupported TTS voice %s; supported: male/female. Existing settings kept."),
            *Profile.RowName.ToString(),*Row->TtsVoice.ToString());
        return false;
    }
    DisplayName=Row->DisplayName;Agent->Persona=Row->Persona;Presentation->VoiceSet=Row->VoiceSet;
    if (!Row->TtsVoice.IsNone()) Presentation->TtsVoice=FName(*VoiceName);
    return true;
}
bool ADeliveryNpcCharacter::CanTalk(APawn* Interactor,bool bCheckAvailability)
{ return Interactor && !IsKnockedDown() && FVector::DistSquared(Interactor->GetActorLocation(),GetActorLocation())<=FMath::Square(350.f); }
bool ADeliveryNpcCharacter::OpenConversation(APawn* Interactor)
{
    if (!HasAuthority() || !CanTalk(Interactor,true) || !Interactor->GetController()) return false;
    if (auto* Conversation=Interactor->GetController()->FindComponentByClass<UDeliveryConversationComponent>())
    { Conversation->ClientOpenConversation(this); return true; }
    return false;
}
void ADeliveryNpcCharacter::ObserveFact(FName Kind,AActor* Subject,const FString& Text,EDeliveryNpcEvidenceSource Source)
{
    if (!HasAuthority()) return;
    FDeliveryNpcObservation Event; Event.Kind=Kind; Event.Subject=Subject; Event.Description=Text; Event.Source=Source; Event.Lifetime=45;
    Event.bHasObservedLocation=Subject && Source!=EDeliveryNpcEvidenceSource::ToldBy;
    if (Event.bHasObservedLocation) Event.ObservedLocation=Subject->GetActorLocation();
    if (Agent->ObserveEvent(Event))
    {
        // 新的现场事件可以开启下一次争执，自己的动作结束不重置接话轮数
        if (Source!=EDeliveryNpcEvidenceSource::ActionResult) ConversationDepth=0;
        WakeDecision();
    }
}
bool ADeliveryNpcCharacter::HearSpeech(AActor* Speaker,const FString& Text,FGuid TurnId,int32 Depth)
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
    if (Event.Kind==TEXT("PlayerSpeech"))
    {
        Agent->BeginListening(Speaker,TurnId);
        ConversationDepth=0;
        bDecisionNeeded=!Agent->RequestDecision();
        return true;
    }
    ConversationDepth=Depth; WakeDecision();
    // 先保存玩家的新发言，继续等待后端正在处理的请求
    // 旧回复到达后会检查是否已有新发言，再根据最新内容请求回复
    if (Agent->IsDecisionPending()) Agent->RequestDecision();
    return true;
}
bool ADeliveryNpcCharacter::CanExecute(EDeliveryNpcAction Action,AActor* Target) const
{
    if (Action==EDeliveryNpcAction::GetUp) return Combat->IsDown();
    if (IsKnockedDown()) return false;
    if (Action==EDeliveryNpcAction::ReleaseVehicle) return GetHeldVehicle()!=nullptr;
    if (GetHeldVehicle()) return Action==EDeliveryNpcAction::Wait || Action==EDeliveryNpcAction::LookAt;
    if (Action==EDeliveryNpcAction::StrikeOnce) return Combat->CanStrike(Target);
    if (Action==EDeliveryNpcAction::HoldVehicle)
    { const auto* Bike=Cast<ADeliveryMotorbike>(Target); return Bike && Bike->ResidentHold && Bike->ResidentHold->CanHold(this); }
    if (Action==EDeliveryNpcAction::MoveTo)
        return IsValid(Target) && Target!=this && Cast<AAIController>(GetController()) && !HasReachedMoveTarget(Target);
    return Action==EDeliveryNpcAction::Wait || Action==EDeliveryNpcAction::LookAt || Action==EDeliveryNpcAction::LieDown;
}
bool ADeliveryNpcCharacter::HasReachedMoveTarget(AActor* Target) const
{
    const auto* AI=Cast<AAIController>(GetController());
    const auto* PathFollowing=AI?AI->GetPathFollowingComponent():nullptr;
    if (!IsValid(Target) || !PathFollowing) return false;
    FAIMoveRequest Request(Target);
    Request.SetAcceptanceRadius(MoveAcceptanceRadius);
    Request.SetReachTestIncludesAgentRadius(true);
    Request.SetReachTestIncludesGoalRadius(true);
    return PathFollowing->HasReached(Request);
}
bool ADeliveryNpcCharacter::ExecuteAction(const FDeliveryNpcPublicState& State)
{
    if (!HasAuthority() || !CanExecute(State.Action,State.Target)) return false;
    ExecutingAction=State.ActionId;
    bool bStarted=true;
    if (State.Action==EDeliveryNpcAction::StrikeOnce) bStarted=Combat->StartStrike(State.Target,State.ActionId);
    else if (State.Action==EDeliveryNpcAction::HoldVehicle) bStarted=CastChecked<ADeliveryMotorbike>(State.Target)->ResidentHold->TryHold(this,State.ActionId);
    else if (State.Action==EDeliveryNpcAction::ReleaseVehicle) { if (GetHeldVehicle()) GetHeldVehicle()->Release(TEXT("VoluntaryRelease")); }
    else if (State.Action==EDeliveryNpcAction::LieDown) bStarted=Combat->SetDeliberateProne(true);
    else if (State.Action==EDeliveryNpcAction::GetUp) bStarted=Combat->SetDeliberateProne(false);
    if (bStarted && !State.bStreamSpeech && !State.Speech.IsEmpty() &&
        !Presentation->PublishSpeech(State.Speech,State.Emotion,ConversationDepth,State.ReplyToEventId))
    {
        UE_LOG(LogTemp,Warning,TEXT("ResidentSpeech rejected: npc=%s action=%s"),*GetName(),*State.ActionId.ToString());
        return false;
    }
    return bStarted;
}
void ADeliveryNpcCharacter::StopAction(FGuid Id)
{
    if (Id!=ExecutingAction) return;
    Combat->CancelStrike(); if (GetHeldVehicle()) GetHeldVehicle()->Release(TEXT("ActionEnded")); ExecutingAction.Invalidate();
}
void ADeliveryNpcCharacter::SetHeldVehicle(UDeliveryVehicleHoldComponent* Hold)
{
    HeldVehicle=Hold;
    GetCharacterMovement()->SetMovementMode(Hold?MOVE_None:MOVE_Walking);
}
bool ADeliveryNpcCharacter::IsKnockedDown() const { return Combat && Combat->IsDown(); }
void ADeliveryNpcCharacter::ReceiveImpact(const FVector& Impulse,const FVector& Point) { ReceiveImpactFrom(Impulse,Point,nullptr); }
void ADeliveryNpcCharacter::ReceiveImpactFrom(const FVector& Impulse,const FVector& Point,AActor* Source)
{
    if (!HasAuthority()) return;
    const bool bWasDown=Combat->IsDown();
    Presentation->CancelSpeech();Combat->ReceiveImpact(Impulse,Source);
    if (!bWasDown && Combat->IsDown()) Presentation->PlayReaction(EDeliveryNpcVoiceReaction::Hit,FGuid::NewGuid());
}
void ADeliveryNpcCharacter::ReceiveHorn(float Multiplier) { ReceiveHornFrom(Multiplier,nullptr,nullptr); }
void ADeliveryNpcCharacter::ReceiveHornFrom(float Multiplier,AActor* Vehicle,AActor* Driver)
{
    if (!HasAuthority()) return;
    const double Now=GetWorld()->GetTimeSeconds(); if (Now-LastHorn>2) HornPressure=0;
    ++HornPressure; LastHorn=Now;
    if (Now-LastHornObservation<1.5 || Agent->IsDecisionPending()) return;
    LastHornObservation=Now;
    ObserveFact(TEXT("Horn"),Vehicle,FString::Printf(TEXT("附近车辆正在鸣笛，近期累计感知 %d 次鸣笛扫描"),HornPressure),EDeliveryNpcEvidenceSource::Heard);
    if (Driver) ObserveFact(TEXT("HornDriver"),Driver,TEXT("鸣笛时这名玩家正在驾驶该车辆"),EDeliveryNpcEvidenceSource::Seen);
}
void ADeliveryNpcCharacter::Tick(float Dt)
{
    Super::Tick(Dt);
    // 交谈期间隐藏图标，为对白字幕留出位置，玩家仍可继续交谈
    ConversationInteractable->bShowPrompt=!Presentation->GetSpeech().bActive && !Agent->IsDecisionPending();
    if (!HasAuthority()) return;
    if (auto* AI=Cast<ADeliveryNpcAIController>(GetController())) AI->RefreshBlackboard();
    const auto State=Agent->GetPublicState();
    if (DeliveryNpc::IsTerminal(State.Status) && State.ActionId!=LastFinishedAction)
    {
        LastFinishedAction=State.ActionId;
        if (State.Status!=EDeliveryNpcActionStatus::Succeeded || (State.Action!=EDeliveryNpcAction::Wait && State.Action!=EDeliveryNpcAction::LookAt)) bDecisionNeeded=true;
    }
    if (bDecisionNeeded && !Presentation->HasPendingSpeech() && !IsKnockedDown() && !Agent->HasActiveAction() && !Agent->IsDecisionPending() && GetWorld()->GetTimeSeconds()>=NextDecision)
    { NextDecision=GetWorld()->GetTimeSeconds()+4; if (Agent->RequestDecision()) bDecisionNeeded=false; }
}
void ADeliveryNpcCharacter::EndPlay(const EEndPlayReason::Type Reason)
{
    if (GetHeldVehicle()) GetHeldVehicle()->Release(TEXT("ResidentEndPlay"));
    Agent->OnDecisionFinished.RemoveDynamic(this,&ADeliveryNpcCharacter::OnDecisionFinished);
    ConversationInteractable->CanUse.Unbind(); ConversationInteractable->Use.Unbind(); Super::EndPlay(Reason);
}

void ADeliveryNpcCharacter::WakeDecision()
{
    bDecisionNeeded=true; DecisionRetryCount=0;
    NextDecision=FMath::Min(NextDecision,GetWorld()->GetTimeSeconds()+.25);
}

void ADeliveryNpcCharacter::OnDecisionFinished(FName Reason)
{
    if (!HasAuthority()) return;
    if (Reason.IsNone()) { bDecisionNeeded=false; DecisionRetryCount=0; return; }
    // 请求忙碌、超时或回复失效时，最多重新请求两次
    if ((Reason==TEXT("TransportFailure") || Reason==TEXT("RequestTimeout") || Reason==TEXT("StaleProposal") || Reason==TEXT("MissingPlayerReply")) && DecisionRetryCount<2)
    {
        ++DecisionRetryCount; bDecisionNeeded=true;
        NextDecision=GetWorld()->GetTimeSeconds()+2*DecisionRetryCount;
    }
}
