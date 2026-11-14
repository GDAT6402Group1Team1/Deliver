#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Companion/DeliveryCompanionEncounterScene.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Character/DeliveryCharacter.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Interaction/DeliveryInteractableComponent.h"
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
#include "Task/World/DeliveryPickupPoint.h"
#include "Task/World/DeliveryPickupPointComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Vehicle/DeliveryCompanionMotorbike.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Task/World/DeliveryItemComponent.h"
#include "World/DeliveryLocationComponent.h"
#include "World/DeliveryLocationRegistry.h"
#include "UObject/UnrealType.h"
#include "Engine/Engine.h"
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
    Agent->FinishReplyPlayback(Playback.ReplyId);
    TestEqual(TEXT("仅完成回调且没有真实 SelfSpeech 不算说完"),Agent->PendingJoinedEvent,Joined.EventId);
    Playback.ReplyId=FGuid::NewGuid(); Agent->Replies.Add(FGuid::NewGuid(),Playback);
    FDeliveryNpcObservation Spoken;
    Spoken.Kind=TEXT("SelfSpeech"); Spoken.Subject=Npc; Spoken.Addressee=Owner->GetPawn();
    Spoken.Source=EDeliveryNpcEvidenceSource::ActionResult; Spoken.ReplyToEventId=Joined.EventId;
    Spoken.Description=TEXT("这句话只作为测试中的模型回复"); Spoken.Lifetime=60;
    TestTrue(TEXT("表现完成才记录真实说话结果"),Agent->ObserveEvent(Spoken));
    Agent->FinishReplyPlayback(Playback.ReplyId);
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
    Agent->FinishReplyPlayback(OwnerReply.ReplyId);
    TestEqual(TEXT("玩家回复未实际说完时不结清"),Agent->PendingJoinedEvent,Joined.EventId);
    OwnerReply.ReplyId=FGuid::NewGuid(); Agent->Replies.Add(FGuid::NewGuid(),OwnerReply);
    Spoken.EventId=FGuid::NewGuid(); Spoken.ReplyToEventId=OwnerQuestion;
    Spoken.Description=TEXT("这句测试回复先回答主人当前的问题");
    TestTrue(TEXT("完整台词仍关联玩家原话"),Agent->ObserveEvent(Spoken));
    Agent->FinishReplyPlayback(OwnerReply.ReplyId);
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
    Companion->ErrandVehicle=OwnBike;
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
    OwnerBike->Destroy();
    for (const auto Travel:{EDeliveryCompanionTravelMode::Walk,EDeliveryCompanionTravelMode::OwnVehicle,EDeliveryCompanionTravelMode::Automatic})
    {
        TestTrue(TEXT("主人取得实际出行候选"),Agent->BuildDecisionRequest(FGuid::NewGuid(),TravelChoices));
        TestTrue(TEXT("明确出行方式出现在实际候选中"),HasTravelCandidate(TravelChoices,EDeliveryNpcAction::Follow,Travel));
        TestTrue(TEXT("主人可以提交明确出行选择"),Companion->ApplyCommand(EDeliveryNpcAction::Follow,
            Owner->PlayerState,Companion->GetCommandRevision(),nullptr,nullptr,Travel));
        TestEqual(TEXT("服务器记录主人选择"),Companion->GetTravelMode(),Travel);
        TestFalse(TEXT("访客不能改主人的出行方式"),Companion->ApplyCommand(EDeliveryNpcAction::Follow,
            Stranger->PlayerState,Companion->GetCommandRevision(),nullptr,nullptr,EDeliveryCompanionTravelMode::Walk));
        TestEqual(TEXT("访客被拒后选择不变"),Companion->GetTravelMode(),Travel);
    }
    Agent->CancelPlan(TEXT("TravelChoiceTest"));
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
    auto* CarryingPlayer=World->SpawnActor<ADeliveryCharacter>(FVector(2000,0,88),FRotator::ZeroRotator,TravelSpawn);
    if (!TestNotNull(TEXT("创建真实持件玩家"),CarryingPlayer)) return false;
    Owner->Possess(CarryingPlayer); CarryingPlayer->GetActiveRagdoll()->StopRagdoll();
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
    return true;
}
#endif
