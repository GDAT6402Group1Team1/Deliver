#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionTuning.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Combat/Types/DeliveryCombatEvents.h"
#include "Components/SkeletalMeshComponent.h"
#include "Game/DeliveryGameState.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Player/DeliverPlayerState.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryCompanionAssistTest,"Delivery.AI.Companion.ConfirmedHitAssistAuthority",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryCompanionAssistTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool bEnabled=Settings->bEnabled;
    Settings->bEnabled=false;
    ON_SCOPE_EXIT { Settings->bEnabled=bEnabled; };
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    if (!TestNotNull(TEXT("创建护主规则世界"),World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit);
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
    };
    World->SetGameState(World->SpawnActor<ADeliveryGameState>());
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    auto* PlayerClass=LoadClass<ADeliveryCharacter>(nullptr,
        TEXT("/Game/Blueprint/Character/DeliverMan/BP_DeliveryMan.BP_DeliveryMan_C"));
    if (!TestNotNull(TEXT("正式玩家蓝图包含护主事件所需的角色配置"),PlayerClass)) return false;
    const auto* PlayerMesh=PlayerClass->GetDefaultObject<ADeliveryCharacter>()->GetMesh();
    if (!TestNotNull(TEXT("玩家蓝图具有物理资产"),PlayerMesh->GetPhysicsAsset())) return false;
    const auto MakePlayer=[&](const FVector& Location)
    {
        auto* PC=World->SpawnActor<APlayerController>();
        auto* State=World->SpawnActor<ADeliverPlayerState>();
        State->SetOwner(PC); PC->PlayerState=State;
        FActorSpawnParameters Spawn;
        Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        // 玩家会在 BeginPlay 启动布娃娃，因此从正式蓝图创建完整的骨骼和物理配置
        auto* Character=World->SpawnActor<ADeliveryCharacter>(PlayerClass,Location,FRotator::ZeroRotator,Spawn);
        PC->Possess(Character);
        Character->GetActiveRagdoll()->StopRagdoll();
        Character->SetActorLocation(Location,false,nullptr,ETeleportType::TeleportPhysics);
        return Character;
    };
    auto* Owner=MakePlayer(FVector(0,0,88));
    auto* Visitor=MakePlayer(FVector(0,400,88));
    auto* OwnerState=Owner->GetPlayerState<ADeliverPlayerState>();
    auto* Npc=World->SpawnActor<ADeliveryCompanionCharacter>(FVector(150,0,88),FRotator::ZeroRotator);
    auto* Target=World->SpawnActor<ADeliveryNpcCharacter>(FVector(300,0,88),FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("创建同伴"),Npc) || !TestNotNull(TEXT("创建受击居民"),Target)) return false;
    auto* Companion=Npc->Companion.Get();
    // 这组用例检查模型选择协战的流程，命中后直接加入的开关在这里关闭
    Companion->bJoinAssistWithoutModel=false;
    // 已结识是本测试前置；这里只验证真实 ASC 事件路由和规则权限，不代替挥拳接触或扶起试玩
    Npc->Encounter.Phase=EDeliveryCompanionEncounterPhase::Joined;
    Companion->OwnerPlayerState=OwnerState;
    Companion->Mode=EDeliveryCompanionMode::Follow;
    Companion->Affinity=Companion->GetTuning().InitialAffinity;
    Companion->RecordRecruitMemory();
    Npc->OnRep_Encounter();
    Companion->RefreshOwnerCombatSubscription();
    if (!TestTrue(TEXT("订阅真实主人 ASC"),Companion->BoundOwnerCombat.IsValid())) return false;
    auto* AI=Cast<AAIController>(Npc->GetController());
    auto* Tree=LoadObject<UBehaviorTree>(nullptr,TEXT("/Game/NPC/Companion/AI/BT_Companion.BT_Companion"));
    if (!TestTrue(TEXT("加载二狗的正式行为树"),AI && Tree)) return false;
    const auto ExecuteModelChoice=[&](const FDeliveryNpcDecisionRequest& Request,const FDeliveryNpcCandidate& Candidate,FName ExpectedResult)
    {
        // 人工替代模型传输，候选校验、正式行为树和角色执行沿正常服务器路径
        FDeliveryNpcDecisionProposal Proposal;
        Proposal.CapabilityVersion=Request.RequestedCapabilityVersion;
        Proposal.RequestId=Request.RequestId; Proposal.WorldEpoch=Request.WorldEpoch; Proposal.NpcId=Request.NpcId;
        Proposal.ContextRevision=Request.ContextRevision; Proposal.PlanRevision=Request.PlanRevision;
        Proposal.CandidateId=Candidate.CandidateId; Proposal.Action=Candidate.Action;
        Proposal.TargetId=Candidate.TargetId; Proposal.EvidenceIds=Candidate.EvidenceIds;
        Proposal.Duration=DeliveryNpc::CandidateDuration(Proposal.Action);
        Npc->Agent->ActiveRequest=Request;
        Npc->Agent->bDecisionPending=true;
        Npc->Agent->ReceiveDecision(Request.RequestId,&Proposal,NAME_None);
        if (Npc->Agent->GetPublicState().Status!=EDeliveryNpcActionStatus::Accepted || !AI->RunBehaviorTree(Tree)) return false;
        auto* Brain=Cast<UBehaviorTreeComponent>(AI->GetBrainComponent());
        if (!Brain) return false;
        ON_SCOPE_EXIT { Brain->StopTree(EBTStopMode::Forced); };
        for (int32 Frame=0;Frame<8 && Npc->Agent->GetPublicState().Status==EDeliveryNpcActionStatus::Accepted;++Frame)
        {
            Npc->Tick(0.f);
            Brain->TickComponent(.02f,LEVELTICK_All,nullptr);
        }
        const auto Result=Npc->Agent->GetPublicState();
        return Result.Status==EDeliveryNpcActionStatus::Succeeded && Result.Action==Candidate.Action && Result.Reason==ExpectedResult;
    };
    const auto BuildSpokenRequest=[&](ADeliveryCharacter* Speaker,const TCHAR* Line,FDeliveryNpcDecisionRequest& Request)
    {
        FDeliveryNpcObservation Speech;
        Speech.EventId=FGuid::NewGuid(); Speech.Kind=TEXT("PlayerSpeech"); Speech.Subject=Speaker; Speech.Addressee=Npc;
        Speech.Source=EDeliveryNpcEvidenceSource::Heard; Speech.bNeedsReply=true; Speech.Description=Line;
        return Npc->Agent->BeginListening(Speaker,Speech.EventId) && Npc->Agent->ObserveEvent(Speech) &&
            Npc->Agent->BuildDecisionRequest(FGuid::NewGuid(),Request);
    };
    TestTrue(TEXT("二狗拳头不会击倒任何人"),Npc->Combat->bHarmlessStrikes);
    TestTrue(TEXT("二狗默认一拳倒地"),Npc->Combat->bKnockDownOnAnyPunch);
    TestTrue(TEXT("二狗允许编号去重的倒地再命中"),Npc->Combat->bReceiveHitsWhileDown);

    DeliveryCombatEvents::ReportHit(Visitor,Target,FGuid::NewGuid(),Target->GetActorLocation());
    TestNull(TEXT("旁人的交战不建立主人护主目标"),Companion->GetAssistTarget());
    const FGuid ConfirmedPunch=FGuid::NewGuid();
    {
        FDeliveryNpcObservation StreetEvent;
        StreetEvent.Kind=TEXT("CompanionAssistTestStreetEvent"); StreetEvent.Source=EDeliveryNpcEvidenceSource::ActionResult;
        StreetEvent.Subject=Visitor; StreetEvent.Lifetime=30;
        for (int32 Index=0;Index<32;++Index)
        {
            StreetEvent.EventId=FGuid::NewGuid();
            Npc->Agent->ObserveEvent(StreetEvent);
        }
        FDeliveryNpcDecisionRequest InFlight;
        if (!TestTrue(TEXT("旧环境请求占满真实证据窗口"),Npc->Agent->BuildDecisionRequest(FGuid::NewGuid(),InFlight) &&
            InFlight.Evidence.Num()==32)) return false;
        Npc->Agent->ActiveRequest=InFlight;
        ON_SCOPE_EXIT { Npc->Agent->ActiveRequest={}; };
        DeliveryCombatEvents::ReportHit(Owner,Target,ConfirmedPunch,Target->GetActorLocation());
        StreetEvent.EventId=FGuid::NewGuid();
        Npc->Agent->ObserveEvent(StreetEvent);
        Companion->UpdateAssist();
        if (!TestTrue(TEXT("旧请求在途时的新现场事件保留那次真实主人命中"),Npc->Agent->HasObservationEvent(ConfirmedPunch) &&
            Companion->CanJoinAssist(OwnerState) && Companion->GetAssistEvidenceId()==ConfirmedPunch)) return false;
    }
    if (!TestTrue(TEXT("主人实际命中只登记可选目标，行为树仍未起斗"),Companion->GetAssistTarget()==Target &&
        !Companion->HasCommittedAssist() && !Npc->Agent->HasActiveAction())) return false;
    const int32 PendingRevision=Companion->GetAssistRevision();
    TestFalse(TEXT("初次真实命中等待模型选择，尚不能直接授权 Fight"),Npc->CanExecuteGameplayRule(EDeliveryNpcAction::Fight,Target,PendingRevision));
    FDeliveryNpcDecisionRequest PendingAssist;
    if (!TestTrue(TEXT("实际主人命中为模型提供当前目标和原命中凭据"),
        Npc->Agent->BuildDecisionRequest(FGuid::NewGuid(),PendingAssist) && PendingAssist.bCompanionAssistPending &&
        PendingAssist.CompanionAssistRevision==PendingRevision && PendingAssist.Candidates.ContainsByPredicate([&](const auto& Candidate)
        { return Candidate.Action==EDeliveryNpcAction::JoinAssist && Candidate.TargetId==Npc->Agent->FindKnownSubjectId(Target) &&
            Candidate.EvidenceIds.Num()==1 && Candidate.EvidenceIds[0]==ConfirmedPunch; }))) return false;
    TestFalse(TEXT("协战候选没有逃跑动作"),PendingAssist.AllowedActions.Contains(EDeliveryNpcAction::Flee));
    TestFalse(TEXT("访客不能确认主人命中的目标"),Companion->JoinAssist(Visitor->GetPlayerState(),PendingRevision));
    TestFalse(TEXT("过期候选修订不能确认目标"),Companion->JoinAssist(OwnerState,PendingRevision-1));
    const auto* ApprovedJoin=PendingAssist.Candidates.FindByPredicate([](const auto& Candidate)
    { return Candidate.Action==EDeliveryNpcAction::JoinAssist; });
    if (!TestTrue(TEXT("模型选择真实命中目标，正式行为树确认持续协战"),ApprovedJoin &&
        ExecuteModelChoice(PendingAssist,*ApprovedJoin,TEXT("AssistArrangementAccepted")) && Companion->HasCommittedAssist())) return false;
    const int32 Revision=Companion->GetAssistRevision();
    TestTrue(TEXT("确认后当前规则可以授权原有 Fight"),Npc->CanExecuteGameplayRule(EDeliveryNpcAction::Fight,Target,Revision));
    TestFalse(TEXT("旧规则版本不能发拳"),Npc->CanExecuteGameplayRule(EDeliveryNpcAction::Fight,Target,Revision-1));
    TestFalse(TEXT("模型仍不能选择二狗 Fight"),Npc->CanExecuteForRequester(EDeliveryNpcAction::Fight,Target,OwnerState));
    TestFalse(TEXT("访客不能伪装护主命令"),Npc->CanExecuteForRequester(EDeliveryNpcAction::Fight,Target,Visitor->GetPlayerState()));
    TestTrue(TEXT("真实证据走专用 Agent 规则采纳入口"),Npc->Agent->RequestGameplayRuleAction(
        EDeliveryNpcAction::Fight,Target,Revision,ConfirmedPunch,5));
    const auto Accepted=Npc->Agent->GetPublicState();
    TestEqual(TEXT("来源明确标记为游戏规则"),Accepted.Origin,EDeliveryNpcActionOrigin::GameplayRule);
    TestFalse(TEXT("同代次不能每帧重启交战"),Npc->Agent->RequestGameplayRuleAction(
        EDeliveryNpcAction::Fight,Target,Revision,ConfirmedPunch,5));
    DeliveryCombatEvents::ReportHit(Owner,Target,ConfirmedPunch,Target->GetActorLocation());
    TestEqual(TEXT("重复同一拳不改护主代次"),Companion->GetAssistRevision(),Revision);
    TestEqual(TEXT("重复同一拳不换已接动作"),Npc->Agent->GetPublicState().ActionId,Accepted.ActionId);
    DeliveryCombatEvents::ReportHit(Owner,Target,FGuid::NewGuid(),Target->GetActorLocation());
    DeliveryCombatEvents::ReportHit(Visitor,Owner,FGuid::NewGuid(),Owner->GetActorLocation());
    TestTrue(TEXT("重复或另一个对手的新命中不会换掉已确认目标和真实凭据"),Companion->GetAssistTarget()==Target &&
        Companion->GetAssistEvidenceId()==ConfirmedPunch && Companion->GetAssistRevision()==Revision);

    FDeliveryNpcPublicState Running;
    if (!TestTrue(TEXT("按原 Agent 入口领取本轮 Fight"),Npc->Agent->StartAction(Running))) return false;
    Npc->Agent->TryFinishAction(Running.ActionId,EDeliveryNpcActionStatus::Succeeded,TEXT("FightTimeLimit"));
    TestTrue(TEXT("共享短回合结束仍保留同一确认目标"),Companion->HasCommittedAssist() && Companion->GetAssistTarget()==Target &&
        Companion->GetAssistRevision()==Revision);
    // 过期只移除交谈证据，持续安排仍保存那次真实命中的服务器凭据
    World->TimeSeconds+=61.0;
    TestFalse(TEXT("旧命中已经离开活动观察窗口"),Npc->Agent->HasObservationEvent(ConfirmedPunch));
    TestTrue(TEXT("已确认的同一真实凭据可授权下一轮共有 Fight"),Npc->Agent->RequestGameplayRuleAction(
        EDeliveryNpcAction::Fight,Target,Revision,ConfirmedPunch,5));
    FDeliveryNpcPublicState Resumed;
    if (!TestTrue(TEXT("真实观察过期后原 Fight 节点仍可领取该次规则动作"),Npc->Agent->StartAction(Resumed) &&
        Resumed.Action==EDeliveryNpcAction::Fight && Resumed.Target==Target && Resumed.RuleRevision==Revision)) return false;
    TestFalse(TEXT("其他事件编号不能冒充同一持续安排的凭据"),Companion->IsAssistEvidenceCurrent(Target,FGuid::NewGuid()));

    const FVector TargetPosition=Target->GetActorLocation();
    Target->SetActorLocation(TargetPosition+FVector(5000,0,0),false,nullptr,ETeleportType::TeleportPhysics);
    Companion->UpdateAssist();
    TestTrue(TEXT("对手暂时远离只等机会，不撤销协战"),Companion->HasCommittedAssist() &&
        !Npc->CanExecuteGameplayRule(EDeliveryNpcAction::Fight,Target,Revision));
    Target->SetActorLocation(TargetPosition,false,nullptr,ETeleportType::TeleportPhysics);
    if (!TestTrue(TEXT("对手可以真实进入倒地状态"),Target->Combat->SetDeliberateProne(true))) return false;
    Companion->UpdateAssist();
    TestTrue(TEXT("对手倒地保留目标但禁止继续出拳"),Companion->GetAssistTarget()==Target && Companion->HasCommittedAssist() &&
        !Npc->CanExecuteGameplayRule(EDeliveryNpcAction::Fight,Target,Revision));
    Target->Combat->SetDeliberateProne(false);

    TestFalse(TEXT("访客或旧修订都不能让二狗收手"),Companion->StopAssist(Visitor->GetPlayerState(),Revision) ||
        Companion->StopAssist(OwnerState,Revision-1));
    const int32 BeforeStopCommand=Companion->GetCommandRevision();
    FDeliveryNpcDecisionRequest VisitorStop;
    if (!TestTrue(TEXT("访客说收手仍不能获得指令候选"),BuildSpokenRequest(Visitor,TEXT("别打了"),VisitorStop) &&
        !VisitorStop.AllowedActions.Contains(EDeliveryNpcAction::StopAssist))) return false;
    Npc->Agent->CancelPlan(TEXT("VisitorStopChecked"));
    FDeliveryNpcDecisionRequest OwnerStop;
    if (!TestTrue(TEXT("主人明确说收手获得当轮候选"),BuildSpokenRequest(Owner,TEXT("别打了，收手"),OwnerStop))) return false;
    const auto* ApprovedStop=OwnerStop.Candidates.FindByPredicate([](const auto& Candidate)
    { return Candidate.Action==EDeliveryNpcAction::StopAssist && !Candidate.TargetId.IsValid() && Candidate.EvidenceIds.IsEmpty(); });
    if (!TestTrue(TEXT("模型选择收手后正式行为树才解除持续协战"),ApprovedStop &&
        ExecuteModelChoice(OwnerStop,*ApprovedStop,TEXT("AssistStoppedByOwner")))) return false;
    TestTrue(TEXT("收手保留原跟随安排"),!Companion->HasCommittedAssist() && !Companion->GetAssistTarget() &&
        Companion->GetMode()==EDeliveryCompanionMode::Follow && Companion->GetCommandRevision()==BeforeStopCommand);
    DeliveryCombatEvents::ReportHit(Owner,Target,ConfirmedPunch,Target->GetActorLocation());
    TestNull(TEXT("取消后重放旧拳不会重新开打"),Companion->GetAssistTarget());
    DeliveryCombatEvents::ReportHit(Owner,Target,FGuid::NewGuid(),Target->GetActorLocation());
    TestTrue(TEXT("已叫停的同一对手不能靠重复互殴自动重开"),!Companion->CanJoinAssist(nullptr) && Companion->CanJoinAssist(OwnerState) &&
        !Companion->HasCommittedAssist());
    if (!TestTrue(TEXT("主人明确允许后可重新协战"),Companion->JoinAssist(OwnerState,Companion->GetAssistRevision()))) return false;
    TestTrue(TEXT("主人可以改为等候"),Companion->ApplyCommand(EDeliveryNpcAction::Stay,OwnerState,Companion->GetCommandRevision()));
    TestTrue(TEXT("改为等候会解除协战并记录对手"),!Companion->HasCommittedAssist() && !Companion->GetAssistTarget() &&
        Companion->StoppedAssistTarget.Get()==Target && Companion->GetMode()==EDeliveryCompanionMode::Stay);
    const int32 BeforeOwnerHit=Companion->GetAffinity();
    const FGuid OwnerHit=FGuid::NewGuid();
    DeliveryCombatEvents::ReportHit(Owner,Npc,OwnerHit,Npc->GetActorLocation());
    TestNull(TEXT("主人打二狗时不会反击主人"),Companion->GetAssistTarget());
    TestEqual(TEXT("真实主人命中事件改变亲密度"),Companion->GetAffinity(),BeforeOwnerHit-Companion->GetTuning().OwnerHitAffinityLoss);
    const int32 AfterOwnerHit=Companion->GetAffinity();
    DeliveryCombatEvents::ReportHit(Owner,Npc,OwnerHit,Npc->GetActorLocation());
    TestEqual(TEXT("重复命中事件不再次扣亲密度"),Companion->GetAffinity(),AfterOwnerHit);
    FDeliveryNpcDecisionRequest MemoryRequest;
    Companion->ExtendDecisionRequest(MemoryRequest,OwnerState);
    TestTrue(TEXT("记忆保留服务器确认的命中编号"),MemoryRequest.CompanionMemories.ContainsByPredicate(
        [OwnerHit](const auto& Item){ return Item.EventId==OwnerHit && Item.Kind==TEXT("OwnerHit"); }));
    TestFalse(TEXT("历史记忆不补成当前行动证据"),MemoryRequest.Evidence.ContainsByPredicate(
        [OwnerHit](const auto& Item){ return Item.EventId==OwnerHit; }));
    TestFalse(TEXT("模型请求不暴露亲密度数字字段"),DeliveryNpc::EncodeRequest(MemoryRequest).Contains(TEXT("affinity")));

    auto* SameOwner=World->SpawnActor<ADeliveryCompanionCharacter>(FVector(350,0,88),FRotator::ZeroRotator);
    SameOwner->Companion->OwnerPlayerState=OwnerState;
    DeliveryCombatEvents::ReportHit(Owner,SameOwner,FGuid::NewGuid(),SameOwner->GetActorLocation());
    TestNull(TEXT("同属主同伴不能成为目标"),Companion->GetAssistTarget());

    DeliveryCombatEvents::ReportHit(Visitor,Owner,FGuid::NewGuid(),Owner->GetActorLocation());
    TestEqual(TEXT("主人被另一玩家实际命中会帮助主人"),Companion->GetAssistTarget(),static_cast<AActor*>(Visitor));
    if (!TestTrue(TEXT("选择原来伤害主人的对手"),Companion->JoinAssist(nullptr,Companion->GetAssistRevision()))) return false;
    const int32 DefenceRevision=Companion->GetAssistRevision();
    Npc->Agent->BeginListening(Owner,FGuid::NewGuid());
    Companion->UpdateAssist();
    TestEqual(TEXT("玩家说话暂停但保留当前目标"),Companion->GetAssistRevision(),DefenceRevision);
    TestTrue(TEXT("护主重试没有抢走正在倾听的玩家"),Npc->Agent->IsListening());
    FDeliveryNpcDecisionRequest StopRequest;
    TestTrue(TEXT("主人本轮对话有独立收手候选"),Npc->Agent->BuildDecisionRequest(FGuid::NewGuid(),StopRequest) &&
        StopRequest.bCompanionAssistCommitted && StopRequest.Candidates.ContainsByPredicate([](const auto& Candidate)
        { return Candidate.Action==EDeliveryNpcAction::StopAssist && !Candidate.TargetId.IsValid() && Candidate.EvidenceIds.IsEmpty(); }));
    Npc->Agent->CancelPlan(TEXT("TestListeningEnded"));
    TestTrue(TEXT("测试倒地前置"),Npc->Combat->SetDeliberateProne(true));
    Companion->UpdateAssist();
    TestEqual(TEXT("倒地暂停仍保留对手"),Companion->GetAssistTarget(),static_cast<AActor*>(Visitor));
    TestFalse(TEXT("倒地时规则不可执行"),Npc->CanExecuteGameplayRule(EDeliveryNpcAction::Fight,Visitor,DefenceRevision));
    TestTrue(TEXT("结束测试倒地前置"),Npc->Combat->SetDeliberateProne(false));
    TestTrue(TEXT("起身后保留同一已确认规则"),Npc->CanExecuteGameplayRule(EDeliveryNpcAction::Fight,Visitor,DefenceRevision));

    // 控制对象变化只复核身份，不重新绑定或丢掉 PlayerState 上的事件订阅
    auto* PC=OwnerState->GetPlayerController();
    const auto BoundASC=Companion->BoundOwnerCombat;
    PC->UnPossess();
    Companion->RefreshOwnerCombatSubscription();
    TestEqual(TEXT("换 Pawn 的空档保留同一个主人"),Companion->GetOwnerPlayerState(),static_cast<APlayerState*>(OwnerState));
    TestEqual(TEXT("换 Pawn 不重建主人 ASC 绑定"),Companion->BoundOwnerCombat.Get(),BoundASC.Get());
    PC->Possess(Owner);
    Visitor->Destroy();
    TestNull(TEXT("目标销毁立即撤销目标"),Companion->GetAssistTarget());
    DeliveryCombatEvents::ReportHit(Owner,Target,FGuid::NewGuid(),Target->GetActorLocation());
    Companion->PendingAssistExpiresAt=World->GetTimeSeconds();
    Companion->UpdateAssist();
    TestNull(TEXT("未确认的过期事件不能启动协战"),Companion->GetAssistTarget());
    Companion->ReleaseOwner();
    TestTrue(TEXT("解绑后清理旧主人的私有当局记忆"),Companion->Memories.IsEmpty());
    TestFalse(TEXT("解绑主人时移除 ASC 事件订阅"),Companion->BoundOwnerCombat.IsValid());
    DeliveryCombatEvents::ReportHit(Owner,Target,FGuid::NewGuid(),Target->GetActorLocation());
    TestNull(TEXT("旧主人之后的命中不再指挥同伴"),Companion->GetAssistTarget());
    return true;
}
#endif
