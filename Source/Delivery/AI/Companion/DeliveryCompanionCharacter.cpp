#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Companion/DeliveryCompanionEncounterScene.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Conversation/DeliveryNpcConversationTrace.h"
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
#include "GameFramework/GameStateBase.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "Animation/AnimSequence.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Vehicle/DeliveryCompanionMotorbike.h"

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
    Combat->bReceiveHitsWhileDown=true;
    Combat->KnockdownHorizontalSpeed=600;
    Combat->KnockdownUpwardSpeed=250;
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
bool ADeliveryCompanionCharacter::CanExecute(EDeliveryNpcAction Action,AActor* Target) const
{
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
    // 模型保留陪伴安排，护主交战只能由服务器真实命中建立的规则授权
    if (Action!=EDeliveryNpcAction::Wait && Action!=EDeliveryNpcAction::LookAt && Action!=EDeliveryNpcAction::MoveAside) return false;
    return Super::CanExecute(Action,Target);
}
bool ADeliveryCompanionCharacter::CanExecuteForRequester(EDeliveryNpcAction Action,AActor* Target,APlayerState* Requester) const
{
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
void ADeliveryCompanionCharacter::ExtendDecisionRequest(FDeliveryNpcDecisionRequest& Request,APlayerState* Requester) const
{
    Companion->ExtendDecisionRequest(Request,Requester);
    Companion->ExtendHomeDecisionRequest(Request,Requester);
    Companion->ExtendItemDecisionRequest(Request,Requester);
    Companion->ExtendTravelDecisionRequest(Request,Requester);
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
    if (Agent->CanContinueCompanionMovement() && State.ActionId==Agent->GetPublicState().ActionId &&
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
    if (!HasAuthority() || !CanExecuteForRequester(State.Action,State.Target,State.Requester)) return false;
    const bool bAccepted=DeliveryNpc::IsCompanionItemErrand(State.Action)?
        State.ItemChoice.IsValid() && Companion->ApplyItemChoice(*State.ItemChoice,State.Requester,State.CompanionCommandRevision,State.CompanionTravelMode):
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
    ConversationInteractable->bInteractEnabled=!IsKnockedDown();
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
        if (HasAuthority() && Reason==EEndPlayReason::Destroyed) EncounterScene->RequestDoorOpen(false);
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
