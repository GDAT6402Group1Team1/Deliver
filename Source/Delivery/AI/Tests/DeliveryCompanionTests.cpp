#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/BehaviorTree/BTTask_DeliveryExecuteNpcAction.h"
#include "AI/Characters/DeliveryNpcAIController.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionEncounterScene.h"
#include "AI/Companion/DeliveryCompanionTuning.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AIController.h"
#include "Animation/AnimSequence.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "NavLinkCustomComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationData.h"
#include "NPC/DeliveryGrandpa.h"
#include "Player/DeliveryPlayerController.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/Data/DeliveryTaskUnlockCondition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/World/DeliveryBreakableDoor.h"
#include "Task/World/DeliveryPickupPoint.h"
#include "Task/World/DeliveryPickupPointComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Vehicle/DeliveryCompanionMotorbike.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Task/World/DeliveryItemComponent.h"
#include "World/DeliveryHome.h"
#include "World/DeliveryLocationComponent.h"
#include "World/DeliveryLocationRegistry.h"
#include "Net/UnrealNetwork.h"
#include "UObject/UnrealType.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryCompanionRulesTest,"Delivery.AI.Companion.OwnershipAndCommands",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryCompanionRulesTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool Enabled=Settings->bEnabled;
    Settings->bEnabled=false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
        Settings->bEnabled=Enabled;
    };
    const auto MakePawn=[&](const FVector& Location)
    {
        auto* Pawn=World->SpawnActor<APawn>();
        auto* Root=NewObject<USceneComponent>(Pawn); Pawn->SetRootComponent(Root); Root->RegisterComponent();
        Pawn->SetActorLocation(Location);
        return Pawn;
    };
    const auto* StartRagdoll=FindFProperty<FBoolProperty>(UDeliveryActiveRagdollComponent::StaticClass(),TEXT("bStartOnBeginPlay"));
    if (!TestNotNull(TEXT("原生玩家夹具可配置开局物理驱动"),StartRagdoll)) return false;
    const auto MakeWalkingCharacter=[&](const FVector& Location) -> ADeliveryCharacter*
    {
        const FTransform SpawnTransform(Location);
        auto* Character=World->SpawnActorDeferred<ADeliveryCharacter>(ADeliveryCharacter::StaticClass(),SpawnTransform,
            nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if (!Character) return nullptr;
        // 门口和持件夹具只用原生胶囊和交接接口
        StartRagdoll->SetPropertyValue_InContainer(Character->GetActiveRagdoll(),false);
        Character->FinishSpawning(SpawnTransform);
        return Character;
    };
    const auto MakePlayer=[&](const FVector& Location)
    {
        auto* PC=World->SpawnActor<ADeliveryPlayerController>();
        auto* PS=World->SpawnActor<APlayerState>(); PS->SetOwner(PC); PC->PlayerState=PS;
        PC->Possess(MakePawn(Location));
        return PC;
    };
    auto* Owner=MakePlayer(FVector(100,0,88));
    auto* Stranger=MakePlayer(FVector(120,0,88));
    auto* Npc=World->SpawnActor<ADeliveryCompanionCharacter>(FVector(0,0,88),FRotator::ZeroRotator);
    TestEqual(TEXT("隐藏等待期间关闭移动，避免无碰撞下落"),Npc->GetCharacterMovement()->MovementMode.GetValue(),MOVE_None);
    Npc->Encounter.EncounterStartedAt=0;
    Npc->Encounter.FallAnimationStartedAt=0;
    Npc->Encounter.Phase=EDeliveryCompanionEncounterPhase::Ejecting;
    TestFalse(TEXT("抛出期间即使贴近也不能提前扶起"),Npc->Companion->CanRecruit(Owner->GetPawn()));
    Npc->Encounter.Phase=EDeliveryCompanionEncounterPhase::Falling;
    TestFalse(TEXT("轨迹结束但尚未落地不能认主"),Npc->Companion->CanRecruit(Owner->GetPawn()));
    Npc->Encounter.Phase=EDeliveryCompanionEncounterPhase::AwaitingHelp;
    Npc->OnRep_Encounter();
    TestEqual(TEXT("初遇后恢复正常移动模式"),Npc->GetCharacterMovement()->MovementMode.GetValue(),MOVE_Walking);
    TestTrue(TEXT("通路无遮挡时允许扶起"),Npc->Companion->CanRecruit(Owner->GetPawn()));
    // 使用车辆通道的 Pawn 查询体，防止扶起误用忽略所有 Pawn 的交谈射线
    auto* VehicleBlocker=World->SpawnActor<APawn>();
    auto* VehicleBox=NewObject<UBoxComponent>(VehicleBlocker);
    VehicleBlocker->SetRootComponent(VehicleBox);
    VehicleBox->SetBoxExtent(FVector(8,30,100));
    VehicleBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    VehicleBox->SetCollisionObjectType(ECC_Vehicle);
    VehicleBox->SetCollisionResponseToAllChannels(ECR_Block);
    VehicleBox->RegisterComponent();
    VehicleBlocker->SetActorLocation(FVector(50,0,88));
    FHitResult VehicleHit;
    TestTrue(TEXT("测试车辆查询体实际挡住双方射线"),World->LineTraceSingleByObjectType(VehicleHit,
        Npc->GetActorLocation(),Owner->GetPawn()->GetActorLocation(),FCollisionObjectQueryParams(ECC_Vehicle)));
    TestEqual(TEXT("射线命中本次车辆查询体"),VehicleHit.GetActor(),static_cast<AActor*>(VehicleBlocker));
    TestFalse(TEXT("隔着车辆不能扶起并写入主人"),Npc->Companion->TryRecruit(Owner->GetPawn()));
    TestNull(TEXT("被车辆阻挡后仍无主人"),Npc->Companion->GetOwnerPlayerState());
    VehicleBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    TestTrue(TEXT("服务器扶起后归属写入"),Npc->Companion->TryRecruit(Owner->GetPawn()));
    TestEqual(TEXT("主人绑定PlayerState"),Npc->Companion->GetOwnerPlayerState(),Owner->PlayerState.Get());
    TestFalse(TEXT("重复扶起不能重新加入"),Npc->Companion->TryRecruit(Owner->GetPawn()));
    TestFalse(TEXT("其他玩家不能抢改主人"),Npc->Companion->TryRecruit(Stranger->GetPawn()));
    Npc->Encounter.Phase=EDeliveryCompanionEncounterPhase::Joined;
    const int32 Revision=Npc->Companion->GetCommandRevision();
    TestFalse(TEXT("非主人不能安排等候"),Npc->Companion->ApplyCommand(EDeliveryNpcAction::Stay,Stranger->PlayerState,Revision));
    TestTrue(TEXT("主人可以安排等候"),Npc->Companion->ApplyCommand(EDeliveryNpcAction::Stay,Owner->PlayerState,Revision));
    TestFalse(TEXT("旧指令编号不能覆盖等候"),Npc->Companion->ApplyCommand(EDeliveryNpcAction::Follow,Owner->PlayerState,Revision));
    auto* Replacement=MakePawn(FVector(100,0,88));
    Owner->Possess(Replacement);
    TestEqual(TEXT("切换Pawn保留主人"),Npc->Companion->GetOwnerPlayerState(),Owner->PlayerState.Get());
    TestEqual(TEXT("切换Pawn不解除手动等候"),Npc->Companion->GetMode(),EDeliveryCompanionMode::Stay);
    Npc->Agent->BeginListening(Replacement,FGuid::NewGuid());
    FDeliveryNpcDecisionRequest Request;
    TestTrue(TEXT("跟随之外仍能请求模型"),Npc->Agent->BuildDecisionRequest(FGuid::NewGuid(),Request));
    TestTrue(TEXT("主人请求获得持续模式候选"),Request.AllowedActions.Contains(EDeliveryNpcAction::Follow));
    TestTrue(TEXT("上下文记录主人权限"),Request.bRequesterIsCompanionOwner);
    Npc->Agent->ActiveRequest=Request;
    FDeliveryNpcDecisionProposal Proposal;
    const auto* FollowCandidate=Request.Candidates.FindByPredicate([](const auto& Candidate)
        { return Candidate.Action==EDeliveryNpcAction::Follow && !Candidate.TargetId.IsValid(); });
    if (!TestNotNull(TEXT("跟随候选实际存在"),FollowCandidate)) return false;
    Proposal.CapabilityVersion=Request.RequestedCapabilityVersion;
    Proposal.CandidateId=FollowCandidate->CandidateId;
    Proposal.TargetId=FollowCandidate->TargetId;
    Proposal.EvidenceIds=FollowCandidate->EvidenceIds;
    Proposal.WorldEpoch=Request.WorldEpoch; Proposal.NpcId=Request.NpcId; Proposal.RequestId=Request.RequestId;
    Proposal.ContextRevision=Request.ContextRevision; Proposal.PlanRevision=Request.PlanRevision;
    Proposal.Action=EDeliveryNpcAction::Follow; Proposal.Duration=0; Proposal.Speech=TEXT("我跟着你。");
    TestTrue(TEXT("请求期间主人取消旧安排"),Npc->Companion->ApplyCommand(EDeliveryNpcAction::CancelCompanionTask,Owner->PlayerState,Request.CompanionCommandRevision));
    FName Reason;
    TestFalse(TEXT("迟到模型不能恢复已取消的跟随"),Npc->Agent->TryAcceptProposal(Proposal,Reason));
    TestEqual(TEXT("因持续命令编号变化拒绝"),Reason,FName(TEXT("StaleCompanionCommand")));
    TestFalse(TEXT("主人请求尚未结束时访客不能覆盖"),Npc->Agent->BeginListening(Stranger->GetPawn(),FGuid::NewGuid()));
    Npc->Agent->InterruptPlayerReply(Owner);
    TestTrue(TEXT("主人请求结束后访客可以聊天"),Npc->Agent->BeginListening(Stranger->GetPawn(),FGuid::NewGuid()));
    TestTrue(TEXT("其他玩家仍可聊天"),Npc->Agent->BuildDecisionRequest(FGuid::NewGuid(),Request));
    TestFalse(TEXT("其他玩家没有跟随候选"),Request.AllowedActions.Contains(EDeliveryNpcAction::Follow));
    TestFalse(TEXT("其他玩家没有等候候选"),Request.AllowedActions.Contains(EDeliveryNpcAction::Stay));
    TestFalse(TEXT("其他玩家没有取消候选"),Request.AllowedActions.Contains(EDeliveryNpcAction::CancelCompanionTask));
    Npc->Agent->CancelPlan(TEXT("TestFinished"));

    {
        auto* SocialAgent=Npc->Agent.Get();
        auto* SocialCompanion=Npc->Companion.Get();
        const int32 ArrangementRevision=SocialCompanion->GetCommandRevision();
        const auto BuildFromSpeech=[&](APlayerController* Speaker,const TCHAR* Text,FDeliveryNpcDecisionRequest& Out)
        {
            const FGuid MessageId=FGuid::NewGuid();
            if (!SocialAgent->BeginListening(Speaker->GetPawn(),MessageId)) return false;
            FDeliveryNpcObservation Speech;
            Speech.EventId=MessageId;
            Speech.Subject=Speaker->GetPawn();
            Speech.Addressee=Npc;
            Speech.Kind=TEXT("PlayerSpeech");
            Speech.Source=EDeliveryNpcEvidenceSource::Heard;
            Speech.Description=Text;
            return SocialAgent->ObserveEvent(Speech) && SocialAgent->BuildDecisionRequest(FGuid::NewGuid(),Out);
        };
        const auto AcceptPreference=[&](const FDeliveryNpcDecisionRequest& Context,EDeliveryCompanionSpeechPreference Preference)
        {
            const auto* Option=Context.Candidates.FindByPredicate([Preference](const auto& Candidate)
                { return Candidate.Action==EDeliveryNpcAction::ContinueCurrent && Candidate.SpeechPreference==Preference; });
            if (!Option) return false;
            FDeliveryNpcDecisionProposal Proposal;
            Proposal.CapabilityVersion=Context.RequestedCapabilityVersion;
            Proposal.WorldEpoch=Context.WorldEpoch;
            Proposal.NpcId=Context.NpcId;
            Proposal.RequestId=Context.RequestId;
            Proposal.ContextRevision=Context.ContextRevision;
            Proposal.PlanRevision=Context.PlanRevision;
            Proposal.CandidateId=Option->CandidateId;
            Proposal.Action=Option->Action;
            Proposal.Speech=TEXT("好，听你的。");
            SocialAgent->ActiveRequest=Context;
            FName Rejection;
            return SocialAgent->TryAcceptProposal(Proposal,Rejection);
        };
        FDeliveryNpcDecisionRequest QuietRequest;
        if (!TestTrue(TEXT("主人直接发话可生成选择安静的请求"),BuildFromSpeech(Owner,TEXT("先少说两句"),QuietRequest))) return false;
        TestTrue(TEXT("原候选选中安静后由服务器保存期限"),AcceptPreference(QuietRequest,EDeliveryCompanionSpeechPreference::Quiet));
        TestTrue(TEXT("当前有安静期限"),SocialCompanion->GetQuietSecondsRemaining()>0);
        TestEqual(TEXT("安静不作废原移动安排"),SocialCompanion->GetCommandRevision(),ArrangementRevision);
        SocialAgent->CancelPlan(TEXT("QuietReplyFinished"));
        TestFalse(TEXT("安静时环境闲聊不发起请求"),SocialAgent->BuildDecisionRequest(FGuid::NewGuid(),QuietRequest));
        if (!TestTrue(TEXT("安静时仍能回答直接提问"),BuildFromSpeech(Stranger,TEXT("你叫什么"),QuietRequest))) return false;
        TestFalse(TEXT("访客没有更改安静或恢复的候选"),QuietRequest.Candidates.ContainsByPredicate([](const auto& Candidate)
            { return Candidate.SpeechPreference!=EDeliveryCompanionSpeechPreference::Unspecified; }));
        TestFalse(TEXT("访客也不能直接通过规则入口恢复"),SocialCompanion->ApplySpeechPreference(Stranger->PlayerState,
            ArrangementRevision,EDeliveryCompanionSpeechPreference::Normal));
        SocialAgent->CancelPlan(TEXT("QuietVisitorFinished"));
        if (!TestTrue(TEXT("主人可以要求恢复闲聊"),BuildFromSpeech(Owner,TEXT("现在可以聊天了"),QuietRequest))) return false;
        TestTrue(TEXT("选择恢复后立即清除期限"),AcceptPreference(QuietRequest,EDeliveryCompanionSpeechPreference::Normal));
        TestEqual(TEXT("恢复后安静剩余为零"),SocialCompanion->GetQuietSecondsRemaining(),0);
        SocialAgent->CancelPlan(TEXT("QuietTestFinished"));
    }

    // 被动短动作沿用 Stay/Follow，只有真实移动或身体受阻才让位
    auto* Agent=Npc->Agent.Get();
    for (const auto Passive:{EDeliveryNpcAction::Wait,EDeliveryNpcAction::LookAt})
    {
        Agent->PublicState={}; Agent->PublicState.ActionId=FGuid::NewGuid();
        Agent->PublicState.Action=Passive; Agent->PublicState.Status=EDeliveryNpcActionStatus::Running;
        TestTrue(TEXT("普通 Wait/LookAt 不停止原等候安排"),Agent->CanContinueCompanionMovement());
        Agent->PublicState.Origin=EDeliveryNpcActionOrigin::GameplayRule;
        TestFalse(TEXT("服务器身体规则不冒充被动交谈"),Agent->CanContinueCompanionMovement());
        Agent->PublicState.Origin=EDeliveryNpcActionOrigin::Model;
        Npc->Encounter.Phase=EDeliveryCompanionEncounterPhase::AwaitingHelp;
        TestFalse(TEXT("倒地时短动作不能继续导航"),Agent->CanContinueCompanionMovement());
        Npc->Encounter.Phase=EDeliveryCompanionEncounterPhase::Joined;
    }
    Agent->PublicState.Action=EDeliveryNpcAction::MoveAside;
    TestFalse(TEXT("真实让路仍独占短动作移动"),Agent->CanContinueCompanionMovement());
    Agent->PublicState={};

    // 这个测试世界故意没有导航，验证踱步的权限、边界和召回换动作
    FDeliveryNpcPublicState Pace;
    Pace.Action=EDeliveryNpcAction::Pace;
    Pace.ActionId=FGuid::NewGuid();
    Pace.Status=EDeliveryNpcActionStatus::Running;
    Pace.Requester=Owner->PlayerState;
    Pace.CompanionCommandRevision=Npc->Companion->GetCommandRevision();
    TestTrue(TEXT("徒步等候时允许已批准的主人踱步继续"),Npc->CanContinuePace(Pace));
    Npc->Companion->VehicleTravelPurpose=EDeliveryCompanionTravelPurpose::Follow;
    TestTrue(TEXT("等候已落地时之前的跟车用途不会拦住踱步"),Npc->CanContinuePace(Pace));
    Npc->Companion->VehicleTravelPurpose=EDeliveryCompanionTravelPurpose::ReturnToStay;
    TestFalse(TEXT("返回等候点途中不能踱步"),Npc->CanContinuePace(Pace));
    Npc->Companion->VehicleTravelPurpose=EDeliveryCompanionTravelPurpose::None;
    const float NormalPaceSpeed=Npc->PaceSpeed;
    Npc->PaceSpeed=30.f;
    TestTrue(TEXT("慢速踱步的往返预算缩短并留有转身时间"),Npc->GetPaceTravelBudget()>40.f &&
        2.f*Npc->GetPaceTravelBudget()/Npc->PaceSpeed<DeliveryNpc::CandidateDuration(EDeliveryNpcAction::Pace));
    Npc->PaceSpeed=NormalPaceSpeed;
    TestFalse(TEXT("没有导航时不提供踱步"),Npc->CanExecuteForRequester(EDeliveryNpcAction::Pace,nullptr,Owner->PlayerState));
    const FVector WaitOrigin=Npc->GetActorLocation();
    Npc->SetActorLocation(WaitOrigin+FVector(1000,0,0));
    TestFalse(TEXT("走出原等候范围立即结束踱步"),Npc->CanContinuePace(Pace));
    Npc->SetActorLocation(WaitOrigin);
    Npc->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
    TestFalse(TEXT("离地不能继续踱步"),Npc->CanContinuePace(Pace));
    Npc->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
    --Pace.CompanionCommandRevision;
    TestFalse(TEXT("旧等候安排下的踱步不能继续"),Npc->CanContinuePace(Pace));
    ++Pace.CompanionCommandRevision;
    Agent->PublicState=Pace;
    TestFalse(TEXT("踱步独占短动作移动"),Agent->CanContinueCompanionMovement());
    TestFalse(TEXT("访客不能用等待打断踱步"),Npc->CanExecuteForRequester(EDeliveryNpcAction::Wait,nullptr,Stranger->PlayerState));
    TestFalse(TEXT("访客不能重新安排踱步"),Npc->CanExecuteForRequester(EDeliveryNpcAction::Pace,nullptr,Stranger->PlayerState));
    TestTrue(TEXT("访客踱步期间仍能开始交谈"),Agent->BeginListening(Stranger->GetPawn(),FGuid::NewGuid()));
    FDeliveryNpcDecisionRequest PaceRequest;
    if (!TestTrue(TEXT("访客收到原踱步的对话快照"),Agent->BuildDecisionRequest(FGuid::NewGuid(),PaceRequest))) return false;
    TestTrue(TEXT("访客在踱步期间只能接话和继续原安排"),PaceRequest.Candidates.Num()==1 &&
        PaceRequest.Candidates[0].Action==EDeliveryNpcAction::ContinueCurrent &&
        PaceRequest.AllowedActions.Num()==1 && PaceRequest.AllowedActions[0]==EDeliveryNpcAction::ContinueCurrent);
    Agent->ActiveRequest=PaceRequest;
    TestTrue(TEXT("访客聊天期间主人仍可发出召回"),Agent->BeginListening(Owner->GetPawn(),FGuid::NewGuid()));
    TestFalse(TEXT("主人接过对话时取消旧访客请求"),Agent->ActiveRequest.RequestId.IsValid());
    if (!TestTrue(TEXT("主人请求携带正在踱步的编号"),Agent->BuildDecisionRequest(FGuid::NewGuid(),PaceRequest))) return false;
    TestEqual(TEXT("正在踱步的编号用于召回校验"),PaceRequest.RunningActionId,Pace.ActionId);
    const auto* PaceRecallCandidate=PaceRequest.Candidates.FindByPredicate([](const FDeliveryNpcCandidate& Candidate)
        { return Candidate.Action==EDeliveryNpcAction::Follow; });
    if (!TestNotNull(TEXT("主人踱步期间仍有跟随候选"),PaceRecallCandidate)) return false;
    FDeliveryNpcDecisionProposal RecallProposal;
    RecallProposal.CapabilityVersion=PaceRequest.RequestedCapabilityVersion;
    RecallProposal.WorldEpoch=PaceRequest.WorldEpoch; RecallProposal.NpcId=PaceRequest.NpcId;
    RecallProposal.RequestId=PaceRequest.RequestId; RecallProposal.ContextRevision=PaceRequest.ContextRevision;
    RecallProposal.PlanRevision=PaceRequest.PlanRevision; RecallProposal.CandidateId=PaceRecallCandidate->CandidateId;
    RecallProposal.Action=PaceRecallCandidate->Action; RecallProposal.Speech=TEXT("我跟过来。");
    Agent->ActiveRequest=PaceRequest;
    TestTrue(TEXT("主人召回的模型结果可以抢占踱步"),Agent->TryAcceptProposal(RecallProposal,Reason));
    TestEqual(TEXT("旧踱步已经让位于待执行跟随"),Agent->GetPublicState().Action,EDeliveryNpcAction::Follow);
    TestFalse(TEXT("旧踱步回调不能结束新动作"),Agent->TryFinishAction(Pace.ActionId,EDeliveryNpcActionStatus::Succeeded,NAME_None));
    TestEqual(TEXT("模型采纳时仍保留原等候，待执行后才修改"),Npc->Companion->GetMode(),EDeliveryCompanionMode::Stay);
    Agent->CancelPlan(TEXT("PaceTestFinished"));

    Pace.ActionId=FGuid::NewGuid();
    Agent->PublicState=Pace;
    const FGuid PendingRecall=FGuid::NewGuid();
    TestTrue(TEXT("主人在回程受阻前发出新请求"),Agent->BeginListening(Owner->GetPawn(),PendingRecall));
    if (!TestTrue(TEXT("将原踱步快照绑定到本轮请求"),Agent->BuildDecisionRequest(FGuid::NewGuid(),PaceRequest))) return false;
    Agent->ActiveRequest=PaceRequest;
    TestTrue(TEXT("回程受阻按失败结算"),Agent->TryFinishAction(Pace.ActionId,EDeliveryNpcActionStatus::Failed,TEXT("PaceReturnBlocked")));
    TestFalse(TEXT("失效的踱步快照撤下"),Agent->ActiveRequest.RequestId.IsValid());
    TestEqual(TEXT("回程受阻后主人请求继续等待新的判断"),Agent->PlayerReplyMessage,PendingRecall);
    Agent->CancelPlan(TEXT("PaceRetried"));

    // 获救回应绑定真实事件和主人，不伪造玩家发言，也不把模型文字当播放完成
    FDeliveryNpcObservation Joined;
    Joined.EventId=FGuid::NewGuid(); Joined.Kind=TEXT("CompanionJoined");
    Joined.Subject=Owner->GetPawn(); Joined.Source=EDeliveryNpcEvidenceSource::ActionResult;
    Joined.Description=TEXT("玩家刚才亲手扶我起身，我已认他为主人"); Joined.Lifetime=60;
    TestTrue(TEXT("记录已认主的真实救助结果"),Agent->ObserveEvent(Joined));
    TestTrue(TEXT("真实事件进入主动回复上下文"),Agent->BuildDecisionRequest(FGuid::NewGuid(),Request));
    TestFalse(TEXT("主动获救回应没有伪造玩家输入"),Request.bPlayerRequest);
    TestEqual(TEXT("请求绑定原获救事件"),Agent->GetJoinedResponseEvent(Request),Joined.EventId);
    Agent->ActiveRequest=Request;
    const auto* WaitCandidate=Request.Candidates.FindByPredicate([](const auto& Candidate)
        { return Candidate.Action==EDeliveryNpcAction::Wait; });
    if (!TestNotNull(TEXT("获救后仍由模型选择实际候选"),WaitCandidate)) return false;
    FDeliveryNpcDecisionProposal JoiningReply;
    JoiningReply.CapabilityVersion=Request.RequestedCapabilityVersion;
    JoiningReply.WorldEpoch=Request.WorldEpoch; JoiningReply.NpcId=Request.NpcId; JoiningReply.RequestId=Request.RequestId;
    JoiningReply.ContextRevision=Request.ContextRevision; JoiningReply.PlanRevision=Request.PlanRevision;
    JoiningReply.CandidateId=WaitCandidate->CandidateId; JoiningReply.Action=WaitCandidate->Action;
    JoiningReply.TargetId=WaitCandidate->TargetId; JoiningReply.EvidenceIds=WaitCandidate->EvidenceIds; JoiningReply.Duration=1;
    TestFalse(TEXT("静默 Wait 不能消耗获救回应"),Agent->TryAcceptProposal(JoiningReply,Reason));
    TestEqual(TEXT("获救静默失败可解释"),Reason,FName(TEXT("MissingJoinedReply")));
    const FGuid NewPlayerMessage=FGuid::NewGuid();
    TestTrue(TEXT("新玩家发言优先于旧获救请求"),Agent->BeginListening(Owner->GetPawn(),NewPlayerMessage));
    TestFalse(TEXT("玩家插话撤下旧初遇请求"),Agent->ActiveRequest.RequestId.IsValid());
    TestEqual(TEXT("玩家插话保留未说完的获救经历"),Agent->PendingJoinedEvent,Joined.EventId);
    TestEqual(TEXT("新玩家发言编号保持独立"),Agent->PlayerReplyMessage,NewPlayerMessage);
    Agent->InterruptPlayerReply(Owner);
    Agent->ActiveRequest=Request; Agent->JoinedResponseAttempts=3;
    Agent->ReceiveDecision(Request.RequestId,nullptr,TEXT("TransportFailure"));
    TestFalse(TEXT("第三次失败不叠加通用自动重试"),Npc->IsDecisionRetryScheduled());
    TestEqual(TEXT("重试用尽仍未冒充已回应"),Agent->PendingJoinedEvent,Joined.EventId);
    UDeliveryNpcAgentComponent::FReply Playback;
    Playback.ReplyId=FGuid::NewGuid(); Playback.MessageId=Joined.EventId; Playback.JoinedEventId=Joined.EventId;
    Playback.JoinedOwner=Owner->PlayerState; Playback.SentenceCount=1;
    Agent->ActiveRequest={}; Agent->Replies.Add(FGuid::NewGuid(),Playback);
    Agent->FinishReplyPlayback(Playback.ReplyId,FGuid::NewGuid());
    TestEqual(TEXT("仅完成回调且没有真实 SelfSpeech 不算说完"),Agent->PendingJoinedEvent,Joined.EventId);
    Playback.ReplyId=FGuid::NewGuid(); Agent->Replies.Add(FGuid::NewGuid(),Playback);
    FDeliveryNpcObservation Spoken;
    Spoken.EventId=FGuid::NewGuid();
    Spoken.Kind=TEXT("SelfSpeech"); Spoken.Subject=Npc; Spoken.Addressee=Owner->GetPawn();
    Spoken.Source=EDeliveryNpcEvidenceSource::ActionResult; Spoken.ReplyToEventId=Joined.EventId;
    Spoken.Description=TEXT("这句话只作为测试中的模型回复"); Spoken.Lifetime=60;
    TestTrue(TEXT("表现完成才记录真实说话结果"),Agent->ObserveEvent(Spoken));
    Agent->FinishReplyPlayback(Playback.ReplyId,Spoken.EventId);
    TestFalse(TEXT("对应整轮播放完成后撤下初遇待答"),Agent->PendingJoinedEvent.IsValid());

    // 主人插话后的回复保留自己的主消息，同时带上未回应救助的独立关联
    Joined.EventId=FGuid::NewGuid();
    TestTrue(TEXT("第二个独立救助用例记录成功"),Agent->ObserveEvent(Joined));
    const FGuid OwnerQuestion=FGuid::NewGuid();
    TestTrue(TEXT("主人新问题优先处理"),Agent->BeginListening(Owner->GetPawn(),OwnerQuestion));
    FDeliveryNpcDecisionRequest PlayerRequest;
    TestTrue(TEXT("玩家请求仍带真实救助经历"),Agent->BuildDecisionRequest(FGuid::NewGuid(),PlayerRequest));
    TestEqual(TEXT("只给主人回应关联救助"),Agent->GetJoinedResponseEvent(PlayerRequest),Joined.EventId);
    auto StrangerRequest=PlayerRequest;
    StrangerRequest.RequesterPC=Stranger; StrangerRequest.RequesterPlayerState=Stranger->PlayerState;
    TestFalse(TEXT("旁人回复不会结清主人的救助回应"),Agent->GetJoinedResponseEvent(StrangerRequest).IsValid());
    Agent->PublicState.SpeechTarget=Owner->GetPawn();
    UDeliveryNpcAgentComponent::FReply OwnerReply;
    OwnerReply.ReplyId=FGuid::NewGuid(); OwnerReply.MessageId=OwnerQuestion;
    OwnerReply.bPlayerReply=true; OwnerReply.SentenceCount=1;
    Agent->BindJoinedResponse(OwnerReply,PlayerRequest);
    TestEqual(TEXT("关联救助不替换玩家消息编号"),OwnerReply.MessageId,OwnerQuestion);
    TestEqual(TEXT("玩家回复保存独立救助编号"),OwnerReply.JoinedEventId,Joined.EventId);
    Agent->Replies.Add(FGuid::NewGuid(),OwnerReply);
    Agent->FinishReplyPlayback(OwnerReply.ReplyId,FGuid::NewGuid());
    TestEqual(TEXT("玩家回复未实际说完时不结清"),Agent->PendingJoinedEvent,Joined.EventId);
    OwnerReply.ReplyId=FGuid::NewGuid(); Agent->Replies.Add(FGuid::NewGuid(),OwnerReply);
    Spoken.EventId=FGuid::NewGuid(); Spoken.ReplyToEventId=OwnerQuestion;
    Spoken.Description=TEXT("这句测试回复先回答主人当前的问题");
    TestTrue(TEXT("完整台词仍关联玩家原话"),Agent->ObserveEvent(Spoken));
    Agent->FinishReplyPlayback(OwnerReply.ReplyId,Spoken.EventId);
    TestFalse(TEXT("主人回复说完后不再额外主动感谢一次"),Agent->PendingJoinedEvent.IsValid());
    TestEqual(TEXT("附带救助回应不消耗主动重试次数"),Agent->JoinedResponseAttempts,0);
    Agent->ResetContext(TEXT("JoiningTestFinished"));

    auto* Companion=Npc->Companion.Get();
    const auto HasTravelCandidate=[](const FDeliveryNpcDecisionRequest& Choices,EDeliveryNpcAction Action,EDeliveryCompanionTravelMode Travel)
    {
        return Choices.Candidates.ContainsByPredicate([&](const auto& Candidate)
            { return Candidate.Action==Action && Candidate.TravelMode==Travel; });
    };
    TestTrue(TEXT("主人询问出行方式"),Agent->BeginListening(Owner->GetPawn(),FGuid::NewGuid()));
    FDeliveryNpcDecisionRequest TravelChoices;
    TestTrue(TEXT("没有专属车时仍可构造请求"),Agent->BuildDecisionRequest(FGuid::NewGuid(),TravelChoices));
    TestFalse(TEXT("没有专属车不提供 OwnVehicle"),HasTravelCandidate(TravelChoices,EDeliveryNpcAction::Follow,EDeliveryCompanionTravelMode::OwnVehicle));
    FActorSpawnParameters TravelSpawn;
    TravelSpawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* OwnBike=World->SpawnActor<ADeliveryCompanionMotorbike>(FVector(3000,0,88),FRotator::ZeroRotator,TravelSpawn);
    if (!TestNotNull(TEXT("测试使用独立同伴车"),OwnBike)) return false;
    TestTrue(TEXT("地图专属车登记给二狗"),OwnBike->InitializeForCompanion(Npc));
    const FVector FarView=OwnBike->GetActorLocation()+FVector(1000000,0,0);
    const auto BikeRelevantFor=[&](const APlayerController* Player)
    {
        return OwnBike->IsNetRelevantFor(Player,Player->GetPawn(),FarView);
    };
    const bool bParkedRelevantToOwner=BikeRelevantFor(Owner);
    const bool bParkedRelevantToStranger=BikeRelevantFor(Stranger);
    TestFalse(TEXT("远距离停放专属车使用普通网络裁剪"),bParkedRelevantToOwner);
    // 模拟角色已挂到车座、车的骑乘快照尚未读取时，收到迟到的 Joined 阶段
    TestTrue(TEXT("测试身体挂在实际专属车座椅"),Npc->AttachToComponent(OwnBike->CompanionSeat,FAttachmentTransformRules::KeepWorldTransform));
    Npc->OnRep_Encounter();
    TestTrue(TEXT("迟到的结识快照保持骑乘身体隐藏"),Npc->IsHidden());
    TestFalse(TEXT("迟到的结识快照保持骑乘身体无碰撞"),Npc->GetActorEnableCollision());
    TestEqual(TEXT("迟到的结识快照不会让车上身体开始走路"),Npc->GetCharacterMovement()->MovementMode.GetValue(),MOVE_None);
    Npc->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    Npc->OnRep_Encounter();
    TestFalse(TEXT("离开座椅后结识快照允许人物显示"),Npc->IsHidden());
    TestEqual(TEXT("离开座椅后仍能恢复步行"),Npc->GetCharacterMovement()->MovementMode.GetValue(),MOVE_Walking);
    OwnBike->bMounted=true;
    TestFalse(TEXT("没有步行导航时不从未经确认的出口下车"),OwnBike->TryDismountForWalk(OwnBike->GetActorLocation()+FVector(200,0,0)));
    TestTrue(TEXT("找不到步行出口时仍保留骑乘"),OwnBike->IsMounted());
    TestFalse(TEXT("跟随探测失败不会触发车辆的无条件下车重试"),OwnBike->bDismountPending);
    TestTrue(TEXT("二狗骑远后仍向主人同步作为小地图位置来源的专属车"),BikeRelevantFor(Owner));
    TestEqual(TEXT("二狗骑车不改变旁人的距离裁剪"),BikeRelevantFor(Stranger),bParkedRelevantToStranger);
    OwnBike->bMounted=false;
    TestEqual(TEXT("下车后恢复专属车原有的距离裁剪"),BikeRelevantFor(Owner),bParkedRelevantToOwner);
    Companion->ErrandVehicle=OwnBike;
    const auto MakeRoadBox=[&](const FVector& Location,const FVector& Extent)
    {
        auto* Actor=World->SpawnActor<AActor>();
        auto* Box=NewObject<UBoxComponent>(Actor);
        Actor->SetRootComponent(Box);
        Box->SetBoxExtent(Extent);
        Box->SetCollisionProfileName(TEXT("BlockAll"));
        Box->RegisterComponent();
        Actor->SetActorLocation(Location);
        return Actor;
    };
    const FVector FirstSide=OwnBike->GetBoardingLocation();
    const FVector OppositeSide=OwnBike->GetOppositeBoardingLocation();
    const FVector Seat=OwnBike->CompanionSeat->GetComponentLocation();
    const FVector EyeOffset=Npc->GetPawnViewLocation()-Npc->GetActorLocation();
    TestTrue(TEXT("无遮挡时可从常用侧上车"),OwnBike->CanBoardFromLocation(FirstSide));
    TestTrue(TEXT("无遮挡时可从另一侧上车"),OwnBike->CanBoardFromLocation(OppositeSide));
    auto* BoardingWall=MakeRoadBox((FirstSide+EyeOffset+Seat)*.5,FVector(100,5,110));
    TestFalse(TEXT("人在上车距离内也不能隔墙上车"),OwnBike->CanBoardFromLocation(FirstSide));
    TestTrue(TEXT("常用侧被挡时另一侧仍可上车"),OwnBike->CanBoardFromLocation(OppositeSide));
    BoardingWall->Destroy();
    const FVector RoadPoint(3400,0,55);
    auto* Road=MakeRoadBox(FVector(3500,0,-25),FVector(600,200,25));
    auto* Wall=MakeRoadBox(FVector(3500,75,75),FVector(300,12,75));
    const FQuat AlongRoad=FQuat::Identity;
    const FQuat AcrossRoad=FRotator(0,90,0).Quaternion();
    TestTrue(TEXT("车沿道路到达时停车位有车宽及四角支撑"),OwnBike->IsParkSpaceClear(RoadPoint,AlongRoad));
    TestFalse(TEXT("横着到达同一停车位时车头碰墙"),OwnBike->IsParkSpaceClear(RoadPoint,AcrossRoad));
    const FVector Ahead=RoadPoint+FVector(250,0,0);
    TestTrue(TEXT("沿墙行驶的车身扫掠通过"),OwnBike->HasClearDrivingSegment(RoadPoint,Ahead,AlongRoad));
    TestFalse(TEXT("提前抄过转角的车身扫掠被墙挡住"),OwnBike->HasClearDrivingSegment(RoadPoint,Ahead+FVector(0,120,0),AlongRoad));
    Wall->Destroy();
    Road->Destroy();
    {
        const FTransform PreviousBikePose=OwnBike->GetActorTransform();
        const bool bPreviousMounted=OwnBike->bMounted;
        const bool bPreviousArrived=OwnBike->bArrived;
        const FName PreviousDriveFailure=OwnBike->DriveFailure;
        const FVector SlopeSurface(9800,7000,0);
        const FVector RampExtent(360,100,20);
        auto* Ramp=MakeRoadBox(SlopeSurface,RampExtent);
        AActor* RampWall=nullptr;
        AActor* RidgeDescent=nullptr;
        ON_SCOPE_EXIT
        {
            OwnBike->StopDriving();
            OwnBike->bMounted=bPreviousMounted;
            OwnBike->bArrived=bPreviousArrived;
            OwnBike->DriveFailure=PreviousDriveFailure;
            OwnBike->SetActorTransform(PreviousBikePose,false,nullptr,ETeleportType::TeleportPhysics);
            Ramp->Destroy();
            if (RampWall) RampWall->Destroy();
            if (RidgeDescent) RidgeDescent->Destroy();
        };
        const auto SetSlope=[&](float Degrees)
        {
            const FQuat Rotation(FVector::ForwardVector,FMath::DegreesToRadians(Degrees));
            Ramp->SetActorLocationAndRotation(SlopeSurface-Rotation.RotateVector(FVector(0,0,RampExtent.Z)),Rotation);
        };
        // 沿实体横坡的等高线验证默认车身能够通过
        OwnBike->bMounted=true;
        const FVector SlopeStart=SlopeSurface+FVector(-150,0,55);
        const FVector SlopeGoal=SlopeSurface+FVector(150,0,55);
        const float DrivingNormal=FMath::Cos(FMath::DegreesToRadians(35.f));
        SetSlope(30.f);
        if (!TestTrue(TEXT("实体横坡夹具满足空闲骑行前置条件"),OwnBike->CanCompanionRide())) return false;
        FVector SupportedStart;
        if (!TestTrue(TEXT("默认车身在草坡上可预检出有净空的中心"),
            OwnBike->IsSupportedVehicleSpace(SlopeStart,AlongRoad,DrivingNormal,&SupportedStart,true))) return false;
        OwnBike->SetActorLocation(SupportedStart,false,nullptr,ETeleportType::TeleportPhysics);
        TestTrue(TEXT("30 度真实草坡提供可以行驶的四角支撑"),
            OwnBike->IsSupportedVehicleSpace(SlopeGoal,AlongRoad,DrivingNormal,nullptr,true));
        TestFalse(TEXT("同一草坡不可作为停车点"),OwnBike->IsParkSpaceClear(SlopeGoal,AlongRoad));
        TestTrue(TEXT("专属车接受沿坡面通过的真实跟车路段"),OwnBike->TrySetDirectFollowDestination(SlopeGoal) &&
            OwnBike->HasActiveDriveRoute());
        OwnBike->StopDriving();
        TestFalse(TEXT("车轮伸出坡边时仍拒绝缺乏支撑的点"),
            OwnBike->IsSupportedVehicleSpace(SlopeGoal+FVector(0,75,0),AlongRoad,DrivingNormal,nullptr,true));

        RampWall=MakeRoadBox(SlopeSurface+FVector(0,0,110),FVector(10,120,110));
        TestFalse(TEXT("可行驶坡面也不能穿过中间的墙"),OwnBike->TrySetDirectFollowDestination(SlopeGoal));
        RampWall->Destroy();
        RampWall=nullptr;
        SetSlope(50.f);
        TestFalse(TEXT("50 度陡坡无可接受的行驶支撑"),
            OwnBike->IsSupportedVehicleSpace(SlopeGoal,AlongRoad,DrivingNormal,nullptr,true));
        TestFalse(TEXT("跟车也拒绝陡坡路段"),OwnBike->TrySetDirectFollowDestination(SlopeGoal));

        const auto PlaceRamp=[&](AActor* Ground, const FVector& TopCenter, const FVector& Extent, const FQuat& Pitch)
        {
            CastChecked<UBoxComponent>(Ground->GetRootComponent())->SetBoxExtent(Extent);
            Ground->SetActorLocationAndRotation(TopCenter-Pitch.RotateVector(FVector(0,0,Extent.Z)),Pitch);
        };
        const auto FindDriveCenter=[&](const FVector& Ground,FVector& Center)
        {
            return OwnBike->IsSupportedVehicleSpace(Ground,AlongRoad,DrivingNormal,&Center,true);
        };
        const FQuat Grade=FRotator(15,0,0).Quaternion();
        const double GradeRise=900.*FMath::Tan(FMath::DegreesToRadians(15.));
        const FVector GradeRun(900,0,GradeRise);
        PlaceRamp(Ramp,SlopeSurface,FVector(1250,250,24),Grade);
        FVector GradeLow,GradeHigh;
        if (!TestTrue(TEXT("1800 厘米实体长坡两端均有支撑，累计高差超过 180 厘米"),
            FindDriveCenter(SlopeSurface-GradeRun,GradeLow) && FindDriveCenter(SlopeSurface+GradeRun,GradeHigh) &&
            GradeHigh.Z-GradeLow.Z>180.)) return false;
        OwnBike->SetActorLocation(GradeLow,false,nullptr,ETeleportType::TeleportPhysics);
        TestTrue(TEXT("连续贴地探测通过长距离上坡"),OwnBike->TrySetDirectFollowDestination(GradeHigh) &&
            OwnBike->HasActiveDriveRoute());
        OwnBike->StopDriving();
        TestFalse(TEXT("同一平面位置的上层错误目标仍被拒绝"),
            OwnBike->TrySetDirectFollowDestination(GradeHigh+FVector(0,0,350)));
        FVector HorizontalProbe=GradeHigh;
        HorizontalProbe.Z=GradeLow.Z;
        TestTrue(TEXT("合成水平探路可找到真实坡面终点"),
            OwnBike->TrySetDirectFollowDestination(HorizontalProbe,false) && OwnBike->DriveGoal.Equals(GradeHigh,1.));
        OwnBike->StopDriving();
        OwnBike->SetActorLocation(GradeHigh,false,nullptr,ETeleportType::TeleportPhysics);
        TestTrue(TEXT("同一实体长坡也允许反向下行"),OwnBike->TrySetDirectFollowDestination(GradeLow) &&
            OwnBike->HasActiveDriveRoute());
        OwnBike->StopDriving();

        const FVector Crest(9800,11000,400);
        // 1980 厘米分成 22 个采样，中央采样落在坡脊顶
        constexpr double RidgeHalfRun=990.;
        const double RidgeRise=RidgeHalfRun*FMath::Tan(FMath::DegreesToRadians(17.));
        const FVector RidgeExtent(605,250,24);
        const FQuat Ascent=FRotator(17,0,0).Quaternion();
        const FQuat Descent=FRotator(-17,0,0).Quaternion();
        RidgeDescent=MakeRoadBox(Crest,RidgeExtent);
        // 两个坡面的顶部略重叠，避免在坡脊处留下夹具接缝
        PlaceRamp(Ramp,Crest-Ascent.RotateVector(FVector(600,0,0)),RidgeExtent,Ascent);
        PlaceRamp(RidgeDescent,Crest+Descent.RotateVector(FVector(600,0,0)),RidgeExtent,Descent);
        const FVector RidgeEndGround=Crest+FVector(RidgeHalfRun,0,-RidgeRise);
        FVector RidgeStart,RidgeEnd;
        if (!TestTrue(TEXT("坡脊起终点同高但中间超过 300 厘米"),RidgeRise>300. &&
            FindDriveCenter(Crest+FVector(-RidgeHalfRun,0,-RidgeRise),RidgeStart) &&
            FindDriveCenter(RidgeEndGround,RidgeEnd) && FMath::IsNearlyEqual(RidgeStart.Z,RidgeEnd.Z,1.))) return false;
        OwnBike->SetActorLocation(RidgeStart,false,nullptr,ETeleportType::TeleportPhysics);
        const bool bRidgeRoute=OwnBike->TrySetDirectFollowDestination(RidgeEnd);
        if (!bRidgeRoute)
        {
            // 失败时按正式查询的顺序定位第一个采样，原坡面和车辆不变
            const FVector Here=OwnBike->GetActorLocation();
            const double Distance=FVector::Dist2D(Here,RidgeEnd);
            const int32 Samples=FMath::CeilToInt(Distance/90.);
            const FQuat Heading=(RidgeEnd-Here).GetSafeNormal2D().Rotation().Quaternion();
            const float MinimumNormal=FMath::Cos(FMath::DegreesToRadians(OwnBike->MaxClimbAngle));
            auto* Box=OwnBike->CollisionBox.Get();
            const FVector Extent=Box->GetScaledBoxExtent();
            FCollisionQueryParams Query(SCENE_QUERY_STAT(SharpRidgeRegression),false,OwnBike);
            Query.AddIgnoredActor(OwnBike->GetReservedCompanion());
            const auto TraceGround=[&](const FVector& At,double Top,double Bottom,FHitResult& Hit)
            {
                return World->LineTraceSingleByChannel(Hit,FVector(At.X,At.Y,Top),FVector(At.X,At.Y,Bottom),ECC_Visibility,Query);
            };
            const auto SupportFailure=[&](const FVector& Probe)
            {
                FHitResult Ground;
                if (!TraceGround(Probe,Probe.Z+150.,Probe.Z-300.,Ground)) return FString(TEXT("center-missing"));
                if (Ground.ImpactNormal.Z<MinimumNormal)
                    return FString::Printf(TEXT("center-slope normal=%.4f minimum=%.4f"),Ground.ImpactNormal.Z,MinimumNormal);
                for (const float X:{-.85f,.85f}) for (const float Y:{-.85f,.85f})
                {
                    const FVector Offset=Heading.RotateVector(FVector(Extent.X*X,Extent.Y*Y,0));
                    const FVector Corner=Ground.ImpactPoint+Offset;
                    const double PredictedZ=Corner.Z-FVector::DotProduct(Offset,Ground.ImpactNormal)/Ground.ImpactNormal.Z;
                    const double Top=FMath::Max(Ground.ImpactPoint.Z,PredictedZ)+50.;
                    const double Bottom=FMath::Min(Ground.ImpactPoint.Z,PredictedZ)-50.;
                    FHitResult Support;
                    if (!TraceGround(Corner,Top,Bottom,Support) || Support.ImpactNormal.Z<MinimumNormal)
                        return FString::Printf(TEXT("corner-support xy=%s traceZ=%.2f..%.2f hit=%s normal=%.4f minimum=%.4f"),
                            *Corner.ToCompactString(),Bottom,Top,*GetNameSafe(Support.GetActor()),Support.ImpactNormal.Z,MinimumNormal);
                }
                return FString::Printf(TEXT("body-overlap ground=%s normal=%.4f extent=%s"),
                    *Ground.ImpactPoint.ToCompactString(),Ground.ImpactNormal.Z,*Extent.ToCompactString());
            };

            FVector Previous=Here;
            FString Rejection;
            for (int32 Index=1;Index<=Samples;++Index)
            {
                FVector Probe=FMath::Lerp(Here,RidgeEnd,double(Index)/Samples);
                Probe.Z=Previous.Z;
                FVector Center;
                if (!OwnBike->IsSupportedVehicleSpace(Probe,Heading,MinimumNormal,&Center,true)) Rejection=SupportFailure(Probe);
                else if (FMath::Abs(Center.Z-Previous.Z)>65.)
                    Rejection=FString::Printf(TEXT("local-step deltaZ=%.2f fit=%s"),Center.Z-Previous.Z,*Center.ToCompactString());
                else if (!OwnBike->HasClearDrivingSegment(Previous,Center,Heading))
                {
                    FHitResult Hit;
                    World->SweepSingleByChannel(Hit,Previous,Center,Heading,Box->GetCollisionObjectType(),
                        FCollisionShape::MakeBox(Extent),Query,FCollisionResponseParams(Box->GetCollisionResponseToChannels()));
                    Rejection=FString::Printf(TEXT("segment-sweep fit=%s hit=%s impact=%s normal=%s penetrating=%d extent=%s"),
                        *Center.ToCompactString(),*GetNameSafe(Hit.GetActor()),*Hit.ImpactPoint.ToCompactString(),
                        *Hit.ImpactNormal.ToCompactString(),Hit.bStartPenetrating,*Extent.ToCompactString());
                }
                if (!Rejection.IsEmpty())
                {
                    AddInfo(FString::Printf(TEXT("SharpRidge first_failure sample=%d/%d from=%s probe=%s reason=%s"),
                        Index,Samples,*Previous.ToCompactString(),*Probe.ToCompactString(),*Rejection));
                    break;
                }
                Previous=Center;
            }
            if (Rejection.IsEmpty())
                AddInfo(FString::Printf(TEXT("SharpRidge samples_passed=%d last=%s goal=%s can_ride=%d seated=%d"),
                    Samples,*Previous.ToCompactString(),*RidgeEnd.ToCompactString(),OwnBike->CanCompanionRide(),OwnBike->IsMounted()));
        }
        TestTrue(TEXT("起终同高时跟车仍可经过中间的真实坡脊"),bRidgeRoute &&
            OwnBike->HasActiveDriveRoute());
        OwnBike->StopDriving();
        RidgeDescent->AddActorWorldOffset(FVector(260,0,0));
        TestTrue(TEXT("坡脊分开后远端本身仍有可走的支撑"),FindDriveCenter(RidgeEndGround,RidgeEnd));
        TestFalse(TEXT("即使两端各有支撑也不能跨坡脊中的断层"),OwnBike->TrySetDirectFollowDestination(RidgeEnd));
    }
    {
        const FTransform ActualBikePose=OwnBike->GetActorTransform();
        const FTransform SavedNpcPose=Npc->GetActorTransform();
        const bool bSavedMounted=OwnBike->bMounted;
        const double SavedDismountAttempt=OwnBike->NextDismountAttempt;
        TArray<AActor*,TInlineAllocator<6>> ExitGeometry;
        ON_SCOPE_EXIT
        {
            OwnBike->bMounted=bSavedMounted;
            OwnBike->NextDismountAttempt=SavedDismountAttempt;
            Npc->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
            Npc->SetActorTransform(SavedNpcPose,false,nullptr,ETeleportType::TeleportPhysics);
            for (auto* Geometry:ExitGeometry) if (IsValid(Geometry)) Geometry->Destroy();
        };
        if (!TestTrue(TEXT("查询停车位时二狗仍挂在实际车的座椅"),Npc->AttachToComponent(OwnBike->CompanionSeat,
            FAttachmentTransformRules::SnapToTargetNotIncludingScale))) return false;
        OwnBike->bMounted=true;
        const FVector ActualCenter=ActualBikePose.GetLocation();
        const FVector SeatInBike=ActualBikePose.InverseTransformPosition(OwnBike->CompanionSeat->GetComponentLocation());
        const FVector CandidateFloor(6200,5100,0);
        ExitGeometry.Add(MakeRoadBox(FVector(ActualCenter.X,ActualCenter.Y,-25),FVector(350,350,25)));
        ExitGeometry.Add(MakeRoadBox(CandidateFloor-FVector(0,0,25),FVector(350,350,25)));
        FVector CandidateCenter;
        if (!TestTrue(TEXT("另一个朝向的预计车位确实可停车"),
            OwnBike->IsParkSpaceClear(CandidateFloor,AcrossRoad,&CandidateCenter))) return false;
        const FVector CandidateSeat=FTransform(AcrossRoad,CandidateCenter,ActualBikePose.GetScale3D()).TransformPosition(SeatInBike);
        FVector CurrentExit, CandidateExit;
        if (!TestTrue(TEXT("当前车位实际存在安全落脚点"),OwnBike->FindDismountLocation(CurrentExit)) ||
            !TestTrue(TEXT("远处换向后的车位也有真实落脚点"),
                OwnBike->FindDismountLocationAtPose(CandidateCenter,AcrossRoad,CandidateSeat,CandidateExit))) return false;
        const FVector ExpectedExitOffset=AcrossRoad.RotateVector(ActualBikePose.GetRotation().UnrotateVector(CurrentExit-ActualCenter));
        TestTrue(TEXT("预计落脚点使用新车位位置和朝向"),
            FVector::Dist2D(CandidateExit,CandidateCenter+ExpectedExitOffset)<3.);

        const auto* BikeBox=CastChecked<UBoxComponent>(OwnBike->GetRootComponent());
        const FVector BikeExtent=BikeBox->GetScaledBoxExtent();
        const auto AddExitWall=[&](const FVector& LocalOffset,const FVector& WallExtent)
        {
            FVector Location=CandidateCenter+AcrossRoad.RotateVector(LocalOffset);
            Location.Z=WallExtent.Z;
            auto* ExitWall=MakeRoadBox(Location,WallExtent);
            ExitWall->SetActorRotation(AcrossRoad);
            ExitGeometry.Add(ExitWall);
        };
        // 高墙立在车身外缘，墙顶高于落脚探测，不会被当作可站地面
        for (const double Side:{-1.,1.})
        {
            AddExitWall(FVector(0,Side*(BikeExtent.Y+15.),0),FVector(BikeExtent.X+35.,5,260));
            AddExitWall(FVector(Side*(BikeExtent.X+15.),0,0),FVector(5,BikeExtent.Y+35.,260));
        }
        TestTrue(TEXT("四侧有墙时预计车身仍可停放"),OwnBike->IsParkSpaceClear(CandidateFloor,AcrossRoad));
        TestTrue(TEXT("远处有围墙时当前停车点仍有安全出口"),OwnBike->FindDismountLocation(CurrentExit));
        TestFalse(TEXT("预计车位被围住时不能借当前车辆的落脚点"),
            OwnBike->FindDismountLocationAtPose(CandidateCenter,AcrossRoad,CandidateSeat,CandidateExit));
        for (int32 Index=2;Index<ExitGeometry.Num();++Index) ExitGeometry[Index]->Destroy();
        ExitGeometry.SetNum(2);
        TestTrue(TEXT("移开远处围墙后预计落脚点恢复"),
            OwnBike->FindDismountLocationAtPose(CandidateCenter,AcrossRoad,CandidateSeat,CandidateExit));
        TestTrue(TEXT("试查前后车辆未移动且二狗仍在原座"),OwnBike->GetActorTransform().Equals(ActualBikePose) &&
            Npc->GetRootComponent()->GetAttachParent()==OwnBike->CompanionSeat.Get() && OwnBike->IsMounted() && !OwnBike->bDismountPending);

        auto* ExitBlocker=MakeRoadBox(ActualCenter+FVector(0,0,120),FVector(380,380,250));
        ExitGeometry.Add(ExitBlocker);
        TestFalse(TEXT("实际无出口时安全下车请求被拒绝"),OwnBike->TryDismountIfClear());
        TestTrue(TEXT("无净空时保留座椅且不进入无条件下车重试"),OwnBike->IsMounted() && !OwnBike->bDismountPending &&
            Npc->GetRootComponent()->GetAttachParent()==OwnBike->CompanionSeat.Get());
        ExitBlocker->Destroy();
        TestTrue(TEXT("清除阻挡后经同一接口在真实安全点落脚"),OwnBike->TryDismountIfClear() &&
            !OwnBike->IsMounted() && !OwnBike->bDismountPending && !Npc->GetAttachParentActor() &&
            Npc->GetActorLocation().Equals(CurrentExit,1.f));
    }
    auto* OwnerBike=World->SpawnActor<ADeliveryMotorbike>(FVector(4000,0,88),FRotator::ZeroRotator,TravelSpawn);
    if (!TestNotNull(TEXT("测试使用主人车辆"),OwnerBike)) return false;
    for (int32 Step=0;Step<40;++Step)
    {
        OwnerBike->SetActorLocation(FVector(4000+Step*200,0,88));
        Companion->SampleFollowBikeRoute(OwnerBike);
    }
    TestEqual(TEXT("跟车只保留最近的车位"),Companion->FollowRoute.Num(),32);
    Companion->SampleFollowBikeRoute(OwnerBike);
    TestEqual(TEXT("停车时不重复添加同一车位"),Companion->FollowRoute.Num(),32);
    OwnerBike->SetActorLocation(FVector(19000,0,88));
    Companion->SampleFollowBikeRoute(OwnerBike);
    TestEqual(TEXT("位置突变清除原路线"),Companion->FollowRoute.Num(),1);
    Companion->SampleFollowBikeRoute(nullptr);
    TestTrue(TEXT("离开当前车辆清除车位记录"),Companion->FollowRoute.IsEmpty());

    const auto ErrandTravel=EDeliveryCompanionTravelMode::OwnVehicle;
    Companion->ApplyTravelInstruction(ErrandTravel);
    const auto CheckFollowSnapshot=[&](EDeliveryCompanionTravelMode Expected)
    {
        FDeliveryNpcDecisionRequest FollowSettings;
        Companion->ExtendTravelDecisionRequest(FollowSettings,Owner->PlayerState);
        TestEqual(TEXT("快照报告原始跟随方式"),FollowSettings.CompanionTravelMode,
            StaticEnum<EDeliveryCompanionTravelMode>()->GetNameStringByValue(int64(Expected)));
        TestEqual(TEXT("快照单独报告跑腿方式"),FollowSettings.CompanionErrandTravelMode,FString(TEXT("OwnVehicle")));
        TestEqual(TEXT("跟随选择未改跑腿设置"),Companion->GetTravelMode(),ErrandTravel);
        TestEqual(TEXT("主人骑车或给出骑车安排不能冒充二狗已上车"),FollowSettings.CompanionVehicleState,FString(TEXT("NotMounted")));
    };
    const auto SnapshotOwnerVehicle=[&](APlayerState* Requester,bool bPlayerRequest=true)
    {
        FDeliveryNpcDecisionRequest Request;
        Request.bPlayerRequest=bPlayerRequest;
        Npc->ExtendDecisionRequest(Request,Requester);
        return Request.CompanionOwnerVehicleState;
    };
    APawn* OriginalWalkingPawn=Owner->GetPawn();
    TestTrue(TEXT("主人可以用默认方式开始跟随"),Companion->ApplyCommand(EDeliveryNpcAction::Follow,
        Owner->PlayerState,Companion->GetCommandRevision()));
    TestEqual(TEXT("从未指定跟随方式时保留 Unspecified"),Companion->FollowTravelMode,EDeliveryCompanionTravelMode::Unspecified);
    CheckFollowSnapshot(EDeliveryCompanionTravelMode::Unspecified);
    TestFalse(TEXT("缺少主人控制对象时默认不跟车"),Companion->ShouldRideForFollow(nullptr));
    TestFalse(TEXT("主人步行时默认步行"),Companion->ShouldRideForFollow(OriginalWalkingPawn));
    TestFalse(TEXT("停在远处且未被主人控制的车不能触发跟车"),Companion->ShouldRideForFollow(OwnerBike));

    FActorSpawnParameters DriverSpawn=TravelSpawn;
    DriverSpawn.Name=TEXT("CompanionFollowDriver");
    // 原生步行夹具没有物理资产，生成和两次真实下车都会尝试开启布娃娃
    AddExpectedError(TEXT("CompanionFollowDriver: Skeletal mesh has no Physics Asset."),
        EAutomationExpectedErrorFlags::Contains,3);
    auto* DrivingCharacter=World->SpawnActor<ADeliveryCharacter>(OwnerBike->GetActorLocation(),FRotator::ZeroRotator,DriverSpawn);
    if (!TestNotNull(TEXT("创建测试驾驶者"),DrivingCharacter)) return false;
    Owner->Possess(DrivingCharacter);
    DrivingCharacter->GetActiveRagdoll()->StopRagdoll();
    if (!TestTrue(TEXT("主人通过真实上车入口控制车辆"),OwnerBike->TryEnter(DrivingCharacter) && Owner->GetPawn()==OwnerBike)) return false;
    TestEqual(TEXT("通过车辆控制权保持主人 PlayerState"),OwnerBike->GetPlayerState(),Owner->PlayerState.Get());
    TestEqual(TEXT("真实上车后主人请求读到已骑乘"),SnapshotOwnerVehicle(Owner->PlayerState),FString(TEXT("Riding")));
    TestTrue(TEXT("访客与环境请求都不携带主人车辆状态"),
        SnapshotOwnerVehicle(Stranger->PlayerState).IsEmpty() && SnapshotOwnerVehicle(Owner->PlayerState,false).IsEmpty());
    TestFalse(TEXT("未获骑车指令时主人上车不会自动取专属车"),Companion->ShouldRideForFollow(OwnerBike));
    CheckFollowSnapshot(EDeliveryCompanionTravelMode::Unspecified);
    TestFalse(TEXT("步行身体仍挂在车上也不是当前控制对象"),Companion->ShouldRideForFollow(DrivingCharacter));
    TestTrue(TEXT("主人驾驶时可明确要求步行"),Companion->ApplyCommand(EDeliveryNpcAction::Follow,
        Owner->PlayerState,Companion->GetCommandRevision(),nullptr,nullptr,EDeliveryCompanionTravelMode::Walk));
    TestFalse(TEXT("明确步行不因主人驾驶改成骑车"),Companion->ShouldRideForFollow(OwnerBike));
    CheckFollowSnapshot(EDeliveryCompanionTravelMode::Walk);
    TestTrue(TEXT("主人驾驶时可以改成自动选择"),Companion->ApplyCommand(EDeliveryNpcAction::Follow,
        Owner->PlayerState,Companion->GetCommandRevision(),nullptr,nullptr,EDeliveryCompanionTravelMode::Automatic));
    TestTrue(TEXT("自动选择随实际控制的车辆骑行"),Companion->ShouldRideForFollow(OwnerBike));
    CheckFollowSnapshot(EDeliveryCompanionTravelMode::Automatic);
    {
        auto* Controller=Cast<AAIController>(Npc->GetController());
        auto* Following=Controller?Controller->GetPathFollowingComponent():nullptr;
        if (!TestNotNull(TEXT("取车共用二狗的实际路径组件"),Following) ||
            !TestTrue(TEXT("取车测试前没有活动路线"),Following->GetStatus()==EPathFollowingStatus::Idle)) return false;
        const FVector SavedNpcLocation=Npc->GetActorLocation();
        const double SavedVehicleNavigationAt=Companion->NextVehicleNavigationAt;
        const int32 TravelRevision=Companion->GetCommandRevision();
        AActor* BlockedBoarding=nullptr;
        FAIRequestID OtherMove=FAIRequestID::InvalidRequest;
        ON_SCOPE_EXIT
        {
            Companion->StopVehicleApproach();
            if (OtherMove.IsValid() && Following->GetCurrentRequestId()==OtherMove)
                Following->AbortMove(*Companion,FPathFollowingResultFlags::UserAbort,OtherMove);
            if (BlockedBoarding) BlockedBoarding->Destroy();
            Companion->NextVehicleNavigationAt=SavedVehicleNavigationAt;
            Npc->SetActorLocation(SavedNpcLocation,false,nullptr,ETeleportType::TeleportPhysics);
        };
        Npc->SetActorLocation(FirstSide-FVector(500,0,0),false,nullptr,ETeleportType::TeleportPhysics);
        const FVector FeetToCenter(0,0,Npc->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
        const FVector BoardFeet=FirstSide-FeetToCenter;
        const FVector OppositeFeet=OppositeSide-FeetToCenter;
        // 测试世界没有烘焙导航，直接向路径组件提交已选中的两点路线
        const auto RequestMoveTo=[&](const FVector& Goal)
        {
            FAIMoveRequest Move(Goal);
            Move.SetAcceptanceRadius(15.f);
            const auto Path=MakeShared<FNavigationPath,ESPMode::ThreadSafe>(TArray<FVector>{Npc->GetNavAgentLocation(),Goal});
            return Following->RequestMove(Move,Path);
        };
        const auto RequestBoardingMove=[&](const FVector& Goal)
        {
            Companion->VehicleApproachMoveId=RequestMoveTo(Goal);
            Companion->bVehicleOwnsApproach=Companion->VehicleApproachMoveId.IsValid();
            Companion->VehicleApproachFeet=Goal;
            return Companion->VehicleApproachMoveId;
        };
        if (!TestTrue(TEXT("车辆两侧的上车站位最初都可用"),Companion->IsVehicleBoardingSpotUsable(BoardFeet) &&
            Companion->IsVehicleBoardingSpotUsable(OppositeFeet))) return false;
        const FAIRequestID BoardingMove=RequestBoardingMove(BoardFeet);
        if (!TestTrue(TEXT("二狗正在执行已选中的上车路线"),BoardingMove.IsValid() &&
            Following->GetStatus()==EPathFollowingStatus::Moving)) return false;
        const double Now=World->GetTimeSeconds();
        Companion->NextVehicleNavigationAt=0;
        TestTrue(TEXT("净空的上车站位保留原请求"),Companion->KeepVehicleApproach(*Following,Now) &&
            Following->GetCurrentRequestId()==BoardingMove);

        BlockedBoarding=MakeRoadBox(BoardFeet+FeetToCenter,FVector(22,22,FeetToCenter.Z));
        TestFalse(TEXT("放入实体箱后原上车站位不可用"),Companion->IsVehicleBoardingSpotUsable(BoardFeet));
        TestTrue(TEXT("原站位被占时另一侧仍可上车"),Companion->IsVehicleBoardingSpotUsable(OppositeFeet));
        TestTrue(TEXT("仍在原导航间隔内时保留原请求"),Companion->KeepVehicleApproach(*Following,
            Now+Companion->GetTuning().NavigationInterval*.5) && Following->GetCurrentRequestId()==BoardingMove);
        TestFalse(TEXT("到复查时间后放弃已被占用的上车路线"),Companion->KeepVehicleApproach(*Following,Now+10.));
        TestTrue(TEXT("解除原取车请求并允许立即重选"),Following->GetStatus()==EPathFollowingStatus::Idle &&
            !Companion->bVehicleOwnsApproach && !Companion->VehicleApproachMoveId.IsValid() &&
            Companion->NextVehicleNavigationAt<=Now+10.);
        TestTrue(TEXT("更换上车站位保留主人和自动跟随安排"),Companion->GetMode()==EDeliveryCompanionMode::Follow &&
            Companion->FollowTravelMode==EDeliveryCompanionTravelMode::Automatic && Companion->GetCommandRevision()==TravelRevision &&
            Companion->CanCommand(Owner->PlayerState) && Companion->ShouldRideForFollow(OwnerBike));

        const FAIRequestID OppositeMove=RequestBoardingMove(OppositeFeet);
        if (!TestTrue(TEXT("另一侧可提交新的上车路线"),OppositeMove.IsValid() &&
            Companion->KeepVehicleApproach(*Following,Now+20.))) return false;
        OtherMove=RequestMoveTo(OppositeFeet+FVector(200,0,0));
        if (!TestTrue(TEXT("已有另一个接管路径组件的移动请求"),OtherMove.IsValid() && OtherMove!=OppositeMove)) return false;
        TestFalse(TEXT("取车复查不会认领接管者的请求"),Companion->KeepVehicleApproach(*Following,Now+30.));
        TestTrue(TEXT("取车旧编号不会取消接管者的路线"),Following->GetCurrentRequestId()==OtherMove &&
            Following->GetStatus()==EPathFollowingStatus::Moving);
    }
    if (!TestTrue(TEXT("主人骑车时明确要求二狗骑专属车"),Companion->ApplyCommand(EDeliveryNpcAction::Follow,
        Owner->PlayerState,Companion->GetCommandRevision(),nullptr,nullptr,EDeliveryCompanionTravelMode::OwnVehicle))) return false;
    {
        const int32 RidingFollowRevision=Companion->GetCommandRevision();
        TestFalse(TEXT("访客不能保持或重派主人的跟车"),
            Companion->CanKeepFollowCommand(Stranger->PlayerState,RidingFollowRevision,EDeliveryCompanionTravelMode::Unspecified) ||
            Companion->ApplyCommand(EDeliveryNpcAction::Follow,Stranger->PlayerState,RidingFollowRevision));
        TestFalse(TEXT("旧编号不能当成本次跟车的重复命令"),
            Companion->CanKeepFollowCommand(Owner->PlayerState,RidingFollowRevision-1,EDeliveryCompanionTravelMode::Unspecified) ||
            Companion->ApplyCommand(EDeliveryNpcAction::Follow,Owner->PlayerState,RidingFollowRevision-1));

        auto* Controller=Cast<AAIController>(Npc->GetController());
        if (!TestNotNull(TEXT("重复跟随使用原 AI 控制器"),Controller)) return false;
        const FVector RoadCenter(26000,12000,55);
        auto* Floor=MakeRoadBox(RoadCenter-FVector(0,0,80),FVector(650,350,25));
        const FTransform PreviousBike=OwnBike->GetActorTransform();
        const FTransform PreviousNpc=Npc->GetActorTransform();
        auto* PreviousMesh=Npc->GetMesh()->GetSkeletalMeshAsset();
        auto* PreviousRide=OwnBike->RideAnimation.Get();
        auto* Behavior=NewObject<UBehaviorTreeComponent>(Controller);
        Behavior->RegisterComponent();
        ON_SCOPE_EXIT
        {
            Agent->CancelPlan(TEXT("RepeatFollowTest"));
            Companion->ReleaseErrandVehicle();
            Behavior->DestroyComponent();
            OwnBike->SetActorTransform(PreviousBike,false,nullptr,ETeleportType::TeleportPhysics);
            OwnBike->RideAnimation=PreviousRide;
            Npc->GetMesh()->SetSkeletalMeshAsset(PreviousMesh);
            Npc->SetActorTransform(PreviousNpc,false,nullptr,ETeleportType::TeleportPhysics);
            Floor->Destroy();
        };
        auto* Mesh=LoadObject<USkeletalMesh>(nullptr,TEXT("/Game/NPC/Companion/Appearance/SK_ErgouBoy.SK_ErgouBoy"));
        auto* Ride=LoadObject<UAnimSequence>(nullptr,TEXT("/Game/NPC/Companion/Animation/A_ErgouRide.A_ErgouRide"));
        if (!TestNotNull(TEXT("重复跟随复用正式二狗模型"),Mesh) || !TestNotNull(TEXT("重复跟随复用正式骑姿"),Ride)) return false;
        Npc->GetMesh()->SetSkeletalMeshAsset(Mesh);
        OwnBike->RideAnimation=Ride;
        OwnBike->SetActorLocationAndRotation(RoadCenter,FRotator::ZeroRotator,false,nullptr,ETeleportType::TeleportPhysics);
        FVector Boarding=OwnBike->GetBoardingLocation();
        Boarding.Z=Npc->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()+3.;
        Npc->SetActorLocation(Boarding,false,nullptr,ETeleportType::TeleportPhysics);
        Npc->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
        if (!TestTrue(TEXT("二狗实际骑上自己的实体车"),OwnBike->TryMount(Npc) && Npc->IsMountedOnCompanionBike())) return false;
        Companion->VehicleTravelPurpose=EDeliveryCompanionTravelPurpose::Follow;
        Companion->VehicleLegRevision=RidingFollowRevision;
        Companion->VehicleFollowOwnerPawn=OwnerBike;
        Companion->SampleFollowBikeRoute(OwnerBike);

        const auto ExecuteFollowReply=[&](EDeliveryCompanionTravelMode Travel)
        {
            FDeliveryNpcObservation Order;
            Order.EventId=FGuid::NewGuid();
            Order.Kind=TEXT("PlayerSpeech");
            Order.Subject=OwnerBike;
            Order.Addressee=Npc;
            Order.Source=EDeliveryNpcEvidenceSource::Heard;
            Order.Description=Travel==EDeliveryCompanionTravelMode::Walk?TEXT("你走路跟着我。"):TEXT("继续跟着我。");
            Order.bNeedsReply=true;
            FDeliveryNpcDecisionRequest Request;
            if (!TestTrue(TEXT("真实主人当前车辆提交跟随请求"),Agent->BeginListening(OwnerBike,Order.EventId) &&
                Agent->ObserveEvent(Order) && Agent->BuildDecisionRequest(FGuid::NewGuid(),Request))) return false;
            const auto* Candidate=Request.Candidates.FindByPredicate([Travel](const auto& Entry)
                { return Entry.Action==EDeliveryNpcAction::Follow && Entry.TravelMode==Travel; });
            if (!TestNotNull(TEXT("指定的跟随方式确实来自原请求候选"),Candidate)) return false;
            FDeliveryNpcDecisionProposal Proposal;
            Proposal.CapabilityVersion=Request.RequestedCapabilityVersion;
            Proposal.CandidateId=Candidate->CandidateId;
            Proposal.TargetId=Candidate->TargetId;
            Proposal.EvidenceIds=Candidate->EvidenceIds;
            Proposal.WorldEpoch=Request.WorldEpoch;
            Proposal.NpcId=Request.NpcId;
            Proposal.RequestId=Request.RequestId;
            Proposal.ContextRevision=Request.ContextRevision;
            Proposal.PlanRevision=Request.PlanRevision;
            Proposal.Action=Candidate->Action;
            Agent->ActiveRequest=Request;
            FName Rejection;
            if (!TestTrue(TEXT("模型动作经真实候选和权限采纳"),Agent->TryAcceptProposal(Proposal,Rejection))) return false;
            Agent->ActiveRequest={};
            TestEqual(TEXT("已接受的动作只有重复原跟随才允许持续移动"),Agent->CanContinueCompanionMovement(),
                Travel==EDeliveryCompanionTravelMode::Unspecified);
            auto* Task=NewObject<UBTTask_DeliveryExecuteNpcAction>(Behavior);
            return TestTrue(TEXT("使用正式行为树节点执行并结束此次跟随指令"),
                Task->ExecuteTask(*Behavior,nullptr)==EBTNodeResult::Succeeded &&
                Agent->GetPublicState().Status==EDeliveryNpcActionStatus::Succeeded);
        };

        const FName RouteBlocked(TEXT("CompanionVehicleNoRoute"));
        Companion->ReportVehiclePause(RouteBlocked);
        const auto Retry=Companion->FollowVehicleRoutePause;
        const auto OriginalSamples=Companion->FollowRoute;
        if (!TestTrue(TEXT("实体车的受阻安排已经记录原主人车辆和重试时间"),
            Retry.OwnerBike.Get()==OwnerBike && Retry.Revision==RidingFollowRevision && Retry.RetryAt>World->GetTimeSeconds()) ||
            !ExecuteFollowReply(EDeliveryCompanionTravelMode::Unspecified)) return false;
        TestTrue(TEXT("重复跟随保留原命令、车座、车迹与原受阻重试"),
            Companion->GetCommandRevision()==RidingFollowRevision && Companion->GetMode()==EDeliveryCompanionMode::Follow &&
            Companion->FollowTravelMode==EDeliveryCompanionTravelMode::OwnVehicle && Npc->IsMountedOnCompanionBike() &&
            Companion->VehicleTravelPurpose==EDeliveryCompanionTravelPurpose::Follow && Companion->VehicleLegRevision==RidingFollowRevision &&
            Companion->GetSuspensionReason()==RouteBlocked && Companion->FollowRoute==OriginalSamples &&
            Companion->FollowVehicleRoutePause.OwnerBike==Retry.OwnerBike && Companion->FollowVehicleRoutePause.RetryAt==Retry.RetryAt &&
            Companion->NextVehicleNavigationAt==Retry.RetryAt && Agent->GetPublicState().Reason==TEXT("CompanionFollowContinued"));

        const FVector DriveGoal=RoadCenter+FVector(350,0,0);
        if (!TestTrue(TEXT("安全地面上接着接受真实行驶路段"),OwnBike->TrySetDirectFollowDestination(DriveGoal))) return false;
        const FVector AcceptedDriveGoal=OwnBike->DriveGoal;
        Companion->bVehicleHasDriveGoal=true;
        Companion->FollowVehicleRoutePause={};
        Companion->SetSuspension(NAME_None);
        if (!ExecuteFollowReply(EDeliveryCompanionTravelMode::Unspecified)) return false;
        TestTrue(TEXT("行驶中重复跟随保留实体车的移动路段"),Npc->IsMountedOnCompanionBike() &&
            OwnBike->HasActiveDriveRoute() && OwnBike->DriveGoal.Equals(AcceptedDriveGoal) && Companion->bVehicleHasDriveGoal &&
            Companion->GetCommandRevision()==RidingFollowRevision);

        if (!ExecuteFollowReply(EDeliveryCompanionTravelMode::Walk)) return false;
        TestTrue(TEXT("明确改步行走原改派规则并停止旧车路"),Companion->GetCommandRevision()>RidingFollowRevision &&
            Companion->GetMode()==EDeliveryCompanionMode::Follow && Companion->FollowTravelMode==EDeliveryCompanionTravelMode::Walk &&
            !Companion->ShouldRideForFollow(OwnerBike) && !OwnBike->HasActiveDriveRoute() && !Npc->IsMountedOnCompanionBike());
    }
    if (!TestTrue(TEXT("换回原骑车偏好以继续检查主人下车"),Companion->ApplyCommand(EDeliveryNpcAction::Follow,
        Owner->PlayerState,Companion->GetCommandRevision(),nullptr,nullptr,EDeliveryCompanionTravelMode::OwnVehicle))) return false;
    const int32 OwnerRideRevision=Companion->GetCommandRevision();
    Companion->RefreshFollowOwnerTravel(Companion->GetOwnerPawn());
    TestTrue(TEXT("真实驾驶阶段记住主人正在骑车"),Companion->bFollowOwnerWasRiding &&
        !Companion->bFollowWalkAfterOwnerRide && Companion->ShouldRideForFollow(OwnerBike));
    Owner->UnPossess();
    TestNull(TEXT("控制交接可以短暂没有主人 Pawn"),Companion->GetOwnerPawn());
    TestTrue(TEXT("主人控制交接期间省略车辆事实"),SnapshotOwnerVehicle(Owner->PlayerState).IsEmpty());
    Companion->RefreshFollowOwnerTravel(Companion->GetOwnerPawn());
    TestTrue(TEXT("空 Pawn 保留最后一次真实驾驶"),Companion->bFollowOwnerWasRiding && !Companion->bFollowWalkAfterOwnerRide);
    Owner->Possess(OwnerBike);
    OwnerBike->ExitVehicle();
    if (!TestTrue(TEXT("下车后主人重新控制原身体"),Owner->GetPawn()==DrivingCharacter)) return false;
    TestEqual(TEXT("真实下车后主人请求读到步行"),SnapshotOwnerVehicle(Owner->PlayerState),FString(TEXT("OnFoot")));
    Companion->RefreshFollowOwnerTravel(Companion->GetOwnerPawn());
    TestTrue(TEXT("真实下车后保留骑车偏好但本段跟随主人步行"),Companion->bFollowWalkAfterOwnerRide &&
        !Companion->ShouldRideForFollow(DrivingCharacter) && Companion->FollowTravelMode==EDeliveryCompanionTravelMode::OwnVehicle &&
        Companion->GetCommandRevision()==OwnerRideRevision && Companion->GetOwnerPlayerState()==Owner->PlayerState);
    CheckFollowSnapshot(EDeliveryCompanionTravelMode::OwnVehicle);

    if (!TestTrue(TEXT("同一主人可重新驾驶原车辆"),OwnerBike->TryEnter(DrivingCharacter) && Owner->GetPawn()==OwnerBike)) return false;
    Companion->RefreshFollowOwnerTravel(Companion->GetOwnerPawn());
    TestTrue(TEXT("重新占有车辆解除临时步行且恢复骑车跟随"),!Companion->bFollowWalkAfterOwnerRide &&
        Companion->ShouldRideForFollow(OwnerBike) && Companion->GetCommandRevision()==OwnerRideRevision);
    OwnerBike->ExitVehicle();
    Companion->RefreshFollowOwnerTravel(Companion->GetOwnerPawn());
    TestTrue(TEXT("再次真实下车重新进入临时步行"),Owner->GetPawn()==DrivingCharacter && Companion->bFollowWalkAfterOwnerRide);
    TestTrue(TEXT("主人仍步行时可明确重申骑专属车跟随"),Companion->ApplyCommand(EDeliveryNpcAction::Follow,
        Owner->PlayerState,Companion->GetCommandRevision(),nullptr,nullptr,EDeliveryCompanionTravelMode::OwnVehicle));
    TestTrue(TEXT("明确重骑解除临时步行并开始新版本"),!Companion->bFollowWalkAfterOwnerRide &&
        Companion->ShouldRideForFollow(DrivingCharacter) && Companion->GetCommandRevision()>OwnerRideRevision);
    Owner->Possess(OriginalWalkingPawn);
    DrivingCharacter->Destroy();
    OwnerBike->Destroy();
    {
        // 复用正式房屋的相对门洞和室外锚点，远离当前出行测试的角色与车辆
        const FVector Porch(15000,15000,1000);
        const FVector OutsideFeet=Porch+FVector(0,-80,0);
        const FVector InsideFeet=Porch+FVector(0,290,0);
        const FVector OtherFloor(0,0,220);
        auto* Door=World->SpawnActor<ADeliveryBreakableDoor>(Porch+FVector(0,108,324),FRotator(0,90,0),TravelSpawn);
        if (!TestNotNull(TEXT("创建爷爷家实体门"),Door) ||
            !TestTrue(TEXT("完整门使用正式网格"),IsValid(Door->GetIntactMesh()->GetStaticMesh()))) return false;
        const FTransform GrandpaTransform(InsideFeet+FVector(100,50,58));
        auto* Grandpa=World->SpawnActorDeferred<ADeliveryGrandpa>(ADeliveryGrandpa::StaticClass(),GrandpaTransform,
            nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if (!TestNotNull(TEXT("创建收件人"),Grandpa)) return false;
        Grandpa->FrontDoor=Door;
        Grandpa->AIControllerClass=nullptr;
        Grandpa->FinishSpawning(GrandpaTransform);
        const FTransform HomeTransform(Porch);
        auto* Home=World->SpawnActorDeferred<ADeliveryHome>(ADeliveryHome::StaticClass(),HomeTransform,
            nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if (!TestNotNull(TEXT("创建唯一共用房屋"),Home)) return false;
        Home->FormerResident=Grandpa;
        Grandpa->SharedHome=Home;
        Home->FinishSpawning(HomeTransform);
        if (!TestEqual(TEXT("门洞来自当前世界的实际房屋"),ADeliveryHome::FindSharedHome(Companion),Home)) return false;
        auto* DoorFollower=MakeWalkingCharacter(OutsideFeet+FVector(0,0,88));
        if (!TestNotNull(TEXT("创建跟随测试中的步行主人"),DoorFollower)) return false;
        ON_SCOPE_EXIT
        {
            Owner->Possess(OriginalWalkingPawn);
            DoorFollower->Destroy(); Home->Destroy(); Grandpa->Destroy(); Door->Destroy();
        };
        Owner->Possess(DoorFollower);
        const auto PositionOwner=[&](const FVector& Feet)
        {
            const double HalfHeight=DoorFollower->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
            DoorFollower->SetActorLocation(Feet+FVector(0,0,HalfHeight));
        };
        PositionOwner(OutsideFeet);
        TestFalse(TEXT("完整门关闭实际房屋导航连接"),Home->DoorwayLink->IsEnabled());
        TestFalse(TEXT("两人都在门外不构成隔门跟随"),Companion->IsAcrossClosedFollowDoorway(DoorFollower,Porch));
        PositionOwner(InsideFeet);
        TestTrue(TEXT("同层主人在内且二狗在外时认出完整门挡路"),Companion->IsAcrossClosedFollowDoorway(DoorFollower,OutsideFeet));
        auto* OriginalTuning=Companion->Tuning.Get();
        auto* WiderClearance=NewObject<UDeliveryCompanionTuning>(Companion);
        WiderClearance->DestinationClearance=400.f;
        Companion->Tuning=WiderClearance;
        TestTrue(TEXT("增大入口净空后仍识别较远的门外站位"),Companion->IsAcrossClosedFollowDoorway(DoorFollower,Porch-FVector(0,460,0)));
        Companion->Tuning=OriginalTuning;
        TestFalse(TEXT("主人与二狗都已在门内时沿用常规跟随"),Companion->IsAcrossClosedFollowDoorway(DoorFollower,InsideFeet));
        TestFalse(TEXT("另一层的二狗不使用门外等待"),Companion->IsAcrossClosedFollowDoorway(DoorFollower,OutsideFeet+OtherFloor));
        const FVector BesideDoor(380,0,0);
        PositionOwner(InsideFeet+BesideDoor);
        TestFalse(TEXT("两人连线经过外墙则不算被门隔开"),Companion->IsAcrossClosedFollowDoorway(DoorFollower,OutsideFeet+BesideDoor));
        PositionOwner(InsideFeet+OtherFloor);
        TestFalse(TEXT("另一层的主人不触发本层门洞"),Companion->IsAcrossClosedFollowDoorway(DoorFollower,OutsideFeet));
        PositionOwner(InsideFeet);
        TestFalse(TEXT("完好的门不能当作开放门洞"),Companion->IsAcrossOpenFollowDoorway(DoorFollower,OutsideFeet));
        if (!TestTrue(TEXT("真实破门入口成功"),Door->BreakFromAxe())) return false;
        TestTrue(TEXT("破门事件启用实际房屋导航连接"),Home->DoorwayLink->IsEnabled());
        TestFalse(TEXT("破门后立即解除完整门条件"),Companion->IsAcrossClosedFollowDoorway(DoorFollower,OutsideFeet));
        TestTrue(TEXT("破门后同层两侧由开放门洞跟随接管"),Companion->IsAcrossOpenFollowDoorway(DoorFollower,OutsideFeet));
        TestFalse(TEXT("开放门洞不会提前认领房屋"),Home->IsClaimed());

        const FTransform SavedNpcTransform=Npc->GetActorTransform();
        const FTransform SavedBikeTransform=OwnBike->GetActorTransform();
        const auto SavedFollowMode=Companion->FollowTravelMode;
        const auto SavedFollowOwnerPawn=Companion->VehicleFollowOwnerPawn;
        const int32 SavedFollowFootRevision=Companion->FollowOnFootRevision;
        const auto SavedDeferredIndoorFollow=Companion->DeferredIndoorFollow;
        const auto SavedMode=Companion->Mode;
        const auto SavedErrand=Companion->Errand;
        const int32 SavedRevision=Companion->CommandRevision;
        const FVector SavedWaitLocation=Companion->WaitLocation;
        const double SavedFollowNavigationAt=Companion->NextNavigationAt;
        const double SavedVehicleNavigationAt=Companion->NextVehicleNavigationAt;
        const FName SavedSuspension=Companion->SuspensionReason;
        const bool bSavedFollowDestination=Companion->bHasFollowDestination;
        const bool bSavedReturning=Companion->bReturningToWaitLocation;
        const bool bSavedWaitCapture=Companion->bCaptureWaitLocationOnFoot;
        ON_SCOPE_EXIT
        {
            Companion->FollowTravelMode=SavedFollowMode;
            Companion->VehicleFollowOwnerPawn=SavedFollowOwnerPawn;
            Companion->FollowOnFootRevision=SavedFollowFootRevision;
            Companion->DeferredIndoorFollow=SavedDeferredIndoorFollow;
            Companion->Mode=SavedMode;
            Companion->Errand=SavedErrand;
            Companion->CommandRevision=SavedRevision;
            Companion->WaitLocation=SavedWaitLocation;
            Companion->NextNavigationAt=SavedFollowNavigationAt;
            Companion->NextVehicleNavigationAt=SavedVehicleNavigationAt;
            Companion->SetSuspension(SavedSuspension);
            Companion->bHasFollowDestination=bSavedFollowDestination;
            Companion->bReturningToWaitLocation=bSavedReturning;
            Companion->bCaptureWaitLocationOnFoot=bSavedWaitCapture;
            Npc->SetActorTransform(SavedNpcTransform,false,nullptr,ETeleportType::TeleportPhysics);
            OwnBike->SetActorTransform(SavedBikeTransform,false,nullptr,ETeleportType::TeleportPhysics);
        };
        const FVector NpcFeetOffset=Npc->GetActorLocation()-Npc->GetNavAgentLocation();
        Npc->SetActorLocation(InsideFeet+FVector(-145,0,0)+NpcFeetOffset,false,nullptr,ETeleportType::TeleportPhysics);
        const FVector BikeFeetOffset(0,0,OwnBike->GetSimpleCollisionHalfHeight());
        const FVector ParkedBike=Porch+FVector(0,-500,0)+BikeFeetOffset;
        OwnBike->SetActorLocation(ParkedBike,false,nullptr,ETeleportType::TeleportPhysics);
        Companion->FollowTravelMode=EDeliveryCompanionTravelMode::OwnVehicle;
        Companion->VehicleFollowOwnerPawn.Reset();
        Companion->FollowOnFootRevision=INDEX_NONE;
        const auto PrepareDeliveryErrand=[&]
        {
            Companion->Errand={};
            Companion->Errand.Action=EDeliveryNpcAction::DeliverPackage;
            Companion->Errand.Phase=EDeliveryCompanionErrandPhase::GoToDelivery;
            Companion->Errand.Status=EDeliveryNpcActionStatus::Running;
            Companion->Errand.CommandRevision=Companion->GetCommandRevision();
        };

        PositionOwner(OutsideFeet);
        Companion->ResumeIndoorFollowAfterErrand();
        TestEqual(TEXT("主人已在屋外时保留原取车安排"),Companion->FollowOnFootRevision,INDEX_NONE);

        PositionOwner(InsideFeet);
        OwnBike->SetActorLocation(InsideFeet+FVector(140,0,0)+BikeFeetOffset,false,nullptr,ETeleportType::TeleportPhysics);
        Companion->ResumeIndoorFollowAfterErrand();
        TestEqual(TEXT("专属车同在屋内时不进入临时步行"),Companion->FollowOnFootRevision,INDEX_NONE);

        OwnBike->SetActorLocation(ParkedBike,false,nullptr,ETeleportType::TeleportPhysics);
        PrepareDeliveryErrand();
        Companion->FinishErrand(EDeliveryNpcActionStatus::Succeeded,TEXT("Delivered"));
        TestEqual(TEXT("签收后继续步行跟随屋内主人"),Companion->FollowOnFootRevision,Companion->GetCommandRevision());
        TestTrue(TEXT("临时步行只绑定当前主人步行身体"),Companion->VehicleFollowOwnerPawn.Get()==DoorFollower);
        TestEqual(TEXT("签收后仍保留明确的专属车跟随偏好"),Companion->FollowTravelMode,EDeliveryCompanionTravelMode::OwnVehicle);

        PrepareDeliveryErrand();
        Owner->UnPossess();
        Companion->FinishErrand(EDeliveryNpcActionStatus::Failed,TEXT("TaskInactive"));
        const int32 DeferredRevision=Companion->GetCommandRevision();
        TestTrue(TEXT("委托失败时主人正换控制对象，记录同一主人和收尾修订"),
            Companion->DeferredIndoorFollow.Owner.Get()==Owner->PlayerState && Companion->DeferredIndoorFollow.Revision==DeferredRevision);
        Companion->UpdateFollow();
        TestEqual(TEXT("主人尚未恢复控制时保留复核"),Companion->DeferredIndoorFollow.Revision,DeferredRevision);
        Owner->Possess(DoorFollower);
        Companion->UpdateFollow();
        TestEqual(TEXT("同一主人恢复控制后继续原步行跟随"),Companion->FollowOnFootRevision,DeferredRevision);
        TestTrue(TEXT("用恢复后实际控制的身体确定临时步行"),Companion->VehicleFollowOwnerPawn.Get()==DoorFollower);
        TestEqual(TEXT("恢复控制后结束一次性复核"),Companion->DeferredIndoorFollow.Revision,INDEX_NONE);

        PrepareDeliveryErrand();
        TestTrue(TEXT("沿原方式继续跟随后结束现有委托"),Companion->ApplyCommand(EDeliveryNpcAction::Follow,
            Owner->PlayerState,Companion->GetCommandRevision()));
        TestEqual(TEXT("继续跟随使用新修订仍可在室内步行"),Companion->FollowOnFootRevision,Companion->GetCommandRevision());
        TestTrue(TEXT("同一主人可再次确认原跟随安排"),Companion->ApplyCommand(EDeliveryNpcAction::Follow,
            Owner->PlayerState,Companion->GetCommandRevision()));
        TestEqual(TEXT("同一身体仍在室内时重复跟随继续步行"),Companion->FollowOnFootRevision,Companion->GetCommandRevision());

        PrepareDeliveryErrand();
        Owner->UnPossess();
        Companion->FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("TaskCancelled"));
        Owner->Possess(DoorFollower);
        TestTrue(TEXT("明确要求骑车覆盖先前的室内复核"),Companion->ApplyCommand(EDeliveryNpcAction::Follow,
            Owner->PlayerState,Companion->GetCommandRevision(),nullptr,nullptr,EDeliveryCompanionTravelMode::OwnVehicle));
        TestTrue(TEXT("显式骑车的新修订无临时步行"),Companion->FollowOnFootRevision!=Companion->GetCommandRevision());
        TestEqual(TEXT("显式骑车清理旧复核"),Companion->DeferredIndoorFollow.Revision,INDEX_NONE);

        PrepareDeliveryErrand();
        TestTrue(TEXT("明确取消委托仍可让二狗原地等候"),Companion->ApplyCommand(EDeliveryNpcAction::CancelCompanionTask,
            Owner->PlayerState,Companion->GetCommandRevision()));
        TestEqual(TEXT("明确取消后的安排仍为等候"),Companion->GetMode(),EDeliveryCompanionMode::Stay);
        TestEqual(TEXT("明确取消清理旧复核"),Companion->DeferredIndoorFollow.Revision,INDEX_NONE);
    }
    TestTrue(TEXT("主人先明确安排骑专属车跟随"),Companion->ApplyCommand(EDeliveryNpcAction::Follow,
        Owner->PlayerState,Companion->GetCommandRevision(),nullptr,nullptr,EDeliveryCompanionTravelMode::OwnVehicle));
    TestTrue(TEXT("主人步行时仍服从明确骑专属车安排"),Companion->ShouldRideForFollow(Owner->GetPawn()));
    for (const auto Action:{EDeliveryNpcAction::Stay,EDeliveryNpcAction::CancelCompanionTask})
    {
        TestTrue(TEXT("主人可以暂停骑车跟随"),Companion->ApplyCommand(Action,Owner->PlayerState,Companion->GetCommandRevision()));
        TestEqual(TEXT("暂停后原地等候"),Companion->GetMode(),EDeliveryCompanionMode::Stay);
        TestTrue(TEXT("召回时可以沿用既定方式"),Companion->ApplyCommand(EDeliveryNpcAction::Follow,
            Owner->PlayerState,Companion->GetCommandRevision()));
        FDeliveryNpcDecisionRequest Recall;
        Companion->ExtendTravelDecisionRequest(Recall,Owner->PlayerState);
        TestEqual(TEXT("未重选方式的召回恢复骑自己的车"),Recall.CompanionTravelMode,FString(TEXT("OwnVehicle")));
    }
    for (const auto Travel:{EDeliveryCompanionTravelMode::Walk,EDeliveryCompanionTravelMode::OwnVehicle,EDeliveryCompanionTravelMode::Automatic})
    {
        FDeliveryNpcObservation TravelRequest;
        TravelRequest.EventId=FGuid::NewGuid();
        TravelRequest.Kind=TEXT("PlayerSpeech");
        TravelRequest.Subject=Owner->GetPawn();
        TravelRequest.Addressee=Npc;
        TravelRequest.Source=EDeliveryNpcEvidenceSource::Heard;
        TravelRequest.Description=TEXT("这次换一种出行方式跟着我。");
        TravelRequest.bNeedsReply=true;
        if (!TestTrue(TEXT("主人新一轮发话取得实际出行候选"),Agent->BeginListening(Owner->GetPawn(),TravelRequest.EventId) &&
            Agent->ObserveEvent(TravelRequest) && Agent->BuildDecisionRequest(FGuid::NewGuid(),TravelChoices) &&
            TravelChoices.bPlayerRequest && TravelChoices.RequesterPlayerState.Get()==Owner->PlayerState)) return false;
        TestTrue(TEXT("明确出行方式出现在实际候选中"),HasTravelCandidate(TravelChoices,EDeliveryNpcAction::Follow,Travel));
        TestTrue(TEXT("主人可以提交明确出行选择"),Companion->ApplyCommand(EDeliveryNpcAction::Follow,
            Owner->PlayerState,Companion->GetCommandRevision(),nullptr,nullptr,Travel));
        TestEqual(TEXT("服务器记录主人选择的跟随方式"),Companion->FollowTravelMode,Travel);
        TestEqual(TEXT("主人步行时只有明确专属车安排继续骑行"),
            Companion->ShouldRideForFollow(Owner->GetPawn()),Travel==EDeliveryCompanionTravelMode::OwnVehicle);
        CheckFollowSnapshot(Travel);
        TestFalse(TEXT("访客不能改主人的出行方式"),Companion->ApplyCommand(EDeliveryNpcAction::Follow,
            Stranger->PlayerState,Companion->GetCommandRevision(),nullptr,nullptr,EDeliveryCompanionTravelMode::Walk));
        TestEqual(TEXT("访客被拒后跟随方式不变"),Companion->FollowTravelMode,Travel);
    }
    Agent->CancelPlan(TEXT("TravelChoiceTest"));
    Companion->SetSuspension(TEXT("PathBlocked"));
    TestTrue(TEXT("步行准备允许继续检查跟随路径"),Companion->UpdateVehicleTravel(Owner->GetPawn()->GetActorLocation(),150.f,
        EDeliveryCompanionTravelPurpose::Follow));
    TestEqual(TEXT("已经在步行不代表受阻路线已经恢复"),Companion->GetSuspensionReason(),FName(TEXT("PathBlocked")));
    Companion->SetSuspension(TEXT("CompanionDismountBlocked"));
    TestTrue(TEXT("确认落地后可以执行步行"),Companion->UpdateVehicleTravel(Owner->GetPawn()->GetActorLocation(),150.f,
        EDeliveryCompanionTravelPurpose::Follow));
    TestTrue(TEXT("落地后清掉下车暂停"),Companion->GetSuspensionReason().IsNone());
    {
        APawn* WalkingOwner=Owner->GetPawn();
        auto* Controller=Cast<AAIController>(Npc->GetController());
        auto* Following=Controller?Controller->GetPathFollowingComponent():nullptr;
        if (!TestNotNull(TEXT("步行跟随保留主人的当前角色"),WalkingOwner) ||
            !TestNotNull(TEXT("步行跟随使用自己的路径组件"),Following) ||
            !TestTrue(TEXT("准备步行测试前没有活动路径"),Controller->GetMoveStatus()==EPathFollowingStatus::Idle)) return false;
        const FVector SavedNpcLocation=Npc->GetActorLocation();
        const FVector SavedOwnerLocation=WalkingOwner->GetActorLocation();
        const FVector SavedGoal=Companion->LastDestination;
        const FVector SavedOwnerCoordinate=Companion->LastOwnerPosition;
        const double SavedNavigationAt=Companion->NextNavigationAt;
        const bool bSavedReported=Companion->bReportedBlocked;
        const bool bSavedDestination=Companion->bHasFollowDestination;
        AActor* Occupant=nullptr;
        ON_SCOPE_EXIT
        {
            Companion->StopFollowNavigation();
            Companion->ClearFollowMoveWatch();
            Companion->RejectedFollowStands.Reset();
            Companion->RejectedFollowOwnerPawn.Reset();
            if (Occupant) Occupant->Destroy();
            Companion->bHasFollowDestination=bSavedDestination;
            Companion->bReportedBlocked=bSavedReported;
            Companion->LastDestination=SavedGoal;
            Companion->LastOwnerPosition=SavedOwnerCoordinate;
            Companion->NextNavigationAt=SavedNavigationAt;
            Companion->SetSuspension(NAME_None);
            WalkingOwner->SetActorLocation(SavedOwnerLocation,false,nullptr,ETeleportType::TeleportPhysics);
            Npc->SetActorLocation(SavedNpcLocation,false,nullptr,ETeleportType::TeleportPhysics);
        };
        const FVector Start(19000,19000,1000);
        WalkingOwner->SetActorLocation(Start+FVector(800,0,0),false,nullptr,ETeleportType::TeleportPhysics);
        Npc->SetActorLocation(Start,false,nullptr,ETeleportType::TeleportPhysics);
        const FVector Feet=Npc->GetNavAgentLocation();
        const FVector CachedGoal=Feet+FVector(550,0,0);
        // 独立测试世界没有烘焙导航，这里只向实际路径组件交付已选好的两点步行路线
        const auto StartOwnedMove=[&](const FVector& Goal)
        {
            FAIMoveRequest Move(Goal);
            Move.SetAcceptanceRadius(40.f);
            const auto WalkPath=MakeShared<FNavigationPath,ESPMode::ThreadSafe>(TArray<FVector>{Feet,Goal});
            const FAIRequestID Id=Following->RequestMove(Move,WalkPath);
            Companion->bOwnsNavigation=Id.IsValid();
            Companion->FollowMoveId=Id;
            Companion->bHasFollowDestination=Id.IsValid();
            Companion->LastDestination=Goal;
            Companion->LastOwnerPosition=WalkingOwner->GetActorLocation();
            if (Id.IsValid()) Companion->WatchFollowMove(*Following,Id,*WalkingOwner,Goal);
            return Id;
        };
        const FAIRequestID CachedMove=StartOwnedMove(CachedGoal);
        if (!TestTrue(TEXT("清空站位已经有实际移动请求"),CachedMove.IsValid() &&
            Controller->GetMoveStatus()==EPathFollowingStatus::Moving)) return false;
        Companion->NextNavigationAt=0;
        Companion->UpdateFollow();
        TestTrue(TEXT("原站位净空时保留同一个移动请求"),Following->GetCurrentRequestId()==CachedMove);

        const double HalfHeight=Npc->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
        Occupant=MakeRoadBox(CachedGoal+FVector(0,0,HalfHeight),FVector(50,50,HalfHeight));
        if (!TestFalse(TEXT("新物体真实占住旧站位"),Companion->IsDestinationClear(CachedGoal))) return false;
        Companion->bReportedBlocked=true;
        Companion->NextNavigationAt=0;
        Companion->UpdateFollow();
        TestTrue(TEXT("站位被占后解除仍在移动的旧请求"),Following->GetCurrentRequestId()!=CachedMove &&
            Controller->GetMoveStatus()==EPathFollowingStatus::Idle && !Companion->bHasFollowDestination);
        TestEqual(TEXT("没有其他可用站位时保留跟随并报告受阻"),Companion->GetSuspensionReason(),FName(TEXT("PathBlocked")));
        TestFalse(TEXT("二狗主动放弃被占站位不登记途中受阻"),Companion->IsRejectedFollowStand(*WalkingOwner,CachedGoal));
        Occupant->Destroy();
        Occupant=nullptr;

        {
            const int32 HitRevision=Companion->GetCommandRevision();
            const FVector WaitBeforeHit=Companion->WaitLocation;
            const FAIRequestID InterruptedFollow=StartOwnedMove(CachedGoal);
            if (!TestTrue(TEXT("轻击前二狗正在执行自己的跟随请求"),InterruptedFollow.IsValid() &&
                Following->GetStatus()==EPathFollowingStatus::Moving)) return false;
            auto* Attacker=Stranger->GetPawn<APawn>();
            if (!TestNotNull(TEXT("轻击来自同世界另一名玩家"),Attacker)) return false;
            Npc->Combat->ReceiveFightHit(Attacker,FGuid::NewGuid(),true);
            if (!TestTrue(TEXT("真实轻击进入短后仰且保持站立"),Npc->Combat->IsReactingToHit() && !Npc->IsKnockedDown())) return false;
            const double OldRetryAt=World->GetTimeSeconds()+15.;
            Companion->NextNavigationAt=OldRetryAt;
            Companion->UpdateFollow();
            TestEqual(TEXT("短后仰期间持续跟随暂停"),Companion->GetSuspensionReason(),FName(TEXT("HitReaction")));
            TestTrue(TEXT("短后仰期间没有重发跟随移动"),Following->GetStatus()==EPathFollowingStatus::Idle &&
                Following->GetCurrentRequestId()!=InterruptedFollow);
            TestTrue(TEXT("受击保留同一主人和原安排"),Companion->CanCommand(Owner->PlayerState) &&
                Companion->GetMode()==EDeliveryCompanionMode::Follow && Companion->GetCommandRevision()==HitRevision &&
                Companion->WaitLocation.Equals(WaitBeforeHit));

            // 只推进独立测试世界的时间，让真实后仰结束，其他 Actor 不在这段时间自行跑逻辑
            for (int32 Step=0;Step<8 && Npc->Combat->IsReactingToHit();++Step)
            {
                ++GFrameCounter;
                World->Tick(LEVELTICK_TimeOnly,.1f);
            }
            if (!TestFalse(TEXT("等待原受击时长后结束短后仰"),Npc->Combat->IsReactingToHit())) return false;
            // 无导航网格的夹具提供已选好的有效路线，持续分支负责恢复检查和保留编号
            const FAIRequestID RestoredFollow=StartOwnedMove(CachedGoal);
            if (!TestTrue(TEXT("同一安排下已准备新的有效跟随请求"),RestoredFollow.IsValid() &&
                RestoredFollow!=InterruptedFollow)) return false;
            Companion->UpdateFollow();
            TestTrue(TEXT("后仰结束立即释放旧重试时间并继续当前跟随路径"),Companion->NextNavigationAt<OldRetryAt &&
                Following->GetCurrentRequestId()==RestoredFollow && Following->GetStatus()==EPathFollowingStatus::Moving &&
                Companion->GetSuspensionReason().IsNone());
            TestTrue(TEXT("恢复跟随时无需主人重新命令"),Companion->GetMode()==EDeliveryCompanionMode::Follow &&
                Companion->GetCommandRevision()==HitRevision && Companion->WaitLocation.Equals(WaitBeforeHit));
            Companion->StopFollowNavigation();
        }

        {
            const auto SavedVehiclePurpose=Companion->VehicleTravelPurpose;
            const bool bSavedDriveGoal=Companion->bVehicleHasDriveGoal;
            const bool bSavedMounted=OwnBike->bMounted;
            const bool bSavedDismountPending=OwnBike->bDismountPending;
            const FTransform SavedWalkingPose=Npc->GetActorTransform();
            const int32 RidingRevision=Companion->GetCommandRevision();
            ON_SCOPE_EXIT
            {
                if (Owner->GetPawn()!=WalkingOwner) Owner->Possess(WalkingOwner);
                OwnBike->bMounted=bSavedMounted;
                OwnBike->bDismountPending=bSavedDismountPending;
                OwnBike->ApplyCompanionPresentation();
                Npc->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
                Npc->SetActorTransform(SavedWalkingPose,false,nullptr,ETeleportType::TeleportPhysics);
                Companion->VehicleTravelPurpose=SavedVehiclePurpose;
                Companion->bVehicleHasDriveGoal=bSavedDriveGoal;
                Companion->SetSuspension(NAME_None);
            };
            if (!TestTrue(TEXT("主人换控制前二狗已附着在实际专属车座"),Npc->AttachToComponent(OwnBike->CompanionSeat,
                FAttachmentTransformRules::SnapToTargetNotIncludingScale))) return false;
            OwnBike->bMounted=true;
            Companion->VehicleTravelPurpose=EDeliveryCompanionTravelPurpose::Follow;
            Companion->bVehicleHasDriveGoal=true;
            Owner->UnPossess();
            if (!TestTrue(TEXT("同一个在线主人确实经过无 Pawn 的瞬间"),!Owner->GetPawn() &&
                Companion->CanCommand(Owner->PlayerState))) return false;
            Companion->UpdateFollow();
            TestEqual(TEXT("没有主人 Pawn 时先暂停持续驾驶"),Companion->GetSuspensionReason(),FName(TEXT("OwnerChangingPawn")));
            TestTrue(TEXT("换控制暂时停旧车路线但保留实际骑乘"),!Companion->bVehicleHasDriveGoal && OwnBike->IsMounted() &&
                !OwnBike->bDismountPending && Npc->GetRootComponent()->GetAttachParent()==OwnBike->CompanionSeat.Get());
            TestTrue(TEXT("进入新控制对象前主人与命令仍有效"),Companion->GetMode()==EDeliveryCompanionMode::Follow &&
                Companion->GetCommandRevision()==RidingRevision);

            Owner->Possess(WalkingOwner);
            Npc->Combat->ReceiveFightHit(Stranger->GetPawn<APawn>(),FGuid::NewGuid(),true);
            if (!TestTrue(TEXT("原座位上进入实际轻击后仰"),Npc->Combat->IsReactingToHit() && !Npc->IsKnockedDown())) return false;
            OwnBike->Tick(.016f);
            TestTrue(TEXT("车辆自己的更新也只停车并保留原座"),OwnBike->IsMounted() && !OwnBike->bDismountPending &&
                Npc->GetRootComponent()->GetAttachParent()==OwnBike->CompanionSeat.Get());
            for (int32 Step=0;Step<8 && Npc->Combat->IsReactingToHit();++Step)
            {
                ++GFrameCounter;
                World->Tick(LEVELTICK_TimeOnly,.1f);
            }
        }

        {
            auto* Capsule=Npc->GetCapsuleComponent();
            auto* Movement=Npc->GetCharacterMovement();
            const ECollisionChannel CapsuleChannel=Capsule->GetCollisionObjectType();
            const ECollisionResponse SavedVehicleResponse=Capsule->GetCollisionResponseToChannel(ECC_Vehicle);
            const EMovementMode SavedMovementMode=Movement->MovementMode;
            const uint8 SavedCustomMovementMode=Movement->CustomMovementMode;
            const FVector SavedVelocity=Movement->Velocity;
            ON_SCOPE_EXIT
            {
                Companion->StopFollowNavigation();
                if (Occupant)
                {
                    Occupant->Destroy();
                    Occupant=nullptr;
                }
                Capsule->SetCollisionResponseToChannel(ECC_Vehicle,SavedVehicleResponse);
                Movement->SetMovementMode(SavedMovementMode,SavedCustomMovementMode);
                Movement->Velocity=SavedVelocity;
            };
            Movement->SetMovementMode(MOVE_Walking);
            Movement->Velocity=FVector::ZeroVector;
            Capsule->SetCollisionResponseToChannel(ECC_Vehicle,ECR_Block);
            const FVector BlockerLocation=Feet+FVector(Capsule->GetScaledCapsuleRadius()+35.,0,HalfHeight);
            const FVector ClearLocation=BlockerLocation+FVector(0,400,0);
            Occupant=MakeRoadBox(BlockerLocation,FVector(10,45,HalfHeight));
            auto* Blocker=Cast<UBoxComponent>(Occupant->GetRootComponent());
            if (!TestNotNull(TEXT("途中避让测试使用有实际碰撞的箱体"),Blocker)) return false;
            Blocker->SetCollisionObjectType(ECC_Vehicle);
            Blocker->SetCollisionResponseToChannel(CapsuleChannel,ECR_Block);
            TestTrue(TEXT("途中有挡路车辆时旧站位仍然净空"),Companion->IsDestinationClear(CachedGoal));

            const FAIRequestID ObstructedMove=StartOwnedMove(CachedGoal);
            if (!TestTrue(TEXT("采样使用正在步行的二狗和自己发出的路径"),ObstructedMove.IsValid() &&
                Following->GetStatus()==EPathFollowingStatus::Moving && Movement->IsMovingOnGround())) return false;
            const double SampleStart=World->GetTimeSeconds();
            const auto SampleObstruction=[&](double Elapsed)
            {
                return Companion->SampleFollowMoveObstruction(*Following,*WalkingOwner,SampleStart+Elapsed);
            };
            TestNull(TEXT("首次测到真实挡路车辆时仍等待移动"),SampleObstruction(0.));
            TestNull(TEXT("短暂挡路时不会提前报告受阻"),SampleObstruction(.4));
            TestTrue(TEXT("二狗持续低速且车辆仍在途中时返回实际阻挡组件"),SampleObstruction(.8)==Blocker);

            Occupant->SetActorLocation(ClearLocation);
            TestNull(TEXT("挡路车辆移到路径外时清除受阻"),SampleObstruction(1.));
            Occupant->SetActorLocation(BlockerLocation);
            TestNull(TEXT("车辆横穿重新开始计时"),SampleObstruction(2.));
            Occupant->SetActorLocation(ClearLocation);
            TestNull(TEXT("车辆短暂横穿离开时保持当前路径"),SampleObstruction(2.3));

            Occupant->SetActorLocation(BlockerLocation);
            Blocker->SetCollisionResponseToChannel(CapsuleChannel,ECR_Overlap);
            TestNull(TEXT("仅重叠的车辆不能登记为挡路"),SampleObstruction(3.));
            TestNull(TEXT("持续重叠同样不会报告受阻"),SampleObstruction(4.));
            Blocker->SetCollisionResponseToChannel(CapsuleChannel,ECR_Block);
            TestNull(TEXT("车辆恢复阻挡后重新计时"),SampleObstruction(5.));
            const FAIRequestID ReplacementMove=StartOwnedMove(CachedGoal);
            if (!TestTrue(TEXT("换路后新的请求有自己的编号"),ReplacementMove.IsValid() && ReplacementMove!=ObstructedMove)) return false;
            TestNull(TEXT("新路径不会继承旧请求的受阻时间"),SampleObstruction(6.));
            TestTrue(TEXT("新路径达到等待时间后仍能识别挡路车辆"),SampleObstruction(6.8)==Blocker);
        }

        const FVector BlockedGoal=CachedGoal+FVector(0,160,0);
        const FVector AbortedGoal=CachedGoal+FVector(0,-160,0);
        const FAIRequestID BlockedMove=StartOwnedMove(BlockedGoal);
        if (!TestTrue(TEXT("途中受阻测试有正在执行的自己路径"),BlockedMove.IsValid() &&
            Following->GetCurrentRequestId()==BlockedMove && Controller->GetMoveStatus()==EPathFollowingStatus::Moving)) return false;
        const FPathFollowingResult BlockedPath(EPathFollowingResult::Blocked,FPathFollowingResultFlags::None);
        Following->OnRequestFinished.Broadcast(CachedMove,BlockedPath);
        Following->OnRequestFinished.Broadcast(FAIRequestID(BlockedMove.GetID()+100),BlockedPath);
        TestTrue(TEXT("旧请求和无关请求的受阻通知不改变当前路径"),Companion->bOwnsNavigation &&
            Companion->FollowMoveId==BlockedMove && Following->GetCurrentRequestId()==BlockedMove && Companion->bHasFollowDestination);
        TestFalse(TEXT("旧请求和无关请求不排除当前目标"),Companion->IsRejectedFollowStand(*WalkingOwner,BlockedGoal));

        // 用路径组件原生入口结束真实的活动请求，回调发生时当前路径编号已清除
        Following->OnPathFinished(BlockedPath);
        TestTrue(TEXT("活动请求受阻后移除自己的步行归属"),Controller->GetMoveStatus()==EPathFollowingStatus::Idle &&
            !Companion->bOwnsNavigation && !Companion->FollowMoveId.IsValid() && !Companion->bHasFollowDestination);
        TestTrue(TEXT("活动请求受阻后短时避开原站位"),Companion->IsRejectedFollowStand(*WalkingOwner,BlockedGoal));
        TestFalse(TEXT("途中受阻不排除另一侧站位"),Companion->IsRejectedFollowStand(*WalkingOwner,AbortedGoal));
        TestEqual(TEXT("途中受阻保留持续跟随"),Companion->GetMode(),EDeliveryCompanionMode::Follow);
        TestEqual(TEXT("途中受阻报告路径受阻"),Companion->GetSuspensionReason(),FName(TEXT("PathBlocked")));

        const FAIRequestID AbortedMove=StartOwnedMove(AbortedGoal);
        if (!TestTrue(TEXT("主动取消测试有自己的活动路径"),AbortedMove.IsValid() &&
            Following->GetCurrentRequestId()==AbortedMove)) return false;
        Following->AbortMove(*Companion,FPathFollowingResultFlags::UserAbort,AbortedMove);
        Following->OnRequestFinished.Broadcast(AbortedMove,BlockedPath);
        TestFalse(TEXT("主动取消及之后晚到的受阻通知不排除站位"),Companion->IsRejectedFollowStand(*WalkingOwner,AbortedGoal));
        TestTrue(TEXT("主动取消保留之前独立记录的受阻站位"),Companion->IsRejectedFollowStand(*WalkingOwner,BlockedGoal));

        const FVector PreviousPawnGoal=CachedGoal+FVector(0,320,0);
        const FAIRequestID PreviousPawnMove=StartOwnedMove(PreviousPawnGoal);
        if (!TestTrue(TEXT("切换主人角色前有自己的活动路径"),PreviousPawnMove.IsValid())) return false;
        auto* ReplacementPawn=MakePawn(WalkingOwner->GetActorLocation());
        Owner->Possess(ReplacementPawn);
        Following->OnPathFinished(BlockedPath);
        Owner->Possess(WalkingOwner);
        ReplacementPawn->Destroy();
        TestFalse(TEXT("主人换角色期间的旧受阻不写入排除记录"),Companion->IsRejectedFollowStand(*WalkingOwner,PreviousPawnGoal));
    }
    Companion->ApplyTravelInstruction(EDeliveryCompanionTravelMode::OwnVehicle);
    FDeliveryNpcDecisionRequest AfterErrandChoice;
    Companion->ExtendTravelDecisionRequest(AfterErrandChoice,Owner->PlayerState);
    TestEqual(TEXT("单独指定骑车跑腿仍保留自动跟随"),AfterErrandChoice.CompanionTravelMode,FString(TEXT("Automatic")));
    TestEqual(TEXT("单独指定骑车跑腿出现在对应快照字段"),AfterErrandChoice.CompanionErrandTravelMode,FString(TEXT("OwnVehicle")));
    Companion->ApplyTravelInstruction(EDeliveryCompanionTravelMode::Automatic);
    for (const auto Action:{EDeliveryNpcAction::Stay,EDeliveryNpcAction::CancelCompanionTask})
    {
        TestTrue(TEXT("等候和取消仍可接收"),Companion->ApplyCommand(Action,Owner->PlayerState,Companion->GetCommandRevision()));
        TestEqual(TEXT("等候和取消不清出行选择"),Companion->GetTravelMode(),EDeliveryCompanionTravelMode::Automatic);
    }
    APawn* PreviousOwnerPawn=Owner->GetPawn();
    auto* SwappedPawn=MakePawn(FVector(100,0,88));
    Owner->Possess(SwappedPawn);
    TestEqual(TEXT("同PlayerState换Pawn保留出行选择"),Companion->GetTravelMode(),EDeliveryCompanionTravelMode::Automatic);
    Owner->Possess(PreviousOwnerPawn); SwappedPawn->Destroy();
    Companion->ErrandVehicle=nullptr; OwnBike->Destroy();

    // 同一世界补一份真实持件状态，只测远处接货不能借未指定方式直接出发
    auto* TravelState=World->SpawnActor<AGameStateBase>(); World->SetGameState(TravelState);
    auto* TravelTasks=NewObject<UDeliveryTaskManagerComponent>(TravelState); TravelState->AddInstanceComponent(TravelTasks);
    auto* TravelTask=NewObject<UDeliveryTaskDefinition>(TravelState);
    TravelTask->TaskId=TEXT("CompanionTravel_CarriedPackage"); TravelTask->TimeLimitSeconds=120;
    auto* TravelDefinitions=FindFProperty<FArrayProperty>(TravelTasks->GetClass(),TEXT("TaskDefinitions"));
    if (!TestNotNull(TEXT("复用原任务配置字段"),TravelDefinitions)) return false;
    TravelDefinitions->ContainerPtrToValuePtr<TArray<TObjectPtr<UDeliveryTaskDefinition>>>(TravelTasks)->Add(TravelTask);
    TravelTasks->RegisterComponent();
    if (!TestTrue(TEXT("远处接货测试使用真实接单入口"),TravelTasks->StartTaskTimerFromAcceptedCall(TravelTask))) return false;
    auto* CarryingPlayer=MakeWalkingCharacter(FVector(2000,0,88));
    if (!TestNotNull(TEXT("创建真实持件玩家"),CarryingPlayer)) return false;
    Owner->Possess(CarryingPlayer);
    CarryingPlayer->SetActorLocation(FVector(2000,0,88),false,nullptr,ETeleportType::TeleportPhysics);
    auto* TravelPackage=World->SpawnActor<ADeliveryTestPackage>(FVector(2000,0,150),FRotator::ZeroRotator,TravelSpawn);
    if (!TestNotNull(TEXT("创建同一份实际任务包裹"),TravelPackage)) return false;
    TravelPackage->GetDeliveryItemComponent()->OwningTask=TravelTask;
    if (!TestTrue(TEXT("真实接收接口写入持件关系"),CarryingPlayer->TryReceiveTaskPackage(TravelPackage))) return false;
    Companion->TravelMode=EDeliveryCompanionTravelMode::Unspecified;
    TestTrue(TEXT("真实持件人确实远于就近豁免"),FVector::Dist(Npc->GetActorLocation(),CarryingPlayer->GetTaskPackageTransferPoint())>240.f);
    TestTrue(TEXT("远处持件仍允许提出接件委托"),Companion->CanArrangeErrand(EDeliveryNpcAction::TakePackage,Owner->PlayerState));
    TestTrue(TEXT("主人询问远处接货"),Agent->BeginListening(CarryingPlayer,FGuid::NewGuid()));
    TestTrue(TEXT("远处接货构造真实候选"),Agent->BuildDecisionRequest(FGuid::NewGuid(),TravelChoices));
    TestFalse(TEXT("未指定方式的远处Take不能直接出发"),HasTravelCandidate(TravelChoices,EDeliveryNpcAction::TakePackage,EDeliveryCompanionTravelMode::Unspecified));
    TestTrue(TEXT("主人仍可明确选步行接货"),HasTravelCandidate(TravelChoices,EDeliveryNpcAction::TakePackage,EDeliveryCompanionTravelMode::Walk));
    Agent->CancelPlan(TEXT("TravelTestFinished"));
    CarryingPlayer->ReleaseTaskPackage(TravelPackage); TravelPackage->Destroy();

    const FVector LeadPoint=Npc->GetActorLocation()+FVector(180,400,0);
    CarryingPlayer->SetActorLocation(LeadPoint+FVector(110,0,0),false,nullptr,ETeleportType::TeleportPhysics);
    auto* Tool=World->SpawnActor<ADeliveryHandheldItem>(LeadPoint,FRotator::ZeroRotator,TravelSpawn);
    if (!TestNotNull(TEXT("创建用于核对同一弱引用的道具"),Tool)) return false;
    Tool->GetItemComponent()->ItemType=EDeliveryInventoryItemType::BackpackItem;
    Tool->GetItemComponent()->DisplayName=FText::FromString(TEXT("测试锤子"));
    Tool->GetItemComponent()->bCompanionCanFetch=true;
    // 从已经失视的旧位置开始，测试本轮凭据隔离，不模拟实际拿取或画面
    auto& Seen=Companion->SeenItems.AddDefaulted_GetRef();
    Seen.Item=Tool; Seen.Location=LeadPoint; Seen.SeenAt=World->GetTimeSeconds();
    TestTrue(TEXT("主人可以为旧位置发出请求"),Agent->BeginListening(CarryingPlayer,FGuid::NewGuid()));
    FDeliveryNpcDecisionRequest LeadRequest;
    if (!TestTrue(TEXT("构造含有附近线索的主人请求"),Agent->BuildDecisionRequest(FGuid::NewGuid(),LeadRequest))) return false;
    const auto* LeadOption=LeadRequest.Candidates.FindByPredicate([](const FDeliveryNpcCandidate& Option)
        { return Option.Action==EDeliveryNpcAction::FetchItem && Option.ItemChoice.IsValid() && Option.ItemChoice->bSearchLead; });
    if (!TestNotNull(TEXT("实际请求含有绑定原物的找物候选"),LeadOption)) return false;
    const auto& LeadChoice=*LeadOption->ItemChoice;
    TestTrue(TEXT("线索进入当前请求"),LeadRequest.Evidence.ContainsByPredicate([&LeadChoice](const FDeliveryNpcEvidence& Fact)
        { return Fact.EventId==LeadChoice.EvidenceId && Fact.Kind==DeliveryCompanionItemSearch::EvidenceKind; }));
    TestFalse(TEXT("临时线索没有进入常驻证据"),Agent->HasObservationEvent(LeadChoice.EvidenceId));
    TestFalse(TEXT("临时道具没有进入已认识对象"),Agent->FindKnownSubjectId(Tool).IsValid());
    TestFalse(TEXT("访客不能使用主人原线索"),Companion->CanApplyItemChoice(LeadChoice,Stranger->PlayerState,LeadRequest.CompanionCommandRevision));
    auto ExpiredChoice=LeadChoice;
    ExpiredChoice.ObservedAt-=DeliveryCompanionItemSearch::LeadLifetime;
    TestFalse(TEXT("已过期线索不能启动"),Companion->CanApplyItemChoice(ExpiredChoice,Owner->PlayerState,LeadRequest.CompanionCommandRevision));

    Agent->ActiveRequest=LeadRequest;
    FDeliveryNpcDecisionProposal LeadReply;
    LeadReply.CapabilityVersion=LeadRequest.RequestedCapabilityVersion;
    LeadReply.CandidateId=LeadOption->CandidateId;
    LeadReply.TargetId=LeadOption->TargetId;
    LeadReply.EvidenceIds=LeadOption->EvidenceIds;
    LeadReply.WorldEpoch=LeadRequest.WorldEpoch; LeadReply.NpcId=LeadRequest.NpcId; LeadReply.RequestId=LeadRequest.RequestId;
    LeadReply.ContextRevision=LeadRequest.ContextRevision; LeadReply.PlanRevision=LeadRequest.PlanRevision;
    LeadReply.Action=EDeliveryNpcAction::FetchItem; LeadReply.Speech=TEXT("我过去找找。");
    if (!TestTrue(TEXT("模型只能采纳当轮封存的原物线索"),Agent->TryAcceptProposal(LeadReply,Reason))) return false;
    Agent->ActiveRequest={};
    FDeliveryNpcPublicState StartedLead;
    if (!TestTrue(TEXT("请求回收后行为树仍能用已采纳的线索启动"),Agent->StartAction(StartedLead))) return false;
    StartedLead.Speech.Reset();
    if (!TestTrue(TEXT("执行原凭据启动实际找物阶段"),Npc->ExecuteAction(StartedLead))) return false;
    TestTrue(TEXT("普通道具执行进入找物子树条件"),Companion->HasNearbyItemSearch());
    Agent->TryFinishAction(StartedLead.ActionId,EDeliveryNpcActionStatus::Succeeded,TEXT("CompanionErrandAccepted"));

    const auto BuildLeadOption=[&](FDeliveryNpcDecisionRequest& Request,EDeliveryCompanionTravelMode Travel)
        -> const FDeliveryNpcCandidate*
    {
        if (!Agent->BeginListening(CarryingPlayer,FGuid::NewGuid()) || !Agent->BuildDecisionRequest(FGuid::NewGuid(),Request))
            return nullptr;
        return Request.Candidates.FindByPredicate([Tool,Travel](const auto& Candidate)
        {
            return Candidate.Action==EDeliveryNpcAction::FetchItem && Candidate.TravelMode==Travel && Candidate.ItemChoice.IsValid() &&
                Candidate.ItemChoice->bSearchLead && Candidate.ItemChoice->Item.Get()==Tool;
        });
    };
    const int32 SearchRevision=Companion->GetCommandRevision();
    // 用不同的新候选重复确认同一找物安排，组装可观察的找物进度用于检查保留
    Companion->ErrandActiveSeconds=4.f;
    Companion->ItemSearchPointIndex=1;
    Companion->ItemSearchPointStartedAt=3.f;
    const auto Walk=EDeliveryCompanionTravelMode::Walk;
    const auto Preserve=EDeliveryCompanionTravelMode::Unspecified;
    for (const auto Travel:{Preserve,Walk})
    {
        FDeliveryNpcDecisionRequest RepeatRequest;
        const auto* Repeat=BuildLeadOption(RepeatRequest,Travel);
        if (!TestNotNull(TEXT("重复请求仍提供绑定同一实物的新候选"),Repeat)) return false;
        TestTrue(TEXT("重复候选为新编号"),Repeat->CandidateId!=LeadChoice.CandidateId);
        TestFalse(TEXT("旧修订不能复用当前找物"),Companion->CanKeepItemErrand(*Repeat->ItemChoice,
            Owner->PlayerState,SearchRevision-1,Travel));
        TestFalse(TEXT("访客不能复用主人的找物"),Companion->CanKeepItemErrand(*Repeat->ItemChoice,
            Stranger->PlayerState,SearchRevision,Travel));
        TestTrue(TEXT("同一地点和同一种步行保留当前找物"),Companion->CanKeepItemErrand(*Repeat->ItemChoice,
            Owner->PlayerState,SearchRevision,Travel));
        if (!TestTrue(TEXT("重复委托接续执行"),Companion->ApplyItemChoice(*Repeat->ItemChoice,
            Owner->PlayerState,SearchRevision,Travel))) return false;
        TestEqual(TEXT("重复委托保留修订"),Companion->GetCommandRevision(),SearchRevision);
        TestTrue(TEXT("重复委托保留找物进度"),Companion->HasNearbyItemSearch() && Companion->ErrandActiveSeconds==4.f &&
            Companion->ItemSearchPointIndex==1 && Companion->ItemSearchPointStartedAt==3.f);
    }

    auto* UpdatedSeen=Companion->SeenItems.FindByPredicate([Tool](const auto& Observation) { return Observation.Item.Get()==Tool; });
    if (!TestNotNull(TEXT("保留真实目标的搜索记忆"),UpdatedSeen)) return false;
    UpdatedSeen->Location+=FVector(0,160,0);
    const FVector NewLeadPoint=UpdatedSeen->Location;
    FDeliveryNpcDecisionRequest NewLeadRequest;
    const auto* NewLead=BuildLeadOption(NewLeadRequest,Preserve);
    if (!TestNotNull(TEXT("新地点有自己的线索"),NewLead)) return false;
    TestFalse(TEXT("同一实物的新地点不能复用旧路线"),Companion->CanKeepItemErrand(*NewLead->ItemChoice,
        Owner->PlayerState,SearchRevision,Preserve));
    if (!TestTrue(TEXT("按新地点改派"),Companion->ApplyItemChoice(*NewLead->ItemChoice,
        Owner->PlayerState,SearchRevision,Preserve))) return false;
    TestTrue(TEXT("按新地点重建实际找物阶段"),Companion->ItemLastSeenLocation.Equals(NewLeadPoint) &&
        Companion->GetCommandRevision()>SearchRevision && Companion->ItemSearchPointIndex==0);

    const int32 NewLeadRevision=Companion->GetCommandRevision();
    const auto Automatic=EDeliveryCompanionTravelMode::Automatic;
    FDeliveryNpcDecisionRequest ChangedTravelRequest;
    const auto* ChangedTravel=BuildLeadOption(ChangedTravelRequest,Automatic);
    if (!TestNotNull(TEXT("同一新地点提供明确改出行候选"),ChangedTravel)) return false;
    TestFalse(TEXT("明确更改出行方式不复用旧路线"),Companion->CanKeepItemErrand(*ChangedTravel->ItemChoice,
        Owner->PlayerState,NewLeadRevision,Automatic));
    if (!TestTrue(TEXT("接受新的出行方式"),Companion->ApplyItemChoice(*ChangedTravel->ItemChoice,
        Owner->PlayerState,NewLeadRevision,Automatic))) return false;
    TestTrue(TEXT("改出行方式才更新修订和长期偏好"),Companion->GetCommandRevision()>NewLeadRevision &&
        Companion->GetTravelMode()==Automatic);

    TestTrue(TEXT("主人可取消仍在进行的找物"),Companion->ApplyCommand(EDeliveryNpcAction::CancelCompanionTask,
        Owner->PlayerState,Companion->GetCommandRevision()));
    TestFalse(TEXT("取消后退出找物子树条件"),Companion->HasNearbyItemSearch());
    TestFalse(TEXT("取消找物后没有把没看见的对象写入已知列表"),Agent->FindKnownSubjectId(Tool).IsValid());
    Agent->InterruptPlayerReply(Owner);
    TestTrue(TEXT("访客随后可以发起自己的对话"),Agent->BeginListening(Stranger->GetPawn(),FGuid::NewGuid()));
    FDeliveryNpcDecisionRequest VisitorAfterSearch;
    TestTrue(TEXT("构造访客请求"),Agent->BuildDecisionRequest(FGuid::NewGuid(),VisitorAfterSearch));
    TestFalse(TEXT("访客看不到主人线索"),VisitorAfterSearch.Evidence.ContainsByPredicate([](const FDeliveryNpcEvidence& Fact)
        { return Fact.Kind==DeliveryCompanionItemSearch::EvidenceKind; }));
    TestFalse(TEXT("访客没有对应的目标编号"),VisitorAfterSearch.Subjects.ContainsByPredicate([&LeadChoice](const FDeliveryNpcSubject& Subject)
        { return Subject.SubjectId==LeadChoice.SubjectId; }));
    TestFalse(TEXT("临时线索编号不出现在下一轮历史"),VisitorAfterSearch.LastDecisionEventIds.Contains(LeadChoice.EvidenceId));
    TestFalse(TEXT("访客不能发起没有具体物件的区域找物"),VisitorAfterSearch.AllowedActions.Contains(EDeliveryNpcAction::SearchNearOwner));
    Agent->CancelPlan(TEXT("ItemSearchScopeTestFinished"));
    {
        FDeliveryNpcObservation Speech;
        Speech.EventId=FGuid::NewGuid(); Speech.Kind=TEXT("PlayerSpeech"); Speech.Source=EDeliveryNpcEvidenceSource::Heard;
        Speech.Subject=CarryingPlayer; Speech.Addressee=Npc; Speech.bNeedsReply=true; Speech.Description=TEXT("锤子可能在我旁边，帮我找找。");
        FDeliveryNpcDecisionRequest AreaRequest;
        if (!TestTrue(TEXT("主人原话生成新区域找物候选"),Agent->BeginListening(CarryingPlayer,Speech.EventId) &&
            Agent->ObserveEvent(Speech) && Agent->BuildDecisionRequest(FGuid::NewGuid(),AreaRequest))) return false;
        const auto* Area=AreaRequest.Candidates.FindByPredicate([](const auto& Option)
        { return Option.Action==EDeliveryNpcAction::SearchNearOwner; });
        if (!TestTrue(TEXT("区域找物只绑定当前主人发言与身体，不绑定猜测的道具"),Area && Area->ItemChoice.IsValid() &&
            !Area->TargetId.IsValid() && !Area->ItemChoice->Item.IsValid() && Area->ItemChoice->SourceCarrier.Get()==CarryingPlayer &&
            Area->EvidenceIds.Num()==1 && Area->EvidenceIds[0]==Speech.EventId)) return false;
        FDeliveryNpcDecisionProposal AreaReply;
        AreaReply.CapabilityVersion=AreaRequest.RequestedCapabilityVersion;
        AreaReply.CandidateId=Area->CandidateId; AreaReply.EvidenceIds=Area->EvidenceIds;
        AreaReply.WorldEpoch=AreaRequest.WorldEpoch; AreaReply.NpcId=AreaRequest.NpcId; AreaReply.RequestId=AreaRequest.RequestId;
        AreaReply.ContextRevision=AreaRequest.ContextRevision; AreaReply.PlanRevision=AreaRequest.PlanRevision;
        AreaReply.Action=Area->Action; AreaReply.Speech=TEXT("我到你旁边看看。");
        Agent->ActiveRequest=AreaRequest;
        if (!TestTrue(TEXT("原话的候选经服务器校验"),Agent->TryAcceptProposal(AreaReply,Reason))) return false;
        Agent->ActiveRequest={};
        FDeliveryNpcPublicState StartedArea;
        if (!TestTrue(TEXT("领取消息后以正式角色入口执行查找"),Agent->StartAction(StartedArea))) return false;
        StartedArea.Speech.Reset();
        if (!TestTrue(TEXT("区域委托沿正式找物子树条件执行"),Npc->ExecuteAction(StartedArea) && Companion->HasNearbyItemSearch() &&
            Companion->GetErrand().Action==EDeliveryNpcAction::SearchNearOwner && !Companion->GetErrand().Item.IsValid())) return false;
        Agent->TryFinishAction(StartedArea.ActionId,EDeliveryNpcActionStatus::Succeeded,TEXT("CompanionErrandAccepted"));
        const FVector PreviousOwnerPosition=CarryingPlayer->GetActorLocation();
        CarryingPlayer->SetActorLocation(PreviousOwnerPosition+FVector(DeliveryCompanionItemSearch::Radius+200.f,0,0),false,nullptr,ETeleportType::TeleportPhysics);
        ON_SCOPE_EXIT { CarryingPlayer->SetActorLocation(PreviousOwnerPosition,false,nullptr,ETeleportType::TeleportPhysics); };
        Companion->UpdateErrand();
        TestTrue(TEXT("主人离开原区域后结束查找，不追新位置或移交未实见物品"),!Companion->HasActiveErrand() &&
            Companion->GetErrand().Reason==TEXT("ItemSearchOwnerLeftArea") && !Companion->GetCarriedItem());
    }
    {
        const FVector Origin(34000,25000,0);
        const FTransform PreviousNpc=Npc->GetActorTransform();
        const FTransform PreviousPlayer=CarryingPlayer->GetActorTransform();
        auto* Movement=Npc->GetCharacterMovement();
        auto* AI=Cast<ADeliveryNpcAIController>(Npc->GetController());
        auto* Path=AI?AI->GetPathFollowingComponent():nullptr;
        auto* ToolMesh=Tool->GetItemMesh();
        auto* Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!TestTrue(TEXT("普通物品跳跃回归使用原二狗控制器和实物网格"),Path && ToolMesh && Cube)) return false;
        auto* Floor=MakeRoadBox(Origin+FVector(240,0,-25),FVector(550,180,25));
        ON_SCOPE_EXIT
        {
            Agent->CancelPlan(TEXT("ItemReturnJumpTest"));
            if (Companion->HasActiveErrand()) Companion->FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("ItemReturnJumpTest"));
            AI->StopObstacleTraversal();
            Npc->SetActorTransform(PreviousNpc,false,nullptr,ETeleportType::TeleportPhysics);
            CarryingPlayer->SetActorTransform(PreviousPlayer,false,nullptr,ETeleportType::TeleportPhysics);
            Movement->SetMovementMode(MOVE_Walking);
            Floor->Destroy();
        };
        ToolMesh->SetStaticMesh(Cube);
        Tool->SetActorScale3D(FVector(.25));
        Npc->SetActorLocation(Origin+FVector(0,0,Npc->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()+3.),
            false,nullptr,ETeleportType::TeleportPhysics);
        CarryingPlayer->SetActorLocation(Origin+FVector(180,0,CarryingPlayer->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()+3.),
            false,nullptr,ETeleportType::TeleportPhysics);
        Movement->SetMovementMode(MOVE_Walking);
        if (!TestTrue(TEXT("二狗拿着同一普通物品，主人确实能接"),Npc->TryReceiveHandheldItem(Tool) &&
            Tool->GetOwner()==Npc && CarryingPlayer->CanReceiveHandheldItem(Tool))) return false;
        // 本夹具前几轮共用同一世界时刻，重新观察真实持件证据后再生成候选
        Companion->NextItemObservationAt=0;
        FDeliveryNpcDecisionRequest ReturnRequest;
        if (!TestTrue(TEXT("真实主人本轮可以提出交还普通物品"),Agent->BeginListening(CarryingPlayer,FGuid::NewGuid()) &&
            Agent->BuildDecisionRequest(FGuid::NewGuid(),ReturnRequest))) return false;
        const auto* Return=ReturnRequest.Candidates.FindByPredicate([Tool](const auto& Candidate)
        {
            return Candidate.Action==EDeliveryNpcAction::ReturnItem && Candidate.TravelMode==EDeliveryCompanionTravelMode::Walk &&
                Candidate.ItemChoice.IsValid() && Candidate.ItemChoice->Item.Get()==Tool;
        });
        if (!TestTrue(TEXT("由主人专属候选安排交还原物"),Return && Companion->ApplyItemChoice(*Return->ItemChoice,
            Owner->PlayerState,ReturnRequest.CompanionCommandRevision,Return->TravelMode))) return false;
        Agent->InterruptPlayerReply(Owner);
        const int32 ItemJumpRevision=Companion->GetCommandRevision();
        const FVector Goal=Origin+FVector(450,0,0);
        FAIMoveRequest Move(Goal);
        const auto Route=MakeShared<FNavigationPath,ESPMode::ThreadSafe>(TArray<FVector>{Npc->GetNavAgentLocation(),Goal});
        const FAIRequestID MoveId=Path->RequestMove(Move,Route);
        if (!TestTrue(TEXT("普通物品委托使用真实 UE 移动编号"),MoveId.IsValid())) return false;
        Companion->bErrandOwnsNavigation=true;
        Companion->ErrandMoveId=MoveId;
        Path->PauseMove(MoveId,EPathFollowingVelocityMode::Keep);
        // 这项验证从已批准的跳弧开始，地面和跳跃状态仍由真实角色移动处理
        const FVector Launch(120,0,340);
        AI->ObstacleTraversal.Pawn=Npc;
        AI->ObstacleTraversal.Request=MoveId;
        AI->ObstacleTraversal.LaunchVelocity=Launch;
        AI->ObstacleTraversal.LandingFeet=Origin+FVector(82,0,0);
        AI->ObstacleTraversal.ExpiresAt=World->GetTimeSeconds()+2.;
        Npc->LaunchCharacter(Launch,true,true);
        if (!TestTrue(TEXT("二狗确实处于当前路线的腾空期"),Movement->HandlePendingLaunch() && Movement->IsFalling() &&
            AI->IsTraversingObstacle(MoveId) && FVector::Dist(Npc->GetActorLocation(),CarryingPlayer->GetActorLocation())<220.)) return false;
        AI->UpdateObstacleTraversal();
        Companion->UpdateErrand();
        TestTrue(TEXT("腾空时即使主人已在交互距离内仍持原物并保留暂停的原路线"),Tool->GetOwner()==Npc &&
            Companion->GetCommandRevision()==ItemJumpRevision && Companion->GetErrand().Phase==EDeliveryCompanionErrandPhase::ReturnToOwner &&
            Path->GetCurrentRequestId()==MoveId && AI->IsTraversingObstacle(MoveId));
        for (int32 Index=0;Index<48 && AI->IsTraversingObstacle(MoveId);++Index)
        {
            ++GFrameCounter;
            World->Tick(LEVELTICK_All,.025f);
        }
        if (!TestTrue(TEXT("真正落地后沿同一编号恢复原路"),Movement->IsMovingOnGround() && Movement->CurrentFloor.IsWalkableFloor() &&
            Path->GetCurrentRequestId()==MoveId && Path->GetStatus()==EPathFollowingStatus::Moving)) return false;
        Companion->UpdateErrand();
        TestTrue(TEXT("落地之后才将原普通物品交还主人"),Tool->GetOwner()==CarryingPlayer && CarryingPlayer->OwnsHandheldItem(Tool) &&
            !Companion->HasActiveErrand() && Companion->GetErrand().Reason==TEXT("ItemReturnedToOwner"));
        CarryingPlayer->ReleaseHandheldItem(Tool);
    }
    Tool->Destroy();

    Owner->Possess(PreviousOwnerPawn); CarryingPlayer->Destroy();
    TestTrue(TEXT("主人重新安排跟随以检查补位回包"),Companion->ApplyCommand(
        EDeliveryNpcAction::Follow,Owner->PlayerState,Companion->GetCommandRevision()));
    // 这里只构造服务器待答挑战并检查协议，不生成视口，也不把回答当成画面验证
    const auto PrepareChallenge=[&](bool bIncludeStranger=true)
    {
        Companion->CancelCatchUp();
        Companion->CatchUpRequestId=FGuid::NewGuid();
        Companion->CatchUpDeadline=World->GetTimeSeconds()+.6;
        Companion->CatchUpStart=Npc->GetActorLocation();
        Companion->CatchUpOwnerPawn=Owner->GetPawn();
        Companion->CatchUpOwnerPosition=Owner->GetPawn()->GetActorLocation();
        Companion->CatchUpCommandRevision=Companion->GetCommandRevision();
        Companion->CatchUpRound=1;
        Companion->NextCatchUpRoundAt=0;
        for (auto* PC:{Owner,Stranger})
        {
            if (PC==Stranger && !bIncludeStranger) continue;
            UDeliveryCompanionComponent::FCatchUpViewer Viewer;
            Viewer.Player=PC; Viewer.Pawn=PC->GetPawn(); Viewer.ViewTarget=PC->GetViewTarget();
            Viewer.ViewRevision=PC->GetCompanionViewRevision();
            Companion->CatchUpViewers.Add(Viewer);
        }
        return Companion->CatchUpRequestId;
    };
    const FVector BeforeReplies=Npc->GetActorLocation();
    FGuid Challenge=PrepareChallenge(false);
    Stranger->ServerReportCompanionCatchUpView(Npc,Challenge,Stranger->GetPawn(),Stranger->GetViewTarget(),true);
    TestFalse(TEXT("未被查询的PC不能代替等待中的玩家回答"),Companion->CatchUpViewers[0].bReplied);

    const FGuid PreviousChallenge=Challenge;
    Challenge=PrepareChallenge();
    Owner->ServerReportCompanionCatchUpView(Npc,PreviousChallenge,Owner->GetPawn(),Owner->GetViewTarget(),false);
    TestFalse(TEXT("旧GUID不能占用本轮回答"),Companion->CatchUpViewers[0].bReplied);
    Owner->ServerReportCompanionCatchUpView(Npc,Challenge,Owner->GetPawn(),Owner->GetViewTarget(),true);
    Owner->ServerReportCompanionCatchUpView(Npc,Challenge,Owner->GetPawn(),Owner->GetViewTarget(),false);
    TestFalse(TEXT("重复回包不能覆盖本轮已接受的回答"),Companion->bCatchUpDenied);
    Stranger->ServerReportCompanionCatchUpView(Npc,Challenge,Stranger->GetPawn(),Stranger->GetViewTarget(),true);
    TestTrue(TEXT("全员回包只记录结果，挑战仍等待BT执行"),Companion->CatchUpRequestId==Challenge &&
        !Companion->CatchUpViewers.ContainsByPredicate([](const auto& Viewer){return !Viewer.bReplied;}));
    TestEqual(TEXT("全员RPC回包不能移动同伴"),Npc->GetActorLocation(),BeforeReplies);

    // 第一轮结束后计时，即使原查询期限已过也保留完整的轮间等待
    Companion->CatchUpRound=0;
    TestTrue(TEXT("第一轮全员回答后进入轮间等待"),Companion->UpdateCatchUp(Owner->GetPawn()));
    TestTrue(TEXT("轮间等待从全员回答完成后起算"),Companion->NextCatchUpRoundAt>=World->GetTimeSeconds()+.19);
    Companion->CatchUpDeadline=World->GetTimeSeconds()-.1;
    TestTrue(TEXT("轮间等待不再沿用第一轮查询超时"),Companion->UpdateCatchUp(Owner->GetPawn()));

    Challenge=PrepareChallenge();
    APawn* PreviousPawn=Owner->GetPawn();
    AActor* PreviousView=Owner->GetViewTarget();
    Owner->Possess(MakePawn(FVector(100,0,88)));
    Owner->ServerReportCompanionCatchUpView(Npc,Challenge,PreviousPawn,PreviousView,true);
    TestTrue(TEXT("换Pawn后旧视角回答拒绝本次补位"),Companion->bCatchUpDenied);
    TestFalse(TEXT("换Pawn后的挑战不再继续"),Companion->UpdateCatchUp(Owner->GetPawn()));

    Challenge=PrepareChallenge();
    const uint64 ViewRevision=Owner->GetCompanionViewRevision();
    Owner->SetViewTarget(Owner->GetViewTarget());
    TestTrue(TEXT("相同目标重新设置视角也使旧版本失效"),Owner->GetCompanionViewRevision()>ViewRevision);
    Owner->ServerReportCompanionCatchUpView(Npc,Challenge,Owner->GetPawn(),Owner->GetViewTarget(),true);
    TestTrue(TEXT("Pawn和ViewTarget相同仍拒绝旧视角版本"),Companion->bCatchUpDenied);

    Challenge=PrepareChallenge();
    TestTrue(TEXT("新取消命令终止待答补位"),Companion->ApplyCommand(
        EDeliveryNpcAction::CancelCompanionTask,Owner->PlayerState,Companion->GetCommandRevision()));
    Owner->ServerReportCompanionCatchUpView(Npc,Challenge,Owner->GetPawn(),Owner->GetViewTarget(),true);
    TestTrue(TEXT("取消后的旧回包不能重建挑战"),!Companion->CatchUpRequestId.IsValid() && Companion->CatchUpViewers.IsEmpty());
    TestEqual(TEXT("取消后的旧回包保持等候"),Companion->GetMode(),EDeliveryCompanionMode::Stay);
    TestEqual(TEXT("失效回包均未改变同伴位置"),Npc->GetActorLocation(),BeforeReplies);

    Owner->PlayerState->SetIsInactive(true);
    Npc->Companion->TickComponent(.25f,LEVELTICK_All,nullptr);
    TestNull(TEXT("断线清理主人"),Npc->Companion->GetOwnerPlayerState());
    TestTrue(TEXT("断线清理旧主人的私人经历"),Npc->Agent->Evidence.IsEmpty());
    TestTrue(TEXT("断线清理旧对话主体引用"),Npc->Agent->KnownSubjects.IsEmpty());
    TestEqual(TEXT("断线后回到等人扶起"),Npc->Companion->GetMode(),EDeliveryCompanionMode::Unrecruited);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryCompanionEncounterOrderTest,"Delivery.AI.Companion.EncounterOrderLifetime",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryCompanionEncounterOrderTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool Enabled=Settings->bEnabled;
    Settings->bEnabled=false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
        Settings->bEnabled=Enabled;
    };
    auto* State=World->SpawnActor<AGameStateBase>();
    World->SetGameState(State);
    auto* Manager=NewObject<UDeliveryTaskManagerComponent>(State);
    State->AddInstanceComponent(Manager);
    auto* Task=NewObject<UDeliveryTaskDefinition>(State);
    Task->TaskId=TEXT("Task_001");
    Task->PickupLocationId=TEXT("Location_Restaurant_001");
    auto* Unlock=NewObject<UDeliveryTaskUnlockCondition_TimeSinceStart>(Task);
    Unlock->DelaySeconds=1000;
    Task->UnlockConditions.Add(Unlock);
    // 只通过反射设置相当于 GameState 蓝图的任务配置，运行状态由真实接单、取件与交付接口推进
    auto* DefinitionsProperty=FindFProperty<FArrayProperty>(Manager->GetClass(),TEXT("TaskDefinitions"));
    if (!TestNotNull(TEXT("任务配置反射字段存在"),DefinitionsProperty)) return false;
    DefinitionsProperty->ContainerPtrToValuePtr<TArray<TObjectPtr<UDeliveryTaskDefinition>>>(Manager)->Add(Task);
    Manager->RegisterComponent();
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    auto* Npc=World->SpawnActor<ADeliveryCompanionCharacter>();
    auto* Scene=World->SpawnActor<ADeliveryCompanionEncounterScene>(FVector(0,0,100),FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("创建初遇角色"),Npc) || !TestNotNull(TEXT("创建可复制的初遇场景"),Scene)) return false;
    TestEqual(TEXT("新场景初始没有阻挡"),Scene->GetEncounterObstruction(),EDeliveryCompanionEncounterObstruction::None);
    const auto* ObstructionProperty=FindFProperty<FProperty>(Scene->GetClass(),TEXT("EncounterObstruction"));
    if (!TestNotNull(TEXT("场景公开状态有可复制字段"),ObstructionProperty)) return false;
    Scene->GetClass()->SetUpRuntimeReplicationData();
    TArray<FLifetimeProperty> ReplicatedProperties;
    Scene->GetLifetimeReplicatedProps(ReplicatedProperties);
    TestTrue(TEXT("阻挡字段登记到场景的网络复制列表"),ObstructionProperty->HasAnyPropertyFlags(CPF_Net) &&
        ReplicatedProperties.ContainsByPredicate([&](const auto& Entry){ return Entry.RepIndex==ObstructionProperty->RepIndex; }));
    Scene->SetEncounterObstruction(EDeliveryCompanionEncounterObstruction::Doorway);
    TestEqual(TEXT("服务器可登记门叶范围受阻"),Scene->GetEncounterObstruction(),EDeliveryCompanionEncounterObstruction::Doorway);
    Scene->SetEncounterObstruction(EDeliveryCompanionEncounterObstruction::EjectionPath);
    TestEqual(TEXT("抛出路径受阻与门叶范围受阻分开"),Scene->GetEncounterObstruction(),EDeliveryCompanionEncounterObstruction::EjectionPath);
    Scene->SetReplicates(true);
    Scene->SwapRoles();
    TestFalse(TEXT("本地角色切换为无权限代理"),Scene->HasAuthority());
    Scene->SetEncounterObstruction(EDeliveryCompanionEncounterObstruction::None);
    TestEqual(TEXT("无权限代理不能清除服务器状态"),Scene->GetEncounterObstruction(),EDeliveryCompanionEncounterObstruction::EjectionPath);
    Scene->SwapRoles();
    Scene->SetEncounterObstruction(EDeliveryCompanionEncounterObstruction::None);
    TestEqual(TEXT("服务器可清除阻挡"),Scene->GetEncounterObstruction(),EDeliveryCompanionEncounterObstruction::None);
    Npc->EncounterScene=Scene;
    const FTransform PickupTransform(FRotator::ZeroRotator,FVector(400,0,100));
    auto* Pickup=World->SpawnActorDeferred<ADeliveryPickupPoint>(ADeliveryPickupPoint::StaticClass(),PickupTransform,
        nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if (!TestNotNull(TEXT("创建真实面馆取货点"),Pickup)) return false;
    Pickup->LocationId=Task->PickupLocationId;
    Pickup->InteractRadius=1000;
    // 构造脚本把配置传给组件，BeginPlay 自动登记地点并配置真实 PickupE 交互
    Pickup->FinishSpawning(PickupTransform);
    auto* Registry=UDeliveryLocationRegistry::Get(Npc);
    if (!TestNotNull(TEXT("测试世界具有实际地点注册表"),Registry)) return false;
    TestEqual(TEXT("地点组件已自动登记面馆"),Registry->ResolveActor(Task->PickupLocationId),static_cast<AActor*>(Pickup));
    auto* Interaction=UDeliveryInteractableComponent::FindOn(Pickup,EDeliveryInteractionKey::PickupE);
    if (!TestNotNull(TEXT("取货点配置了真实 PickupE 交互"),Interaction)) return false;
    TestEqual(TEXT("取货距离沿地点组件锚点计算"),Interaction->InteractAnchor.Get(),static_cast<USceneComponent*>(Pickup->GetLocationComponent()));
    TestEqual(TEXT("面馆实际交互半径为一千厘米"),Interaction->InteractRadius,1000.f);
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Walker=World->SpawnActor<ADeliveryCharacter>(FVector(1420,0,100),FRotator::ZeroRotator,Spawn);
    if (!TestNotNull(TEXT("创建真实步行玩家"),Walker)) return false;
    Walker->GetActiveRagdoll()->StopRagdoll();
    Walker->GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    TestTrue(TEXT("玩家原点在旧九米初遇范围外"),FVector::Dist(Walker->GetActorLocation(),Scene->GetTriggerWorldLocation())>900.f);
    TestTrue(TEXT("玩家原点也略超取货半径，必须沿真实碰撞表面测距"),
        FVector::Dist(Walker->GetActorLocation(),Pickup->GetLocationComponent()->GetComponentLocation())>Interaction->InteractRadius);
    TestTrue(TEXT("真实交互距离允许玩家在胶囊表面进入取货范围"),Interaction->CanInteract(Walker));
    TestTrue(TEXT("真实任务管理器已经登记第一单"),Manager->IsTaskRegistered(Task));
    TestEqual(TEXT("解锁时间未到仍为Locked"),Manager->GetTaskStatus(Task),EDeliveryTaskStatus::Locked);
    TestFalse(TEXT("未解锁第一单不会初遇"),Npc->HasEncounterOrderStarted());
    Unlock->DelaySeconds=0;
    Manager->ReevaluateUnlocks();
    TestEqual(TEXT("解锁但未接听只到Offered"),Manager->GetTaskStatus(Task),EDeliveryTaskStatus::Offered);
    TestFalse(TEXT("未接听第一单不会初遇"),Npc->HasEncounterOrderStarted());
    TestFalse(TEXT("未接听订单不能借面馆范围提前触发"),Npc->CanWitnessEncounter(Walker));
    TestTrue(TEXT("真实接听入口推进待取件"),Manager->StartTaskTimerFromAcceptedCall(Task));
    TestTrue(TEXT("待取件阶段允许初遇"),Npc->HasEncounterOrderStarted());
    TestTrue(TEXT("旧九米范围外但实际能取餐且门口可见时允许初遇"),Npc->CanWitnessEncounter(Walker));
    Interaction->bInteractEnabled=false;
    TestFalse(TEXT("面馆交互禁用后不能借其扩大初遇范围"),Npc->CanWitnessEncounter(Walker));
    Interaction->bInteractEnabled=true;
    const FName OriginalPickupId=Task->PickupLocationId;
    Task->PickupLocationId=TEXT("Location_DifferentShop");
    TestFalse(TEXT("其他地点不能借本面馆的取货范围触发"),Npc->CanWitnessEncounter(Walker));
    Task->PickupLocationId=OriginalPickupId;
    TestTrue(TEXT("恢复匹配地点后仍能看见初遇"),Npc->CanWitnessEncounter(Walker));

    auto* Wall=World->SpawnActor<AActor>();
    auto* WallBox=NewObject<UBoxComponent>(Wall);
    Wall->SetRootComponent(WallBox);
    WallBox->SetBoxExtent(FVector(10,250,250));
    WallBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    WallBox->SetCollisionObjectType(ECC_WorldStatic);
    WallBox->SetCollisionResponseToAllChannels(ECR_Ignore);
    WallBox->SetCollisionResponseToChannel(ECC_Visibility,ECR_Block);
    WallBox->RegisterComponent();
    Wall->SetActorLocation((Walker->GetPawnViewLocation()+Scene->GetTriggerWorldLocation())*.5f);
    FHitResult SightHit;
    FCollisionQueryParams SightParams(SCENE_QUERY_STAT(CompanionEncounterTestWall),false,Walker);
    SightParams.AddIgnoredActor(Npc);
    TestTrue(TEXT("正控制确认查询墙实际挡住门外锚点射线"),World->LineTraceSingleByChannel(SightHit,
        Walker->GetPawnViewLocation(),Scene->GetTriggerWorldLocation(),ECC_Visibility,SightParams));
    TestEqual(TEXT("射线实际命中本次测试墙"),SightHit.GetActor(),Wall);
    TestTrue(TEXT("墙不会改变面馆的距离交互资格"),Interaction->CanInteract(Walker));
    TestFalse(TEXT("即使位于真实取货范围也不能隔墙触发"),Npc->CanWitnessEncounter(Walker));
    WallBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    TestTrue(TEXT("遮挡解除后恢复初遇资格"),Npc->CanWitnessEncounter(Walker));

    auto* Bike=World->SpawnActor<ADeliveryMotorbike>(FVector(1350,200,100),FRotator::ZeroRotator,Spawn);
    if (!TestNotNull(TEXT("创建真实载具Pawn检查入口限制"),Bike)) return false;
    TestTrue(TEXT("载具在旧范围外但位于同一取货距离内"),
        FVector::Dist(Bike->GetActorLocation(),Scene->GetTriggerWorldLocation())>900.f && Interaction->CanInteract(Bike));
    TestFalse(TEXT("车辆不能获得步行取餐的新增触发范围"),Npc->CanWitnessEncounter(Bike));
    Bike->Destroy();

    TestTrue(TEXT("真实取件入口推进配送"),Manager->TryAcquireItem(Task,nullptr));
    TestTrue(TEXT("配送阶段允许初遇"),Npc->HasEncounterOrderStarted());
    TestFalse(TEXT("已经取件后面馆不能再次发货"),Pickup->GetPickupPoint()->CanDispense());
    TestTrue(TEXT("已取件不会丢失真实面馆范围的初遇资格"),Npc->CanWitnessEncounter(Walker));
    Npc->Encounter.Phase=EDeliveryCompanionEncounterPhase::OpeningDoor;
    Npc->Encounter.PhaseStartedAt=Npc->GetEncounterServerTime();
    TestTrue(TEXT("真实交付入口完成第一单"),Manager->TryCompleteDelivery(Task,nullptr));
    TestNull(TEXT("完成后当前计时任务已经清空"),Manager->GetTimedTask());
    TestTrue(TEXT("完成记录仍允许迟来的初遇"),Npc->HasEncounterOrderStarted());
    TestTrue(TEXT("Completed后仍能在面馆真实交互范围看见初遇"),Npc->CanWitnessEncounter(Walker));
    Npc->UpdateEncounter();
    TestEqual(TEXT("开门期间交付不会撤销演出"),Npc->Encounter.Phase,EDeliveryCompanionEncounterPhase::OpeningDoor);
    Npc->EncounterTaskId=TEXT("Task_Unregistered");
    TestFalse(TEXT("其他已完成订单不能解锁未登记的初遇订单"),Npc->HasEncounterOrderStarted());

    // 原生场景没有蓝图门叶表现，堵门验证用实际可编辑面馆门
    auto* AuthoredSceneClass=LoadClass<ADeliveryCompanionEncounterScene>(nullptr,
        TEXT("/Game/NPC/Companion/Encounter/BP_NoodleEncounter.BP_NoodleEncounter_C"));
    if (!TestNotNull(TEXT("加载正式面馆门蓝图"),AuthoredSceneClass)) return false;
    auto* AuthoredScene=World->SpawnActor<ADeliveryCompanionEncounterScene>(AuthoredSceneClass,
        FVector(20000,0,100),FRotator::ZeroRotator,Spawn);
    if (!TestNotNull(TEXT("生成带实际门叶的初遇场景"),AuthoredScene) ||
        !TestTrue(TEXT("服务器接受真实门叶的开门请求"),AuthoredScene->RequestDoorOpen(true))) return false;
    const FVector ThrowStart=AuthoredScene->GetThrowStartWorldLocation();
    const FVector ThrowLanding=AuthoredScene->GetThrowLandingWorldLocation();
    const FVector Outward=(ThrowLanding-ThrowStart).GetSafeNormal2D();
    const auto* Doorway=AuthoredScene->DoorwayClearance.Get();
    const FVector DoorCenter=Doorway->GetComponentLocation();
    const FVector DoorAxis=Doorway->GetComponentQuat().UnrotateVector(Outward);
    const double DoorDepth=FVector::DotProduct(Doorway->GetScaledBoxExtent(),DoorAxis.GetAbs());
    if (!TestTrue(TEXT("正式抛出锚点与转动范围定义有效的离店方向"),!Outward.IsNearlyZero() && DoorDepth>10.)) return false;
    TestFalse(TEXT("店内侧抛出起点不算离开门框"),AuthoredScene->IsPastDoorway(ThrowStart));
    TestFalse(TEXT("已过门中心但仍未离开门叶转动范围"),AuthoredScene->IsPastDoorway(DoorCenter+Outward*(DoorDepth-5.)));
    TestTrue(TEXT("超过门叶转动范围后才进入街面一侧"),AuthoredScene->IsPastDoorway(DoorCenter+Outward*(DoorDepth+100.)));
    TestTrue(TEXT("正式抛出落点可从店外靠近"),AuthoredScene->IsPastDoorway(ThrowLanding));
    TestFalse(TEXT("从店内方向退开九米以内不视为已离开面馆"),AuthoredScene->IsPastDoorway(DoorCenter-Outward*850.));
    TestTrue(TEXT("从其他方向离开面馆九米后也允许关门"),AuthoredScene->IsPastDoorway(DoorCenter-Outward*950.));
    TArray<EDeliveryCompanionEncounterObstruction> ObstructionChanges;
    const FDelegateHandle ObstructionListener=AuthoredScene->OnEncounterObstructionChanged.AddLambda([&]()
    {
        ObstructionChanges.Add(AuthoredScene->GetEncounterObstruction());
    });
    ON_SCOPE_EXIT { AuthoredScene->OnEncounterObstructionChanged.Remove(ObstructionListener); };
    Npc->EncounterScene=AuthoredScene;
    Npc->EncounterTaskId=Task->TaskId;
    auto* Witness=World->SpawnActor<APlayerController>();
    if (!TestNotNull(TEXT("创建退到门边的玩家控制器"),Witness)) return false;
    Witness->Possess(Walker);
    Walker->SetActorLocation(AuthoredScene->GetTriggerWorldLocation()+FVector(0,700,0));
    if (!TestTrue(TEXT("玩家已让开门框且仍在续演范围"),Npc->HasNearbyEncounterWitness())) return false;
    const float DoorStep=AuthoredScene->DoorTravelSeconds*.25f;
    auto* DoorActor=static_cast<AActor*>(AuthoredScene);
    DoorActor->Tick(DoorStep);
    const float BeforeBlock=AuthoredScene->DoorProgress;
    if (!TestTrue(TEXT("真实门叶先走到部分开启角度"),BeforeBlock>0.f && BeforeBlock<1.f)) return false;
    auto* DoorBlocker=World->SpawnActor<AActor>();
    auto* DoorBlockerShape=NewObject<UBoxComponent>(DoorBlocker);
    DoorBlocker->SetRootComponent(DoorBlockerShape);
    DoorBlockerShape->SetBoxExtent(FVector(25.f));
    DoorBlockerShape->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    DoorBlockerShape->SetCollisionObjectType(ECC_WorldDynamic);
    DoorBlockerShape->SetCollisionResponseToAllChannels(ECR_Block);
    DoorBlockerShape->RegisterComponent();
    DoorBlocker->SetActorLocation(AuthoredScene->DoorwayClearance->GetComponentLocation());
    DoorActor->Tick(DoorStep);
    TestTrue(TEXT("物理查询实测门框被占用"),AuthoredScene->bDoorBlocked);
    TestEqual(TEXT("占住开门范围时保持原角度"),AuthoredScene->DoorProgress,BeforeBlock);
    Npc->Encounter.PhaseStartedAt=Npc->GetEncounterServerTime()-60.;
    Npc->NextEncounterCheck=0;
    Npc->UpdateEncounter();
    TestEqual(TEXT("长时间堵门后仍保留已经触发的演出"),Npc->Encounter.Phase,EDeliveryCompanionEncounterPhase::OpeningDoor);
    TestEqual(TEXT("长时间堵门仍报告实际门叶挡住"),AuthoredScene->GetEncounterObstruction(),EDeliveryCompanionEncounterObstruction::Doorway);
    TestTrue(TEXT("同一次真实遮挡只通知一次"),ObstructionChanges.Num()==1 &&
        ObstructionChanges[0]==EDeliveryCompanionEncounterObstruction::Doorway);
    DoorActor->Tick(DoorStep);
    Npc->NextEncounterCheck=0;
    Npc->UpdateEncounter();
    TestEqual(TEXT("仍是同一障碍时不再通知"),ObstructionChanges.Num(),1);
    DoorBlocker->SetActorLocation(AuthoredScene->DoorwayClearance->GetComponentLocation()+FVector(5000,0,0));
    DoorActor->Tick(DoorStep);
    Npc->NextEncounterCheck=0;
    Npc->UpdateEncounter();
    TestFalse(TEXT("真实障碍离开后门框恢复净空"),AuthoredScene->bDoorBlocked);
    TestTrue(TEXT("无需重新触发，门从原角度继续打开"),AuthoredScene->DoorProgress>BeforeBlock);
    TestEqual(TEXT("续开时清除阻挡状态"),AuthoredScene->GetEncounterObstruction(),EDeliveryCompanionEncounterObstruction::None);
    TestTrue(TEXT("障碍移开只通知一次净空"),ObstructionChanges.Num()==2 &&
        ObstructionChanges[1]==EDeliveryCompanionEncounterObstruction::None);

    auto* EncounterTuning=LoadObject<UDeliveryCompanionTuning>(nullptr,
        TEXT("/Game/NPC/Companion/DA_CompanionTuning.DA_CompanionTuning"));
    if (!TestNotNull(TEXT("加载正式初遇抛出配置"),EncounterTuning)) return false;
    Npc->Companion->Tuning=EncounterTuning;
    FDeliveryCompanionEncounterState EjectionPath;
    EjectionPath.EjectionStart=ThrowStart;
    EjectionPath.EjectionTarget=ThrowLanding;
    EjectionPath.EjectionDuration=EncounterTuning->EjectionDuration;
    EjectionPath.EjectionCurve=EncounterTuning->EjectionCurve;
    DoorActor->Tick(AuthoredScene->DoorTravelSeconds);
    if (!TestTrue(TEXT("移开门框障碍后实物门完全打开"),AuthoredScene->IsDoorOpen()) ||
        !TestTrue(TEXT("放置落点障碍前抛出曲线确实净空"),
            Npc->GetEjectionPathStatus(EjectionPath)==ADeliveryCompanionCharacter::EEjectionPathStatus::Clear)) return false;
    const double LandingDepth=FVector::DotProduct(ThrowLanding-DoorCenter,Outward);
    if (!TestTrue(TEXT("搬到落点的同一障碍完全位于门框范围外"),
        LandingDepth>DoorDepth+DoorBlockerShape->GetScaledBoxExtent().Size())) return false;
    DoorBlocker->SetActorLocation(ThrowLanding);
    TestTrue(TEXT("原物移至落点后真实胶囊扫掠命中障碍"),
        Npc->GetEjectionPathStatus(EjectionPath)==ADeliveryCompanionCharacter::EEjectionPathStatus::Blocked);
    Npc->NextEncounterCheck=0;
    Npc->UpdateEncounter();
    TestTrue(TEXT("门已完全开启，阻止抛出的是落点障碍"),AuthoredScene->IsDoorOpen() &&
        Npc->Encounter.Phase==EDeliveryCompanionEncounterPhase::OpeningDoor &&
        AuthoredScene->GetEncounterObstruction()==EDeliveryCompanionEncounterObstruction::EjectionPath);
    TestTrue(TEXT("落点障碍只追加一次抛出路径通知"),ObstructionChanges.Num()==3 &&
        ObstructionChanges[2]==EDeliveryCompanionEncounterObstruction::EjectionPath);
    Npc->NextEncounterCheck=0;
    Npc->UpdateEncounter();
    TestEqual(TEXT("真实抛出路径仍受同一障碍阻挡时不再通知"),ObstructionChanges.Num(),3);

    Walker->SetActorLocation(AuthoredScene->GetTriggerWorldLocation()+FVector(0,5000,0));
    if (!TestFalse(TEXT("所有合资格玩家都离开续演范围"),Npc->HasNearbyEncounterWitness())) return false;
    DoorBlocker->SetActorLocation(DoorCenter+FVector(5000,0,0));
    TestTrue(TEXT("目击者离开并移走原物后抛出路径重新净空"),
        Npc->GetEjectionPathStatus(EjectionPath)==ADeliveryCompanionCharacter::EEjectionPathStatus::Clear);
    const FVector BeforeEjection=Npc->GetActorLocation();
    Npc->NextEncounterCheck=0;
    Npc->UpdateEncounter();
    TestEqual(TEXT("玩家刚离开时仍保留已经触发的演出"),Npc->Encounter.Phase,EDeliveryCompanionEncounterPhase::OpeningDoor);
    TestTrue(TEXT("无人在续演范围内，门打开和落点净空也不抛出"),AuthoredScene->IsDoorOpen() &&
        Npc->GetActorLocation()==BeforeEjection);
    TestTrue(TEXT("落点障碍清除后只追加一次净空通知"),ObstructionChanges.Num()==4 &&
        ObstructionChanges[3]==EDeliveryCompanionEncounterObstruction::None);
    Npc->LastNearbyEncounterWitnessAt=Npc->GetEncounterServerTime()-60.;
    Npc->NextEncounterCheck=0;
    Npc->UpdateEncounter();
    TestEqual(TEXT("持续无附近玩家超过宽限后取消初遇"),Npc->Encounter.Phase,EDeliveryCompanionEncounterPhase::Dormant);
    TestEqual(TEXT("取消原因记录玩家已离开"),Npc->Encounter.FailureReason,FName(TEXT("WitnessLeft")));
    const float BeforeClose=AuthoredScene->DoorProgress;
    DoorActor->Tick(DoorStep);
    TestTrue(TEXT("取消后门叶从当前角度关回"),AuthoredScene->DoorProgress<BeforeClose);
    TestEqual(TEXT("没有新增阻挡时取消不会重复通知"),ObstructionChanges.Num(),4);
    return true;
}
#endif
