#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionTuning.h"
#include "AI/Companion/Encounter/DeliveryCompanionEncounterScene.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Conversation/DeliveryNpcConversationTrace.h"
#include "AIController.h"
#include "Navigation/PathFollowingComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Interaction/DeliveryInteractionComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/GameNetworkManager.h"
#include "GameFramework/GameStateBase.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "Animation/AnimSequence.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Inventory/Items/DeliveryHandheldItem.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Vehicle/Companion/DeliveryCompanionMotorbike.h"

ADeliveryCompanionCharacter::ADeliveryCompanionCharacter()
{
    Companion=CreateDefaultSubobject<UDeliveryCompanionComponent>(TEXT("Companion"));
    PackageAnchor=CreateDefaultSubobject<USceneComponent>(TEXT("PackageAnchor"));
    PackageAnchor->SetupAttachment(GetRootComponent());
    PackageAnchor->SetRelativeLocation(FVector(35,0,10));
    HandheldAnchor=CreateDefaultSubobject<USceneComponent>(TEXT("HandheldAnchor"));
    HandheldAnchor->SetupAttachment(GetMesh(),TEXT("RightHand"));
    HandheldAnchor->SetAbsolute(false,false,true);
    HandheldAnchor->SetRelativeTransform(FTransform::Identity);
    HelpInteractable=CreateDefaultSubobject<UDeliveryInteractableComponent>(TEXT("HelpUp"));
    HelpInteractable->InteractionKey=EDeliveryInteractionKey::PickupE;
    HelpInteractable->PromptText=NSLOCTEXT("Delivery","HelpErgou","按住 E 扶起二狗");
    HelpInteractable->InteractRadius=180;
    HelpInteractable->HoldDuration=1;
    HelpInteractable->bPromptAtTarget=true;
    HelpInteractable->PromptOffset=FVector(0,0,30);
    DisplayName=NSLOCTEXT("Delivery","ErgouName","牛二狗");
    GetCharacterMovement()->bUseRVOAvoidance=true;
    GetCharacterMovement()->AvoidanceConsiderationRadius=400;
    Combat->bHarmlessStrikes=true;
    Combat->bKnockDownOnAnyPunch=true;
    // 二狗只在模型选择协战后动手，挨打不自动还手
    Combat->bRetaliateWhenHit=false;
    Combat->bReceiveHitsWhileDown=true;
    Combat->KnockdownHorizontalSpeed=600;
    Combat->KnockdownUpwardSpeed=250;
}

bool ADeliveryCompanionCharacter::IsMountedOnCompanionBike() const
{
    const auto* Bike=Cast<ADeliveryCompanionMotorbike>(GetAttachParentActor());
    return IsValid(Bike) && !Bike->IsActorBeingDestroyed() && Bike->GetWorld()==GetWorld() &&
        Bike->IsMounted() && Bike->GetReservedCompanion()==this && GetRootComponent() &&
        GetRootComponent()->GetAttachParent()==Bike->CompanionSeat.Get();
}

bool ADeliveryCompanionCharacter::IsNetRelevantFor(const AActor* RealViewer,const AActor* ViewTarget,const FVector& SrcLocation) const
{
    if (Super::IsNetRelevantFor(RealViewer,ViewTarget,SrcLocation)) return true;
    if (!IsMountedOnCompanionBike() || !IsValid(RealViewer) || RealViewer->GetWorld()!=GetWorld()) return false;
    // 骑乘用车上的网格替身，隐藏的原角色仍承载交谈、声音和真实同伴状态
    const auto* Player=Cast<APlayerController>(RealViewer);
    if (Player && Companion && Companion->CanCommand(Player->PlayerState)) return true;
    return !bOnlyRelevantToOwner && (!GetDefault<AGameNetworkManager>()->bUseDistanceBasedRelevancy ||
        IsWithinNetRelevancyDistance(SrcLocation));
}

void ADeliveryCompanionCharacter::BeginPlay()
{
    Super::BeginPlay();
    Companion->InitializeHomeAbility();
    HelpInteractable->HoldDuration=Companion->GetTuning().HelpHoldSeconds;
    HelpInteractable->CanUse.BindUObject(this,&ADeliveryCompanionCharacter::CanHelp);
    HelpInteractable->Use.BindUObject(this,&ADeliveryCompanionCharacter::Help);
    Combat->CanContinueAssistedRecovery.BindUObject(this,&ThisClass::CanContinueAssistedHelp);
    Combat->OnDownChanged.AddUObject(this,&ThisClass::OnCombatDownChanged);
    Combat->OnRecoveryFinished.AddUObject(this,&ThisClass::OnCombatRecoveryFinished);
    OnRep_Encounter();
    UpdateHelpInteraction();
}
bool ADeliveryCompanionCharacter::CanHelp(APawn* Helper,bool bCheckAvailability)
{
    if (Encounter.Phase!=EDeliveryCompanionEncounterPhase::Joined)
        return IsWaitingForHelp() && Companion->CanRecruit(Helper);
    if (Combat->IsDown())
        return Combat->CanStartAssistedRecovery() && CanContinueAssistedHelp(Helper,Combat->GetDownEpisodeId());
    return Companion->CanAcceptCaterpillar(Helper);
}
bool ADeliveryCompanionCharacter::Help(APawn* Helper)
{
    if (!HasAuthority() || !CanHelp(Helper,true)) return false;
    if (Encounter.Phase!=EDeliveryCompanionEncounterPhase::Joined) return Companion->TryRecruit(Helper);
    if (Combat->IsDown()) return Combat->StartAssistedRecovery(Combat->GetDownEpisodeId(),Helper);
    return Companion->AcceptCaterpillar(Helper);
}
bool ADeliveryCompanionCharacter::CanContinueAssistedHelp(APawn* Helper,FGuid DownEpisode) const
{
    const auto* Player=Cast<ADeliveryCharacter>(Helper);
    const auto* PC=IsValid(Player)?Cast<APlayerController>(Player->GetController()):nullptr;
    if (Encounter.Phase!=EDeliveryCompanionEncounterPhase::Joined || !DownEpisode.IsValid() ||
        Combat->GetDownEpisodeId()!=DownEpisode || !Combat->IsDown() || !GetCharacterMovement()->IsMovingOnGround() ||
        !PC || PC->GetPawn()!=Player || !Companion->CanCommand(PC->PlayerState) ||
        Player->GetWorld()!=GetWorld() || !Player->CanHandleTaskPackage() ||
        (Player->GetActiveRagdoll() && Player->GetActiveRagdoll()->GetControlMode()!=EDeliveryRagdollControlMode::Active) ||
        (Player->GetGrabComponent() && Player->GetGrabComponent()->IsGrabbing()) || Companion->IsUsingErrandVehicle() ||
        FVector::DistSquared(Player->GetActorLocation(),GetActorLocation())>FMath::Square(HelpInteractable->InteractRadius)) return false;
    // 扶起采用严格物体射线，不忽略挡在双方之间的其他人物和车辆
    FCollisionQueryParams Params(SCENE_QUERY_STAT(CompanionAssistedHelp),false,this);
    Params.AddIgnoredActor(Player);
    FCollisionObjectQueryParams Objects;
    for (const auto Channel:{ECC_WorldStatic,ECC_WorldDynamic,ECC_Pawn,ECC_Vehicle,ECC_PhysicsBody}) Objects.AddObjectTypesToQuery(Channel);
    FHitResult Obstacle;
    return !GetWorld()->LineTraceSingleByObjectType(Obstacle,GetActorLocation(),Player->GetActorLocation(),Objects,Params);
}
void ADeliveryCompanionCharacter::OnCombatDownChanged(FGuid DownEpisode)
{
    if (!HasAuthority()) return;
    const auto* OwnerState=Companion->GetOwnerPlayerState();
    const auto* PC=IsValid(OwnerState)?OwnerState->GetPlayerController():nullptr;
    auto* Player=PC?Cast<ADeliveryCharacter>(PC->GetPawn()):nullptr;
    // 新击倒和倒地重击都清掉旧按住时间，新一轮必须重新按住 E
    if (Player && Player->GetInteractionComponent()) Player->GetInteractionComponent()->CancelServerHold(this);
    UpdateHelpInteraction();
}
void ADeliveryCompanionCharacter::OnCombatRecoveryFinished(FGuid DownEpisode,APawn* Helper)
{
    UpdateHelpInteraction();
    if (HasAuthority() && IsValid(Helper)) Companion->RecordAssistedRecovery(DownEpisode,Helper);
}
void ADeliveryCompanionCharacter::CancelAssistedHelp()
{
    if (!HasAuthority() || !Combat) return;
    Combat->CancelAssistedRecovery(Combat->GetDownEpisodeId());
    OnCombatDownChanged(Combat->GetDownEpisodeId());
}
void ADeliveryCompanionCharacter::ReceiveMeleeHitFrom(const FVector& Impulse,const FVector& Point,AActor* Source,FGuid PunchId)
{
    if (HasAuthority() && PunchId.IsValid())
    {
        LastMeleePunchId=PunchId;
        bWasDownBeforeMelee=Super::IsKnockedDown();
    }
    Super::ReceiveMeleeHitFrom(Impulse,Point,Source,PunchId);
}
bool ADeliveryCompanionCharacter::HasPaceContext() const
{
    return HasAuthority() && Encounter.Phase==EDeliveryCompanionEncounterPhase::Joined &&
        Companion->GetMode()==EDeliveryCompanionMode::Stay && !IsKnockedDown() &&
        GetCharacterMovement()->IsMovingOnGround() && !GetHeldVehicle() && !Companion->IsUsingErrandVehicle() &&
        !Companion->HasActiveErrand() && !Companion->HasActiveHomeArrangement() && !Companion->GetHomeState().CarriedProp &&
        !Companion->GetCarriedItem() && !Companion->GetAssistTarget() &&
        !Combat->IsRecovering() && !Combat->IsReactingToHit() && !Combat->IsStriking();
}
float ADeliveryCompanionCharacter::GetPaceTravelBudget() const
{
    // 保留转身和停步时间，低速时缩短两段路程
    constexpr float TurnAndStopSeconds=1.25f;
    const float LegTime=(DeliveryNpc::CandidateDuration(EDeliveryNpcAction::Pace)-TurnAndStopSeconds)*.5f;
    return FMath::Min(Companion->GetWaitAreaRemaining()-12.f,PaceSpeed*LegTime);
}
bool ADeliveryCompanionCharacter::CanContinuePace(const FDeliveryNpcPublicState& State) const
{
    return State.Action==EDeliveryNpcAction::Pace && State.Origin==EDeliveryNpcActionOrigin::Model &&
        Companion->CanCommand(State.Requester) && State.CompanionCommandRevision==Companion->GetCommandRevision() &&
        HasPaceContext() && Companion->GetWaitAreaRemaining()>0.f;
}
bool ADeliveryCompanionCharacter::CanExecute(EDeliveryNpcAction Action,AActor* Target) const
{
    if (Action==EDeliveryNpcAction::JoinAssist)
        return Target==Companion->GetAssistTarget() && Companion->CanJoinAssist(nullptr);
    if (Action==EDeliveryNpcAction::StopAssist)
        return !Target && Companion->CanStopAssist(Companion->GetOwnerPlayerState());
    if (Action==EDeliveryNpcAction::Pace)
    {
        const auto* AI=Cast<AAIController>(GetController());
        return !Target && HasPaceContext() && AI && AI->GetMoveStatus()==EPathFollowingStatus::Idle &&
            GetVelocity().SizeSquared2D()<=FMath::Square(5.f) && Super::CanExecute(Action,nullptr);
    }
    if (Action==EDeliveryNpcAction::SoundFollowHorn)
    {
        const auto* Bike=Companion->GetFollowMotorbike();
        return Bike && Bike->CanSoundFollowHorn();
    }
    if (DeliveryNpc::IsCompanionItemErrand(Action))
        return Companion->CanArrangeItem(Action,Cast<ADeliveryHandheldItem>(Target),Companion->GetOwnerPlayerState());
    if (Action==EDeliveryNpcAction::HomeAction)
        return !IsKnockedDown() && !Companion->IsUsingErrandVehicle() && IsValid(Companion->GetOwnerPlayerState());
    if (Action==EDeliveryNpcAction::OfferCaterpillar) return !IsKnockedDown() && Companion->CanOfferCaterpillar(Companion->GetOwnerPlayerState());
    if (Action==EDeliveryNpcAction::GoHome) return !IsKnockedDown() && Companion->CanGoHome(Companion->GetOwnerPlayerState());
    if (DeliveryNpc::IsCompanionErrand(Action)) return !IsKnockedDown() && Companion->CanArrangeErrand(Action,Companion->GetOwnerPlayerState());
    if (DeliveryNpc::IsCompanionCommand(Action)) return !IsKnockedDown() && IsValid(Companion->GetOwnerPlayerState());
    if (Action==EDeliveryNpcAction::MoveAside && Companion->HasActiveHomeArrangement()) return false;
    if (const auto* Item=Cast<ADeliveryHandheldItem>(Target); DeliveryHandheldTransfer::IsOrdinaryItem(Item) &&
        !Companion->CanUseObservedItemTarget(Item)) return false;
    // 模型凭真实命中选择协战安排，具体 Fight 回合复用服务器批准的目标
    if (Action!=EDeliveryNpcAction::Wait && Action!=EDeliveryNpcAction::LookAt && Action!=EDeliveryNpcAction::MoveAside) return false;
    return Super::CanExecute(Action,Target);
}
bool ADeliveryCompanionCharacter::CanExecuteForRequester(EDeliveryNpcAction Action,AActor* Target,APlayerState* Requester) const
{
    if (Action==EDeliveryNpcAction::JoinAssist)
        return Target==Companion->GetAssistTarget() && Companion->CanJoinAssist(Requester);
    if (Action==EDeliveryNpcAction::StopAssist)
        return !Target && Companion->CanStopAssist(Requester);
    const bool bHasAction=Agent->HasActiveAction();
    const bool bPacing=bHasAction && Agent->GetPublicState().Action==EDeliveryNpcAction::Pace;
    if ((bPacing || Action==EDeliveryNpcAction::Pace) && !Companion->CanCommand(Requester)) return false;
    if (Action==EDeliveryNpcAction::Pace && bHasAction && !bPacing) return false;
    if (Action==EDeliveryNpcAction::SoundFollowHorn) return !Requester && CanExecute(Action,Target);
    if (Action==EDeliveryNpcAction::OfferCaterpillar) return CanExecute(Action,Target) && Companion->CanRequestCaterpillarOffer(Requester);
    if (DeliveryNpc::IsCompanionCommand(Action)) return CanExecute(Action,Target) && Companion->CanCommand(Requester);
    return CanExecute(Action,Target);
}
bool ADeliveryCompanionCharacter::CanExecuteGameplayRule(EDeliveryNpcAction Action,AActor* Target,int32 RuleRevision) const
{
    return HasAuthority() && Action==EDeliveryNpcAction::Fight && !IsKnockedDown() && !GetHeldVehicle() &&
        !Companion->IsUsingErrandVehicle() && !Companion->HasActiveHomeArrangement() && !Companion->GetHomeState().CarriedProp &&
        Companion->CanExecuteAssist(Target,RuleRevision);
}
float ADeliveryCompanionCharacter::GetGameplayRuleFightApproachRange() const
{
    return Companion->GetAssistRange();
}

void ADeliveryCompanionCharacter::ExtendDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const
{
    Companion->ExtendDecisionRequest(Request,Requester);
    Companion->ExtendHomeDecisionRequest(Request,Requester);
    Companion->ExtendItemDecisionRequest(Request,Requester);
    Companion->ExtendTravelDecisionRequest(Request,Requester);
    Request.CompanionAssistRevision=Companion->GetAssistRevision();
    Request.bCompanionAssistCommitted=Companion->HasCommittedAssist();
    Request.bCompanionAssistPending=Companion->CanJoinAssist(Companion->GetOwnerPlayerState());
    if ((!Request.bPlayerRequest || Request.bPendingPlayerMessage) && Companion->CanJoinAssist(Requester))
    {
        const FGuid EvidenceId=Companion->GetAssistEvidenceId();
        const auto* Fact=Request.Evidence.FindByPredicate([EvidenceId](const FDeliveryNpcEvidence& Entry)
        {
            return Entry.EventId==EvidenceId && Entry.Source==EDeliveryNpcEvidenceSource::ActionResult &&
                (Entry.Kind==TEXT("OwnerConfirmedAttack") || Entry.Kind==TEXT("OwnerConfirmedHit"));
        });
        if (Fact && Agent->FindKnownSubjectId(Companion->GetAssistTarget())==Fact->SubjectId)
        {
            FDeliveryNpcCandidate Candidate;
            Candidate.Action=EDeliveryNpcAction::JoinAssist;
            Candidate.TargetId=Fact->SubjectId;
            Candidate.EvidenceIds.Add(EvidenceId);
            Request.Candidates.Add(Candidate);
            Request.AllowedActions.AddUnique(Candidate.Action);
        }
    }
    if (Request.bPlayerRequest && Request.bPendingPlayerMessage && Companion->CanStopAssist(Requester))
    {
        FDeliveryNpcCandidate Candidate;
        Candidate.Action=EDeliveryNpcAction::StopAssist;
        Request.Candidates.Add(Candidate);
        Request.AllowedActions.AddUnique(Candidate.Action);
    }
    if (!Requester && CanExecute(EDeliveryNpcAction::SoundFollowHorn,nullptr))
    {
        FDeliveryNpcCandidate Candidate;
        Candidate.Action=EDeliveryNpcAction::SoundFollowHorn;
        Request.Candidates.Add(Candidate);
        Request.AllowedActions.AddUnique(Candidate.Action);
    }
}
bool ADeliveryCompanionCharacter::ExecuteAction(const FDeliveryNpcPublicState& State)
{
    if (State.Action==EDeliveryNpcAction::Pace) return CanContinuePace(State) && Super::ExecuteAction(State);
    if (!DeliveryNpc::IsCompanionCommand(State.Action) && Agent->CanContinueCompanionMovement() &&
        State.ActionId==Agent->GetPublicState().ActionId &&
        State.PlanRevision==Agent->GetPublicState().PlanRevision && State.Action==Agent->GetPublicState().Action &&
        State.Status==EDeliveryNpcActionStatus::Running && State.Target==Agent->GetPublicState().Target &&
        State.Requester==Agent->GetPublicState().Requester && State.Origin==EDeliveryNpcActionOrigin::Model)
    {
        if (!CanExecuteForRequester(State.Action,State.Target,State.Requester)) return false;
        if (State.Action==EDeliveryNpcAction::SoundFollowHorn)
        {
            auto* Bike=Companion->GetFollowMotorbike();
            if (!Bike || !Bike->TrySoundFollowHorn()) return false;
        }
        PassiveActionId=State.ActionId;
        return State.bStreamSpeech || State.Speech.IsEmpty() || Presentation->PublishSpeech(
            State.Speech,State.Emotion,Agent->GetActionConversationDepth(),State.ReplyToEventId,State.GetReplyId());
    }
    if (!DeliveryNpc::IsCompanionCommand(State.Action))
        return Companion->EnsureOnFoot() && Super::ExecuteAction(State);
    const bool bItemErrand=DeliveryNpc::IsCompanionItemErrand(State.Action);
    if (!HasAuthority() || (!bItemErrand && !CanExecuteForRequester(State.Action,State.Target,State.Requester))) return false;
    const bool bAccepted=State.Action==EDeliveryNpcAction::JoinAssist?
        Companion->JoinAssist(State.Requester,State.CompanionAssistRevision):
        State.Action==EDeliveryNpcAction::StopAssist?
        Companion->StopAssist(State.Requester,State.CompanionAssistRevision):bItemErrand?
        State.ItemChoice.IsValid() && State.ItemChoice->Item.Get()==State.Target &&
        State.ItemChoice->GetAction()==State.Action &&
        Companion->ApplyItemChoice(*State.ItemChoice,State.Requester,State.CompanionCommandRevision,State.CompanionTravelMode):
        State.Action==EDeliveryNpcAction::HomeAction?
        State.HomeChoice.IsValid() && Companion->ApplyHomeChoice(*State.HomeChoice,State.Requester,State.CompanionCommandRevision):
        Companion->ApplyCommand(State.Action,State.Requester,State.CompanionCommandRevision,State.CompanionTask,State.CompanionPackage,State.CompanionTravelMode);
    if (!bAccepted) return false;
    if (!State.bStreamSpeech && !State.Speech.IsEmpty())
        Presentation->PublishSpeech(State.Speech,State.Emotion,Agent->GetActionConversationDepth(),State.ReplyToEventId,State.GetReplyId());
    return true;
}
void ADeliveryCompanionCharacter::StopAction(FGuid ActionId)
{
    if (ActionId.IsValid() && ActionId==PassiveActionId)
    { PassiveActionId.Invalidate(); return; }
    Super::StopAction(ActionId);
}
void ADeliveryCompanionCharacter::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (HasAuthority()) UpdateEncounter();
    else if (Encounter.Phase==EDeliveryCompanionEncounterPhase::Ejecting) ApplyEjectionMotion();
    UpdateEncounterPresentation();
    if (HasAuthority() && Encounter.Phase==EDeliveryCompanionEncounterPhase::Joined && Combat->IsDown())
    {
        const auto* OwnerState=Companion->GetOwnerPlayerState();
        const auto* PC=IsValid(OwnerState)?OwnerState->GetPlayerController():nullptr;
        // 长按期间离开或被挡住也撤销服务器旧记录，回来后不能续用原来的按住时间
        if (!CanContinueAssistedHelp(PC?PC->GetPawn():nullptr,Combat->GetDownEpisodeId()))
            OnCombatDownChanged(Combat->GetDownEpisodeId());
    }
    UpdateHelpInteraction();
    // 认了主人之后倒地也能被叫住，初遇倒在地上时仍只开放扶起
    ConversationInteractable->bInteractEnabled=!IsKnockedDown() || GetEncounterState().Phase==EDeliveryCompanionEncounterPhase::Joined;
}
void ADeliveryCompanionCharacter::UpdateHelpInteraction()
{
    const bool bJoined=Encounter.Phase==EDeliveryCompanionEncounterPhase::Joined;
    const bool bCombatDown=bJoined && Combat->IsDown();
    // 同一个 E 入口先处理倒地扶起，站立后才允许接过已提议的小绿
    HelpInteractable->HoldDuration=bJoined && !bCombatDown?0.f:FMath::Clamp(Companion->GetTuning().HelpHoldSeconds,.3f,3.f);
    HelpInteractable->PromptText=bJoined && !bCombatDown?NSLOCTEXT("Delivery","TakeXiaolv","按 E 接过小绿"):
        NSLOCTEXT("Delivery","HelpErgou","按住 E 扶起二狗");
    HelpInteractable->bInteractEnabled=bJoined?(bCombatDown?Combat->CanStartAssistedRecovery():Companion->HasCaterpillarOffer()):
        IsWaitingForHelp() && HasFinishedEncounterFall();
}
void ADeliveryCompanionCharacter::EndPlay(const EEndPlayReason::Type Reason)
{
    CancelAssistedHelp();
    Combat->CanContinueAssistedRecovery.Unbind();
    Combat->OnDownChanged.RemoveAll(this); Combat->OnRecoveryFinished.RemoveAll(this);
    RemoveEjectionMotion();
    if (IsValid(EncounterScene))
    {
        EncounterScene->UpdateBossPresentation(-1,false);
        if (HasAuthority())
        {
            EncounterScene->SetEncounterObstruction(EDeliveryCompanionEncounterObstruction::None);
            if (Reason==EEndPlayReason::Destroyed) EncounterScene->RequestDoorOpen(false);
        }
    }
    HelpInteractable->CanUse.Unbind(); HelpInteractable->Use.Unbind();
    Super::EndPlay(Reason);
}
void ADeliveryCompanionCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ADeliveryCompanionCharacter,Encounter);
    DOREPLIFETIME(ADeliveryCompanionCharacter,EncounterScene);
}
