#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Characters/DeliveryNpcProfile.h"
#include "AI/Characters/DeliveryNpcAIController.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/DeliveryNpcLookComponent.h"
#include "AI/Actions/DeliveryVehicleHoldComponent.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Conversation/DeliveryNpcConversationTrace.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Combat/DeliveryCollisionChannels.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "HAL/IConsoleManager.h"
#include "Engine/World.h"

namespace
{
TAutoConsoleVariable<int32> CVarNpcNavigationDiagnostics(
    TEXT("Delivery.Npc.NavigationDiagnostics"),0,
    TEXT("记录 NPC 行动候选的导航起点、终点和路径结果"),ECVF_Default);
}

ADeliveryNpcCharacter::ADeliveryNpcCharacter()
{
    bReplicates=true; SetReplicateMovement(true); PrimaryActorTick.bCanEverTick=true;
    AIControllerClass=ADeliveryNpcAIController::StaticClass(); AutoPossessAI=EAutoPossessAI::PlacedInWorldOrSpawned;
    Agent=CreateDefaultSubobject<UDeliveryNpcAgentComponent>(TEXT("Agent"));
    Combat=CreateDefaultSubobject<UDeliveryNpcCombatComponent>(TEXT("Combat"));
    Presentation=CreateDefaultSubobject<UDeliveryNpcPresentationComponent>(TEXT("Presentation"));
    Look=CreateDefaultSubobject<UDeliveryNpcLookComponent>(TEXT("Look"));
    ConversationInteractable=CreateDefaultSubobject<UDeliveryInteractableComponent>(TEXT("Conversation"));
    ConversationInteractable->PromptText=FText::FromString(TEXT("交谈")); ConversationInteractable->InteractRadius=600;
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
    ConversationInteractable->InteractRadius=600;
    GetCharacterMovement()->MaxWalkSpeed=WalkSpeed;
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
{
    if (!Interactor || IsKnockedDown()) return false;
    const auto* PC=Cast<APlayerController>(Interactor->GetController());
    const auto* Conversation=PC?PC->FindComponentByClass<UDeliveryConversationComponent>():nullptr;
    if (!Conversation || !Conversation->CanTalkTo(this)) return false;
    // 本机提示与 T 键采用同一个对象，服务器不使用远端玩家的镜头重选目标
    return !PC->IsLocalController() || Conversation->GetAimedNpc()==this;
}
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
bool ADeliveryNpcCharacter::HearSpeech(AActor* Speaker,const FString& Text,FGuid TurnId,int32 Depth,AActor* Addressee,FGuid ReplyToEventId)
{
    if (!HasAuthority() || !IsValid(Speaker) || Speaker->GetWorld()!=GetWorld() || Depth>3 ||
        (Addressee && (!IsValid(Addressee) || Addressee->GetWorld()!=GetWorld())) ||
        Text.IsEmpty() || Text.Len()>256 || FVector::DistSquared(Speaker->GetActorLocation(),GetActorLocation())>FMath::Square(900.f)) return false;
    if (!Addressee) Addressee=this;
    if (!DeliveryNpcConversationTrace::HasClearPath(this,Speaker)) return false;
    const auto* SpeakingPawn=Cast<APawn>(Speaker);
    FDeliveryNpcObservation Event; Event.EventId=TurnId;
    Event.ReplyToEventId=ReplyToEventId;
    const bool bPlayerSpeaking=SpeakingPawn && SpeakingPawn->IsPlayerControlled();
    Event.Kind=Addressee!=this?TEXT("OverheardSpeech"):bPlayerSpeaking?TEXT("PlayerSpeech"):TEXT("Speech");
    Event.Subject=Speaker; Event.Addressee=Addressee;
    Event.Source=EDeliveryNpcEvidenceSource::Heard; Event.Description=Text; Event.Lifetime=45;
    if (!Agent->ObserveEvent(Event)) return false;
    // 旁人先记住玩家问了什么，等听到回答后再决定是否插话
    if (bPlayerSpeaking && Addressee!=this) return true;
    if (Event.Kind==TEXT("PlayerSpeech"))
    {
        Agent->BeginListening(Speaker,TurnId);
        ConversationDepth=0;
        bDecisionNeeded=!Agent->RequestDecision();
        return true;
    }
    ConversationDepth=Depth; WakeDecision();
    // 正在回复时收到同伴的新话，保存后安排下一次回答
    if (Agent->IsDecisionPending()) Agent->RequestDecision();
    return true;
}
bool ADeliveryNpcCharacter::CanExecute(EDeliveryNpcAction Action,AActor* Target) const
{
    if (Action==EDeliveryNpcAction::GetUp) return Combat->IsDown();
    if (IsKnockedDown() || (Combat->IsReactingToHit() && Action!=EDeliveryNpcAction::Fight)) return false;
    if (Action==EDeliveryNpcAction::ReleaseVehicle) return GetHeldVehicle()!=nullptr;
    if (GetHeldVehicle()) return Action==EDeliveryNpcAction::Wait || Action==EDeliveryNpcAction::LookAt;
    if (Action==EDeliveryNpcAction::Pace || Action==EDeliveryNpcAction::MoveAside)
    {
        FVector Destination;
        return Action==EDeliveryNpcAction::Pace?FindPaceDestination(Destination):FindSideStepDestination(Target,Destination);
    }
    if (Action==EDeliveryNpcAction::StrikeOnce)
        return Cast<AAIController>(GetController()) && Combat->CanApproachStrike(Target);
    if (Action==EDeliveryNpcAction::Fight)
        return Cast<AAIController>(GetController()) && Combat->CanBeginFight(Target);
    if (Action==EDeliveryNpcAction::HoldVehicle)
    { const auto* Bike=Cast<ADeliveryMotorbike>(Target); return Bike && Bike->ResidentHold && Bike->ResidentHold->CanHold(this); }
    if (Action==EDeliveryNpcAction::MoveTo)
        return IsValid(Target) && Target!=this && !HasReachedMoveTarget(Target) && CanReachTarget(Target);
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
bool ADeliveryNpcCharacter::CanReachTarget(AActor* Target) const
{
    const auto* AI=Cast<AAIController>(GetController());
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!IsValid(Target) || Target==this || Target->GetWorld()!=GetWorld()) return false;
    const bool bDiagnose=CVarNpcNavigationDiagnostics.GetValueOnGameThread()!=0;
    if (!AI || !Nav)
    {
        if (bDiagnose) UE_LOG(LogTemp,Display,TEXT("NpcNavigation npc=%s target=%s stage=Unavailable controller=%d navigation=%d world=%s"),
            *GetName(),*GetNameSafe(Target),AI!=nullptr,Nav!=nullptr,*GetNameSafe(GetWorld()));
        return false;
    }
    // 与实际 MoveTo 使用相同的角色位置、目标偏移和导航过滤器
    FAIMoveRequest Request(Target);
    Request.SetNavigationFilter(AI->GetDefaultNavigationFilterClass());
    Request.SetAllowPartialPath(false);
    Request.SetRequireNavigableEndLocation(true);
    FPathFindingQuery Query;
    if (!AI->BuildPathfindingQuery(Request,Query))
    {
        if (bDiagnose) UE_LOG(LogTemp,Display,TEXT("NpcNavigation npc=%s target=%s stage=BuildQueryFailed start=%s goal=%s"),
            *GetName(),*GetNameSafe(Target),*GetNavAgentLocation().ToString(),*Target->GetActorLocation().ToString());
        return false;
    }
    const auto Result=Nav->FindPathSync(Query);
    if (bDiagnose) UE_LOG(LogTemp,Display,TEXT("NpcNavigation npc=%s target=%s stage=FindPath navData=%s start=%s end=%s result=%d valid=%d partial=%d points=%d"),
        *GetName(),*GetNameSafe(Target),*GetNameSafe(Query.NavData.Get()),*Query.StartLocation.ToString(),*Query.EndLocation.ToString(),
        int32(Result.Result),Result.Path.IsValid() && Result.Path->IsValid(),Result.Path.IsValid() && Result.Path->IsPartial(),
        Result.Path.IsValid()?Result.Path->GetPathPoints().Num():0);
    return Result.IsSuccessful() && Result.Path.IsValid() && Result.Path->IsValid() && !Result.Path->IsPartial();
}
bool ADeliveryNpcCharacter::FindNearbyWalkDestination(const FVector& Desired,float MaxTravel,FVector& OutLocation) const
{
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!Nav || !Cast<AAIController>(GetController()) || !GetCharacterMovement()->IsMovingOnGround()) return false;
    const FVector Start=GetNavAgentLocation();
    const auto& Properties=GetNavAgentPropertiesRef();
    const auto* NavData=Nav->GetNavDataForProps(Properties,Start);
    FNavLocation Projected;
    if (!NavData || !Nav->ProjectPointToNavigation(Desired,Projected,FVector(30,30,80),NavData) ||
        FVector::Dist2D(Start,Projected.Location)<35 || FMath::Abs(Start.Z-Projected.Location.Z)>60) return false;
    const float HalfHeight=GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    const auto Shape=FCollisionShape::MakeCapsule(GetCapsuleComponent()->GetScaledCapsuleRadius(),HalfHeight);
    const FVector EndCenter=Projected.Location+FVector(0,0,HalfHeight+2);
    FCollisionQueryParams Params(SCENE_QUERY_STAT(NpcNearbyWalk),false,this);
    if (GetWorld()->OverlapBlockingTestByChannel(EndCenter,FQuat::Identity,ECC_Pawn,Shape,Params)) return false;
    FHitResult Hit;
    // 查询胶囊抬高两厘米，避免把当前脚下的接触地面当成前方障碍
    if (GetWorld()->SweepSingleByChannel(Hit,GetActorLocation()+FVector(0,0,2),EndCenter,FQuat::Identity,ECC_Pawn,Shape,Params)) return false;
    FPathFindingQuery Query(this,*NavData,Start,Projected.Location);
    Query.SetAllowPartialPaths(false);
    const auto Path=Nav->FindPathSync(Properties,Query);
    if (!Path.IsSuccessful() || !Path.Path.IsValid() || Path.Path->IsPartial() || Path.Path->GetLength()>MaxTravel) return false;
    OutLocation=Projected.Location;
    return true;
}
bool ADeliveryNpcCharacter::FindPaceDestination(FVector& OutLocation) const
{
    if (!HasAuthority() || IsKnockedDown() || GetHeldVehicle()) return false;
    const float Distance=FMath::Clamp(PaceDistance,40.f,140.f);
    for (const FVector& Direction:{GetActorRightVector(),-GetActorRightVector(),GetActorForwardVector(),-GetActorForwardVector()})
        if (FindNearbyWalkDestination(GetNavAgentLocation()+Direction*Distance,Distance*1.5f,OutLocation)) return true;
    return false;
}
bool ADeliveryNpcCharacter::FindSideStepDestination(AActor* Target,FVector& OutLocation) const
{
    const auto* Person=Cast<APawn>(Target);
    if (!HasAuthority() || IsKnockedDown() || GetHeldVehicle() || !IsValid(Person) || Person==this || Person->GetWorld()!=GetWorld() ||
        FVector::DistSquared(GetActorLocation(),Person->GetActorLocation())>FMath::Square(600.f)) return false;
    FVector Toward=(Person->GetActorLocation()-GetActorLocation()).GetSafeNormal2D();
    if (Toward.IsNearlyZero()) Toward=GetActorForwardVector();
    const FVector Side=FVector::CrossProduct(FVector::UpVector,Toward).GetSafeNormal();
    const float Distance=FMath::Clamp(SideStepDistance,80.f,250.f);
    for (const float Sign:{1.f,-1.f})
        if (FindNearbyWalkDestination(GetNavAgentLocation()+Side*Sign*Distance,Distance*1.5f,OutLocation)) return true;
    return false;
}
bool ADeliveryNpcCharacter::ExecuteAction(const FDeliveryNpcPublicState& State)
{
    if (!HasAuthority() || ExecutingAction.IsValid() || !CanExecute(State.Action,State.Target)) return false;
    ExecutingAction=State.ActionId;
    if (State.Action==EDeliveryNpcAction::Pace || State.Action==EDeliveryNpcAction::MoveAside ||
        State.Action==EDeliveryNpcAction::MoveTo || State.Action==EDeliveryNpcAction::StrikeOnce || State.Action==EDeliveryNpcAction::Fight)
    {
        PreviousActionWalkSpeed=GetCharacterMovement()->MaxWalkSpeed;
        GetCharacterMovement()->MaxWalkSpeed=State.Action==EDeliveryNpcAction::Pace?PaceSpeed:
            (State.Action==EDeliveryNpcAction::StrikeOnce || State.Action==EDeliveryNpcAction::Fight)?AngryApproachSpeed:WalkSpeed;
    }
    bool bStarted=true;
    // 攻击目标还在远处时，由行为树先靠近；到达前不会启动命中判定
    if (State.Action==EDeliveryNpcAction::StrikeOnce && Combat->CanStrike(State.Target))
        bStarted=Combat->StartStrike(State.Target,State.ActionId);
    else if (State.Action==EDeliveryNpcAction::Fight) bStarted=Combat->BeginFight(State.Target,State.ActionId,State.Duration);
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
    if (!Id.IsValid() || Id!=ExecutingAction) return;
    if (auto* AI=Cast<AAIController>(GetController())) AI->StopMovement();
    if (PreviousActionWalkSpeed>=0)
    {
        GetCharacterMovement()->MaxWalkSpeed=IsKnockedDown()?0.f:PreviousActionWalkSpeed;
        PreviousActionWalkSpeed=-1;
    }
    Combat->EndFight(Id); Combat->CancelStrike(); if (GetHeldVehicle()) GetHeldVehicle()->Release(TEXT("ActionEnded")); ExecutingAction.Invalidate();
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
    ConversationInteractable->bShowPrompt=!IsKnockedDown();
    if (!HasAuthority()) return;
    if (auto* AI=Cast<ADeliveryNpcAIController>(GetController())) AI->RefreshBlackboard();
    const auto State=Agent->GetPublicState();
    if (DeliveryNpc::IsTerminal(State.Status) && State.ActionId!=LastFinishedAction)
    {
        LastFinishedAction=State.ActionId;
        if (State.Status!=EDeliveryNpcActionStatus::Succeeded || (State.Action!=EDeliveryNpcAction::Wait && State.Action!=EDeliveryNpcAction::LookAt)) bDecisionNeeded=true;
    }
    if (bDecisionNeeded && !Presentation->HasPendingSpeech() && !IsKnockedDown() && !Combat->IsReactingToHit() && !Agent->HasActiveAction() && !Agent->IsDecisionPending() && GetWorld()->GetTimeSeconds()>=NextDecision)
    { NextDecision=GetWorld()->GetTimeSeconds()+4; if (Agent->RequestDecision()) bDecisionNeeded=false; }
}
void ADeliveryNpcCharacter::EndPlay(const EEndPlayReason::Type Reason)
{
    StopAction(ExecutingAction);
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
