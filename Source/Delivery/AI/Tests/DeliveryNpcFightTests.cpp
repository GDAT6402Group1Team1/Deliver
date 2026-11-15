#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AbilitySystem/DeliverAbilitySystemComponent.h"
#include "AbilitySystem/DeliverAttributeSet.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/BehaviorTree/BTTask_DeliveryExecuteNpcAction.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Characters/DeliveryNpcAIController.h"
#include "AI/Conversation/DeliveryConversationTurnSubsystem.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AI/Presentation/DeliveryNpcBodyAnimInstance.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Components/AudioComponent.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "Camera/CameraModifier_CameraShake.h"
#include "Camera/DeliveryDoorHitCameraShake.h"
#include "Camera/PlayerCameraManager.h"
#include "Character/DeliveryCharacter.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Player/DeliverPlayerState.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcFightTest,"Delivery.AI.Fight",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryNpcFightTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool bWasEnabled=Settings->bEnabled;
    Settings->bEnabled=false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
        Settings->bEnabled=bWasEnabled;
    };
    auto* Floor=World->SpawnActor<AActor>();
    auto* Box=NewObject<UBoxComponent>(Floor);
    Floor->SetRootComponent(Box); Box->SetBoxExtent(FVector(3000,3000,20));
    Box->SetCollisionProfileName(TEXT("BlockAll")); Box->RegisterComponent(); Floor->SetActorLocation(FVector(0,0,-20));
    auto* ManClass=LoadClass<ADeliveryNpcCharacter>(nullptr,TEXT("/Game/NPC/Residents/BP_ResidentLoud.BP_ResidentLoud_C"));
    auto* WomanClass=LoadClass<ADeliveryNpcCharacter>(nullptr,TEXT("/Game/NPC/Residents/BP_ResidentShrewd.BP_ResidentShrewd_C"));
    if (!TestNotNull(TEXT("正式男居民蓝图存在"),ManClass) || !TestNotNull(TEXT("正式女居民蓝图存在"),WomanClass)) return false;

    struct FCase
    {
        ADeliveryNpcCharacter* Attacker=nullptr;
        ADeliveryNpcCharacter* Target=nullptr;
        // 测试手动驱动任务，没有行为树资产持有它；跨 World Tick 必须保留强引用
        TStrongObjectPtr<UBehaviorTreeComponent> Tree;
        TStrongObjectPtr<UBTTask_DeliveryExecuteNpcAction> Task;
        FGuid ActionId;
        TSet<FGuid> PunchIds;
        bool bParentTimeMatches=true;
        double StartedAt=0;
        double FinishedAt=-1;
    };
    const auto SpawnCase=[&]()
    {
        FCase Case;
        FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Case.Attacker=World->SpawnActor<ADeliveryNpcCharacter>(ManClass,FVector(0,0,90),FRotator::ZeroRotator,Spawn);
        Case.Target=World->SpawnActor<ADeliveryNpcCharacter>(WomanClass,FVector(120,0,90),FRotator(0,180,0),Spawn);
        Case.Target->SetActorLocation(FVector(Case.Attacker->Combat->GetStrikeApproachDistance(Case.Target)-.5f,0,90));
        for (auto* Npc:{Case.Attacker,Case.Target})
        {
            Npc->SetActorTickEnabled(false);
            Npc->GetMesh()->VisibilityBasedAnimTickOption=EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
            Npc->GetMesh()->bEnableUpdateRateOptimizations=false;
            // 正式蓝图的树先停下，本检查只让下面这一份任务执行动作
            if (auto* AI=Cast<AAIController>(Npc->GetController()))
            {
                if (auto* NpcController=Cast<ADeliveryNpcAIController>(AI)) NpcController->bKeepDistanceWhenIdle=false;
                if (auto* Tree=Cast<UBehaviorTreeComponent>(AI->GetBrainComponent()))
                { Tree->StopTree(EBTStopMode::Forced); Tree->SetComponentTickEnabled(false); }
            }
        }
        auto* AI=Cast<AAIController>(Case.Attacker->GetController());
        if (AI)
        {
            Case.Tree.Reset(NewObject<UBehaviorTreeComponent>(AI));
            Case.Tree->RegisterComponent(); Case.Tree->SetComponentTickEnabled(false);
        }
        return Case;
    };
    const auto Tick=[&](FCase& Case,float Seconds)
    {
        for (int32 Step=0;Step<FMath::CeilToInt(Seconds/.02f);++Step)
        {
            ++GFrameCounter; World->Tick(LEVELTICK_All,.02f);
            if (Case.Task && Case.Attacker->Agent->GetPublicState().Status==EDeliveryNpcActionStatus::Running)
                Case.Task->TickTask(*Case.Tree,nullptr,.02f);
            const FGuid Punch=Case.Attacker->Combat->GetCurrentPunchId();
            if (Punch.IsValid() && !Case.PunchIds.Contains(Punch))
            {
                Case.PunchIds.Add(Punch);
                Case.bParentTimeMatches&=Case.Attacker->Combat->GetStrikeStartTime(Case.ActionId)>=0;
            }
            if (Case.Task && Case.FinishedAt<0 && DeliveryNpc::IsTerminal(Case.Attacker->Agent->GetPublicState().Status))
                Case.FinishedAt=World->GetTimeSeconds();
        }
    };
    const auto Begin=[&](FCase& Case,EDeliveryNpcAction Kind,float Duration=5.f,const TCHAR* AmbientSpeech=nullptr,
        AActor* ActionTarget=nullptr)
    {
        if (!Case.Tree) return false;
        if (Case.Task) Case.Task->OnTaskFinished(*Case.Tree,nullptr,EBTNodeResult::Succeeded);
        Case.Attacker->Agent->CancelPlan(TEXT("NextFightTest"));
        if (!ActionTarget) ActionTarget=Case.Target;
        const FName SeenKind=Cast<ADeliveryCharacter>(ActionTarget)?TEXT("SeenPlayer"):TEXT("SeenNpc");
        Case.Attacker->ObserveFact(SeenKind,ActionTarget,TEXT("对方就在面前"),EDeliveryNpcEvidenceSource::Seen);
        FDeliveryNpcDecisionRequest Request;
        if (!Case.Attacker->Agent->BuildDecisionRequest(World->GetSubsystem<UDeliveryAiGatewaySubsystem>()->GetWorldEpoch(),Request)) return false;
        const auto* Candidate=Request.Candidates.FindByPredicate([&](const auto& Item)
        {
            return Item.Action==Kind && Case.Attacker->Agent->KnownSubjects.FindRef(Item.TargetId).Get()==ActionTarget;
        });
        if (!Candidate) return false;
        // 使用真实候选与回复校验，仅替代传输层，不启用后台或发送 HTTP
        Case.Attacker->Agent->ActiveRequest=Request;
        Case.Attacker->Agent->bDecisionPending=true;
        FDeliveryNpcDecisionProposal Proposal;
        Proposal.WorldEpoch=Request.WorldEpoch; Proposal.NpcId=Request.NpcId; Proposal.RequestId=Request.RequestId;
        Proposal.ContextRevision=Request.ContextRevision; Proposal.PlanRevision=Request.PlanRevision;
        Proposal.Action=Candidate->Action; Proposal.TargetId=Candidate->TargetId; Proposal.EvidenceIds=Candidate->EvidenceIds;
        Proposal.Duration=Duration; Proposal.Emotion=TEXT("Angry");
        if (AmbientSpeech)
        {
            Proposal.Speech=AmbientSpeech;
            Proposal.bHasSpeechTarget=true;
            Proposal.SpeechTargetId=Candidate->TargetId;
        }
        Case.Attacker->Agent->ReceiveDecision(Request.RequestId,&Proposal,NAME_None);
        if (Case.Attacker->Agent->GetPublicState().Status!=EDeliveryNpcActionStatus::Accepted) return false;
        Case.ActionId=Case.Attacker->Agent->GetPublicState().ActionId;
        Case.PunchIds.Reset(); Case.bParentTimeMatches=true;
        Case.StartedAt=World->GetTimeSeconds(); Case.FinishedAt=-1;
        Case.Task.Reset(NewObject<UBTTask_DeliveryExecuteNpcAction>(Case.Tree.Get()));
        return Case.Task->ExecuteTask(*Case.Tree,nullptr)==EBTNodeResult::InProgress;
    };
    const auto Close=[&](FCase& Case)
    {
        if (Case.Task) Case.Task->OnTaskFinished(*Case.Tree,nullptr,EBTNodeResult::Aborted);
        Case.Attacker->Agent->CancelPlan(TEXT("FightTestFinished"));
        Case.Tree->DestroyComponent();
        Case.Task.Reset(); Case.Tree.Reset();
        if (IsValid(Case.Target)) Case.Target->Destroy();
        Case.Attacker->Destroy();
        Case.Target=nullptr; Case.Attacker=nullptr;
    };
    const auto PrepareUpdate=[&](FCase& Case,EDeliveryNpcAction Action,float Duration,
        FDeliveryNpcDecisionRequest& Request,FDeliveryNpcDecisionProposal& Proposal)
    {
        auto* Agent=Case.Attacker->Agent.Get();
        if (!Agent->BuildDecisionRequest(World->GetSubsystem<UDeliveryAiGatewaySubsystem>()->GetWorldEpoch(),Request)) return false;
        const auto* Candidate=Request.Candidates.FindByPredicate([&](const auto& Item)
        {
            return Item.Action==Action && (Action==EDeliveryNpcAction::Wait ||
                Agent->KnownSubjects.FindRef(Item.TargetId).Get()==Case.Target);
        });
        if (!Candidate) return false;
        Agent->ActiveRequest=Request; Agent->bDecisionPending=true;
        Proposal={}; Proposal.WorldEpoch=Request.WorldEpoch; Proposal.NpcId=Request.NpcId;
        Proposal.RequestId=Request.RequestId; Proposal.ContextRevision=Request.ContextRevision; Proposal.PlanRevision=Request.PlanRevision;
        Proposal.Action=Action; Proposal.TargetId=Candidate->TargetId; Proposal.EvidenceIds=Candidate->EvidenceIds;
        Proposal.Duration=Duration; Proposal.Emotion=TEXT("Angry");
        return true;
    };

    auto TooFar=SpawnCase(); Tick(TooFar,.2f);
    TooFar.Target->SetActorLocation(FVector(120,0,90));
    TestFalse(TEXT("旧120厘米范围内也不能隔空开始出拳"),TooFar.Attacker->Combat->StartStrike(TooFar.Target,FGuid::NewGuid()));
    Tick(TooFar,.6f);
    TestFalse(TEXT("距离判定不会直接让远处目标受击"),TooFar.Target->Combat->IsReactingToHit());
    TooFar.Target->SetActorLocation(FVector(TooFar.Attacker->Combat->GetStrikeApproachDistance(TooFar.Target)-.5f,0,90));
    TooFar.Attacker->Combat->StrikeHandBone=TEXT("MissingHandForTest");
    TestFalse(TEXT("缺失拳头骨骼时拒绝出拳，不退回距离命中"),TooFar.Attacker->Combat->StartStrike(TooFar.Target,FGuid::NewGuid()));
    Close(TooFar);

    auto Dodge=SpawnCase(); Tick(Dodge,.2f);
    if (!TestTrue(TEXT("准备真实拳路未接触的攻击"),Begin(Dodge,EDeliveryNpcAction::StrikeOnce))) return false;
    Tick(Dodge,.06f); Dodge.Target->SetActorLocation(FVector(120,0,90)); Tick(Dodge,.5f);
    bool bDodgedHit=true;
    TestTrue(TEXT("躲出拳路后这一拳仍有明确结果"),Dodge.Attacker->Combat->GetStrikeResult(Dodge.ActionId,bDodgedHit));
    TestFalse(TEXT("目标仍在旧160厘米范围内也可以躲过拳头"),bDodgedHit);
    TestFalse(TEXT("未接触的目标不倒地"),Dodge.Target->IsKnockedDown());
    Close(Dodge);

    auto WallCase=SpawnCase(); Tick(WallCase,.2f);
    if (!TestTrue(TEXT("先启动攻击再检查新增阻挡"),Begin(WallCase,EDeliveryNpcAction::StrikeOnce))) return false;
    auto* Wall=World->SpawnActor<AActor>(); auto* WallBox=NewObject<UBoxComponent>(Wall);
    Wall->SetRootComponent(WallBox); WallBox->SetBoxExtent(FVector(2,100,100));
    WallBox->SetCollisionProfileName(TEXT("BlockAll")); WallBox->RegisterComponent();
    Wall->SetActorLocation((WallCase.Attacker->GetActorLocation()+WallCase.Target->GetActorLocation())*.5f);
    Tick(WallCase,.5f); bool bWallHit=true;
    TestTrue(TEXT("阻挡后结束拳头判定"),WallCase.Attacker->Combat->GetStrikeResult(WallCase.ActionId,bWallHit));
    TestFalse(TEXT("拳头不能穿过墙体让目标受击"),bWallHit);
    TestFalse(TEXT("墙后目标保持站立"),WallCase.Target->IsKnockedDown());
    Wall->Destroy(); Close(WallCase);

    auto Main=SpawnCase(); Tick(Main,.2f);
    // 模拟尚未生成结果的普通闲聊，受击后这份旧请求应停止
    Main.Target->Agent->ActiveRequest.RequestId=FGuid::NewGuid();
    Main.Target->Agent->bDecisionPending=true;
    TestFalse(TEXT("模型返回过短Fight时拒绝执行"),Begin(Main,EDeliveryNpcAction::Fight,.5f));
    TestFalse(TEXT("被拒绝的短动作不会留下挥拳"),Main.Attacker->Combat->IsStriking());
    if (!TestTrue(TEXT("模型候选中的Fight通过校验并由行为树启动"),Begin(Main,EDeliveryNpcAction::Fight))) return false;
    Tick(Main,.44f);
    TestEqual(TEXT("第一拳已实际命中"),Main.Attacker->Combat->GetFightHitCount(),1);
    TestFalse(TEXT("轻拳不会立即击倒居民"),Main.Target->IsKnockedDown());
    TestTrue(TEXT("目标进入短暂后仰"),Main.Target->Combat->IsReactingToHit());
    auto* HitPresentation=Main.Target->Presentation.Get();
    TestTrue(TEXT("轻拳播放受击者自己的短音"),HitPresentation->ReactionAudio &&
        HitPresentation->ReactionAudio->Sound==HitPresentation->VoiceSet->Hit &&
        HitPresentation->ReactionEventId==Main.Attacker->Combat->GetCurrentPunchId());
    TestFalse(TEXT("第一次轻拳会取消此前的闲聊请求"),Main.Target->Agent->IsDecisionPending());
    FDeliveryNpcDecisionRequest Retaliation;
    TestTrue(TEXT("被打后可以结合现场生成新请求"),Main.Target->Agent->BuildDecisionRequest(FGuid::NewGuid(),Retaliation));
    Main.Target->Agent->ActiveRequest=Retaliation; Main.Target->Agent->bDecisionPending=true;
    TestTrue(TEXT("新请求已经记录攻击者"),Main.Target->Agent->IsRespondingToAttackFrom(Main.Attacker));
    auto* HitBody=Cast<UDeliveryNpcBodyAnimInstance>(Main.Target->GetMesh()->GetAnimInstance());
    if (!TestNotNull(TEXT("受击者使用正式主动画图"),HitBody)) return false;
    TestEqual(TEXT("正式动画显示受击姿势"),HitBody->PoseIndex,static_cast<int32>(UDeliveryNpcBodyAnimInstance::EPose::Hit));
    TestTrue(TEXT("受击片段沿服务器时间推进"),HitBody->HitTime>0);
    Tick(Main,4.7f);
    TestEqual(TEXT("五秒交战样本中完成三次挥拳和收拳"),Main.Attacker->Combat->GetFightPunchCount(),3);
    TestEqual(TEXT("近身三拳分别发生真实命中"),Main.Attacker->Combat->GetFightHitCount(),3);
    TestEqual(TEXT("每拳使用不同编号"),Main.PunchIds.Num(),3);
    TestFalse(TEXT("整轮编号不充当单拳编号"),Main.PunchIds.Contains(Main.ActionId));
    TestTrue(TEXT("三拳均能按同一个父动作读取播放时间"),Main.bParentTimeMatches);
    TestFalse(TEXT("第三拳后仍然站立"),Main.Target->IsKnockedDown());
    TestTrue(TEXT("连续轻拳不会反复取消已经了解攻击的新请求"),Main.Target->Agent->IsDecisionPending());
    TestTrue(TEXT("五秒内停止且没有等到Agent超时兜底"),Main.FinishedAt>=0 && Main.FinishedAt-Main.StartedAt<=5.02);
    TestEqual(TEXT("没有新的模型批准时按原期限停止"),Main.Attacker->Agent->GetPublicState().Reason,FName(TEXT("FightTimeLimit")));
    FDeliveryNpcDecisionRequest Followup;
    TestTrue(TEXT("下一轮上下文可正常生成"),Main.Attacker->Agent->BuildDecisionRequest(FGuid::NewGuid(),Followup));
    TestTrue(TEXT("结果记录挥拳和命中次数，不声称打赢"),Followup.Evidence.ContainsByPredicate([](const auto& Item)
    {
        return Item.Kind==TEXT("ActionFinished") && Item.Description.Contains(TEXT("挥拳 3 次")) &&
            Item.Description.Contains(TEXT("命中 3 次")) && !Item.Description.Contains(TEXT("打赢"));
    }));
    Tick(Main,4.f);
    if (!TestTrue(TEXT("冷却后可以开始下一轮，累计轻拳仍有效"),Begin(Main,EDeliveryNpcAction::Fight))) return false;
    Tick(Main,.44f);
    TestTrue(TEXT("第四次累计轻拳使居民倒地"),Main.Target->IsKnockedDown());
    TestEqual(TEXT("目标倒地后立即停手"),Main.Attacker->Agent->GetPublicState().Reason,FName(TEXT("TargetDown")));
    TestFalse(TEXT("倒地目标不会留下待命中的拳"),Main.Attacker->Combat->IsStriking());
    Close(Main);

    auto Pair=SpawnCase(); Tick(Pair,.2f);
    if (!TestTrue(TEXT("双方交战先由一人开始"),Begin(Pair,EDeliveryNpcAction::Fight,5.f,
        TEXT("等一等，等我把话说完！你刚才从街那头走过来，到现在都没给我一个说法。")))) return false;
    auto* Turns=World->GetSubsystem<UDeliveryConversationTurnSubsystem>();
    const FGuid FightingReply=Pair.Attacker->Agent->GetPublicState().GetReplyId();
    if (!TestTrue(TEXT("先开始一条系统跟踪的战斗台词"),Turns && Turns->IsNpcReplyValid(FightingReply) &&
        Pair.Attacker->Presentation->GetSpeech().bActive)) return false;
    Tick(Pair,.44f);
    FCase Reply;
    Reply.Attacker=Pair.Target; Reply.Target=Pair.Attacker;
    Reply.Tree.Reset(NewObject<UBehaviorTreeComponent>(CastChecked<AAIController>(Reply.Attacker->GetController())));
    Reply.Tree->RegisterComponent(); Reply.Tree->SetComponentTickEnabled(false);
    // 对方模型独立选择还手，短暂后仰不作废它的决定
    if (!TestTrue(TEXT("受轻拳后仍能接受自己的交战决定"),Begin(Reply,EDeliveryNpcAction::Fight))) return false;
    bool bKeptFightAfterHit=false;
    bool bPunchedAgainAfterHit=false;
    bool bSpeechInterruptedByHit=false;
    int32 PunchesWhenHit=INDEX_NONE;
    for (int32 Step=0;Step<270;++Step)
    {
        const bool bSpeechWasActive=Pair.Attacker->Presentation->GetSpeech().bActive && Turns->IsNpcReplyValid(FightingReply);
        Tick(Pair,.02f);
        if (Reply.Attacker->Agent->GetPublicState().Status==EDeliveryNpcActionStatus::Running)
            Reply.Task->TickTask(*Reply.Tree,nullptr,.02f);
        const auto State=Pair.Attacker->Agent->GetPublicState();
        bSpeechInterruptedByHit|=bSpeechWasActive && Pair.Attacker->Combat->IsReactingToHit() &&
            !Turns->IsNpcReplyValid(FightingReply) && !Pair.Attacker->Presentation->GetSpeech().bActive;
        if (Pair.Attacker->Combat->IsReactingToHit() && State.ActionId==Pair.ActionId &&
            State.Status==EDeliveryNpcActionStatus::Running)
        {
            bKeptFightAfterHit=true;
            if (PunchesWhenHit==INDEX_NONE) PunchesWhenHit=Pair.Attacker->Combat->GetFightPunchCount();
        }
        if (PunchesWhenHit!=INDEX_NONE && Pair.Attacker->Combat->GetFightPunchCount()>PunchesWhenHit)
            bPunchedAgainAfterHit=true;
    }
    TestTrue(TEXT("双方各自挥出了拳"),Pair.Attacker->Combat->GetFightPunchCount()>0 && Reply.Attacker->Combat->GetFightPunchCount()>0);
    TestTrue(TEXT("后仰结束后双方仍能实际命中"),Pair.Attacker->Combat->GetFightHitCount()>0 && Reply.Attacker->Combat->GetFightHitCount()>0);
    TestTrue(TEXT("还击确实打断了正在播的台词"),bSpeechInterruptedByHit);
    TestTrue(TEXT("被还击时保留模型选择的交战和同一个动作编号"),bKeptFightAfterHit);
    TestTrue(TEXT("后仰结束后继续出拳，中间没有再请求一次模型"),bPunchedAgainAfterHit);
    TestNotEqual(TEXT("轻拳不会再用重新判断为由结束整段交战"),Pair.Attacker->Agent->GetPublicState().Reason,FName(TEXT("ReconsiderAfterHit")));
    TestFalse(TEXT("交战真正结束后不会留下尚未命中的拳头"),Pair.Attacker->Combat->IsStriking());
    TestTrue(TEXT("五秒样本中的双方都保留收拳间隔"),Pair.Attacker->Combat->GetFightPunchCount()<=3 && Reply.Attacker->Combat->GetFightPunchCount()<=3);
    TestTrue(TEXT("双方在各自的时限后结束"),DeliveryNpc::IsTerminal(Pair.Attacker->Agent->GetPublicState().Status) &&
        DeliveryNpc::IsTerminal(Reply.Attacker->Agent->GetPublicState().Status));
    Reply.Task->OnTaskFinished(*Reply.Tree,nullptr,EBTNodeResult::Succeeded);
    Reply.Tree->DestroyComponent(); Reply.Task.Reset(); Reply.Tree.Reset();
    Close(Pair);

    auto Runner=SpawnCase(); Tick(Runner,.2f);
    auto* RunnerAgent=Runner.Target->Agent.Get();
    // 这里只检查逃跑中的受击；路径选择由已有导航检查负责，真实拳头仍须接触身体
    const FGuid FleeId=FGuid::NewGuid();
    RunnerAgent->PublicState.ActionId=FleeId;
    RunnerAgent->PublicState.Action=EDeliveryNpcAction::Flee;
    RunnerAgent->PublicState.Origin=EDeliveryNpcActionOrigin::Model;
    RunnerAgent->PublicState.Status=EDeliveryNpcActionStatus::Running;
    RunnerAgent->PublicState.Target=Runner.Attacker;
    RunnerAgent->ActionDeadline=World->GetTimeSeconds()+6;
    const FGuid PendingFleeRequest=FGuid::NewGuid();
    RunnerAgent->ActiveRequest.RequestId=PendingFleeRequest;
    RunnerAgent->bDecisionPending=true;
    if (!TestTrue(TEXT("准备攻击正在逃跑的居民"),Begin(Runner,EDeliveryNpcAction::Fight))) return false;
    Tick(Runner,.44f);
    TestTrue(TEXT("逃跑中的居民确实受到这一拳"),Runner.Target->Combat->IsReactingToHit());
    TestEqual(TEXT("普通轻拳保留逃跑编号"),RunnerAgent->GetPublicState().ActionId,FleeId);
    TestEqual(TEXT("普通轻拳保留逃跑状态"),RunnerAgent->GetPublicState().Status,EDeliveryNpcActionStatus::Running);
    TestTrue(TEXT("轻拳不会取消交战期间已经发出的判断请求"),RunnerAgent->IsDecisionPending());
    Runner.Target->ReceiveImpactFrom(FVector(100,0,0),Runner.Target->GetActorLocation(),Runner.Attacker);
    TestTrue(TEXT("真正倒地后仍立即终止逃跑"),DeliveryNpc::IsTerminal(RunnerAgent->GetPublicState().Status));
    TestFalse(TEXT("真正倒地后取消旧判断"),RunnerAgent->IsDecisionPending());
    FDeliveryNpcDecisionProposal LateReply;
    LateReply.RequestId=PendingFleeRequest;
    LateReply.Action=EDeliveryNpcAction::Fight;
    LateReply.Duration=5;
    RunnerAgent->ReceiveDecision(PendingFleeRequest,&LateReply,NAME_None);
    TestEqual(TEXT("倒地后迟到的旧回复不能重新开始交战"),RunnerAgent->GetPublicState().ActionId,FleeId);
    TestEqual(TEXT("迟到回复仍保留已取消状态"),RunnerAgent->GetPublicState().Status,EDeliveryNpcActionStatus::Cancelled);
    Close(Runner);

    auto Continuous=SpawnCase(); Tick(Continuous,.2f);
    // 长时间检查续接时不把目标提前击倒，命中仍经过实际拳头接触
    Continuous.Attacker->Combat->bHarmlessStrikes=true;
    if (!TestTrue(TEXT("模型可以选择八秒交战窗口"),Begin(Continuous,EDeliveryNpcAction::Fight,DeliveryNpc::FightDecisionSeconds))) return false;
    Tick(Continuous,.08f);
    auto* ContinuingAgent=Continuous.Attacker->Agent.Get();
    auto* ContinuingPresentation=Continuous.Attacker->Presentation.Get();
    const FGuid ContinuousAction=Continuous.ActionId;
    const FGuid OngoingPunch=Continuous.Attacker->Combat->GetCurrentPunchId();
    const double OngoingPunchTime=Continuous.Attacker->Combat->GetStrikeStartTime(ContinuousAction);
    const int32 ExistingPunches=Continuous.Attacker->Combat->GetFightPunchCount();
    const double ContinuousLimit=ContinuingAgent->CombatDeadline;
    ContinuingPresentation->BeginReply(ContinuousAction,{},0);
    FDeliveryNpcSentence OngoingLine; OngoingLine.Text=TEXT("你少来这一套，我还没说完呢！");
    ContinuingPresentation->QueueSentence(ContinuousAction,{},OngoingLine);
    ContinuingPresentation->FinishSentences(ContinuousAction,1);
    ContinuingPresentation->TickSentences();
    const FGuid OngoingTurn=ContinuingPresentation->Speech.TurnId;
    FDeliveryNpcDecisionRequest UpdateRequest;
    FDeliveryNpcDecisionProposal Update;
    if (!TestTrue(TEXT("挥拳期间可以准备下一次模型请求"),PrepareUpdate(Continuous,EDeliveryNpcAction::Fight,8,UpdateRequest,Update))) return false;
    TestEqual(TEXT("请求记住正在执行的行动"),UpdateRequest.RunningActionId,ContinuousAction);
    TestFalse(TEXT("交战中的后台请求不把看一眼当成停手"),UpdateRequest.AllowedActions.Contains(EDeliveryNpcAction::LookAt));
    TestTrue(TEXT("后台请求仍能明确选停手"),UpdateRequest.Candidates.ContainsByPredicate([](const auto& Item)
        { return Item.Action==EDeliveryNpcAction::Wait; }));
    TestTrue(TEXT("模型收到当前行动和实际出拳情况"),UpdateRequest.Evidence.ContainsByPredicate([](const auto& Item)
        { return Item.Kind==TEXT("SelfCombat") && Item.Description.Contains(TEXT("正在")) && Item.Description.Contains(TEXT("实际命中")); }));
    // 战斗小模型只更新动作，适配器的默认 Calm 不能抹掉正在播放的生气表情
    Update.Emotion=TEXT("Calm");
    TestTrue(TEXT("同目标的新决定在出拳中通过检查"),ContinuingAgent->ReceiveStreamDecision(Update.RequestId,Update));
    TestEqual(TEXT("无台词的同目标续战保留原模型的生气表情"),ContinuingAgent->GetPublicState().Emotion,FName(TEXT("Angry")));
    const FGuid FirstReply=ContinuingAgent->GetPublicState().GetReplyId();
    TestEqual(TEXT("续接保留交战编号"),ContinuingAgent->GetPublicState().ActionId,ContinuousAction);
    TestEqual(TEXT("续接保留当前拳编号"),Continuous.Attacker->Combat->GetCurrentPunchId(),OngoingPunch);
    TestEqual(TEXT("续接保留当前拳开始时间"),Continuous.Attacker->Combat->GetStrikeStartTime(ContinuousAction),OngoingPunchTime);
    TestEqual(TEXT("续接不会清零累计挥拳"),Continuous.Attacker->Combat->GetFightPunchCount(),ExistingPunches);
    TestEqual(TEXT("续接不会推迟整场交战上限"),ContinuingAgent->CombatDeadline,ContinuousLimit);
    TestNotEqual(TEXT("同一行动的新回复有独立编号"),FirstReply,ContinuousAction);
    TestFalse(TEXT("新回复未被上一句的取消通知作废"),ContinuingPresentation->CancelledVoiceActions.Contains(FirstReply));
    TestEqual(TEXT("没有新台词的续战保留原来这句声音"),ContinuingPresentation->Speech.TurnId,OngoingTurn);
    TestTrue(TEXT("原句仍在播放"),ContinuingPresentation->Speech.bActive);
    ContinuingAgent->FinishReply(Update.RequestId,true,0);
    const FGuid FirstRequest=Update.RequestId;
    if (!TestTrue(TEXT("上一轮传输完成后可继续询问，行动仍未停止"),PrepareUpdate(Continuous,EDeliveryNpcAction::Fight,8,UpdateRequest,Update))) return false;
    TestTrue(TEXT("下一次回复可再次续接"),ContinuingAgent->ReceiveStreamDecision(Update.RequestId,Update));
    const FGuid SecondReply=ContinuingAgent->GetPublicState().GetReplyId();
    ContinuingPresentation->DisplayedSpeech.ActionId=SecondReply;
    ContinuingPresentation->DisplayedSpeech.bActive=true;
    ContinuingPresentation->StopStreamSpeech_Implementation(FirstReply,false);
    TestTrue(TEXT("旧回复的停止通知不会关闭新字幕"),ContinuingPresentation->DisplayedSpeech.bActive);
    TestFalse(TEXT("旧回复的停止通知不会取消新语音"),ContinuingPresentation->CancelledVoiceActions.Contains(SecondReply));
    ContinuingAgent->FinishReply(FirstRequest,false);
    TestTrue(TEXT("旧传输失败不会结束新请求"),ContinuingAgent->IsDecisionPending());
    TestEqual(TEXT("无台词续战和旧失败都不替换正在播放的回复"),ContinuingPresentation->ReplyAction,ContinuousAction);
    FDeliveryNpcSpeechBegin OldAudio; OldAudio.Text=TEXT("迟到的旧句子");
    TestFalse(TEXT("旧请求的音频不能加入新回复"),ContinuingAgent->ReceiveSpeechBegin(FirstRequest,OldAudio));
    ContinuingPresentation->DisplayedSpeech={};
    ContinuingAgent->FinishReply(Update.RequestId,true,0);
    if (!TestTrue(TEXT("同目标续战仍可接收带台词的新决定"),PrepareUpdate(Continuous,EDeliveryNpcAction::Fight,8,UpdateRequest,Update))) return false;
    Update.Speech=TEXT("哎，你先别靠过来！"); Update.Emotion=TEXT("Surprised");
    ContinuingAgent->ReceiveDecision(Update.RequestId,&Update,NAME_None);
    TestEqual(TEXT("有新台词时使用本次模型选择的表情"),ContinuingAgent->GetPublicState().Emotion,FName(TEXT("Surprised")));
    double NextRenewal=World->GetTimeSeconds()+4;
    int32 Renewals=0;
    for (int32 Step=0;Step<1100 && ContinuingAgent->HasActiveAction();++Step)
    {
        Tick(Continuous,.02f);
        if (World->GetTimeSeconds()>=NextRenewal && ContinuingAgent->CanUpdateRunningAction())
        {
            if (!TestTrue(TEXT("交战中能够继续提供有效候选"),PrepareUpdate(Continuous,EDeliveryNpcAction::Fight,8,UpdateRequest,Update))) return false;
            ContinuingAgent->ReceiveDecision(Update.RequestId,&Update,NAME_None);
            ++Renewals; NextRenewal=World->GetTimeSeconds()+4;
        }
    }
    TestTrue(TEXT("至少经过两次后台决定仍在连续执行"),Renewals>=2);
    TestTrue(TEXT("同一段交战能越过最初八秒，累计出拳没有清零"),Continuous.Attacker->Combat->GetFightPunchCount()>3);
    TestTrue(TEXT("即使模型一直续战也在二十秒内结束"),Continuous.FinishedAt>Continuous.StartedAt+8 && Continuous.FinishedAt<=ContinuousLimit+.03);
    TestEqual(TEXT("整场上限结束不会重建行动编号"),ContinuingAgent->GetPublicState().ActionId,ContinuousAction);
    TestFalse(TEXT("结束后没有残留挥拳"),Continuous.Attacker->Combat->IsStriking());
    ContinuingAgent->ReceiveDecision(Update.RequestId,&Update,NAME_None);
    TestTrue(TEXT("结束后重放旧决定不会恢复交战"),DeliveryNpc::IsTerminal(ContinuingAgent->GetPublicState().Status));
    Close(Continuous);

    auto Recheck=SpawnCase(); Tick(Recheck,.2f);
    Recheck.Attacker->Combat->bHarmlessStrikes=true;
    if (!TestTrue(TEXT("开始一段可后台复判的交战"),Begin(Recheck,EDeliveryNpcAction::Fight,8))) return false;
    auto* RecheckAgent=Recheck.Attacker->Agent.Get();
    Recheck.Attacker->ObserveFact(TEXT("FleeStarted"),Recheck.Attacker,TEXT("无关角色远离"),EDeliveryNpcEvidenceSource::Seen);
    TestTrue(TEXT("无关对象的消息不会提前询问"),RecheckAgent->CombatEventDecisionAt<0);
    Tick(Recheck,.1f);
    Recheck.Attacker->ObserveFact(TEXT("LightMeleeHit"),Recheck.Target,TEXT("我挨了一拳"),EDeliveryNpcEvidenceSource::Seen);
    const double RecheckAt=RecheckAgent->CombatEventDecisionAt;
    TestTrue(TEXT("挨打后安排提前复判"),RecheckAt>World->GetTimeSeconds() && RecheckAt<RecheckAgent->NextActionDecisionAt);
    Recheck.Attacker->ObserveFact(TEXT("FleeStarted"),Recheck.Target,TEXT("对手开始跑开"),EDeliveryNpcEvidenceSource::Seen);
    TestEqual(TEXT("连续战况变化合并到同一次询问"),RecheckAgent->CombatEventDecisionAt,RecheckAt);
    TestEqual(TEXT("安排复判时保留当前交战"),RecheckAgent->GetPublicState().ActionId,Recheck.ActionId);
    Tick(Recheck,7.6f);
    TestEqual(TEXT("最后零点几秒仍接受后台决定"),RecheckAgent->GetPublicState().Status,EDeliveryNpcActionStatus::Running);
    Tick(Recheck,.4f);
    TestEqual(TEXT("已授权的时间用完才停止"),RecheckAgent->GetPublicState().Reason,FName(TEXT("FightTimeLimit")));
    Close(Recheck);

    auto StopNow=SpawnCase(); Tick(StopNow,.2f);
    if (!TestTrue(TEXT("准备在出拳途中接受停手决定"),Begin(StopNow,EDeliveryNpcAction::Fight,8))) return false;
    Tick(StopNow,.06f);
    auto* StoppingAgent=StopNow.Attacker->Agent.Get();
    auto* StoppingPresentation=StopNow.Attacker->Presentation.Get();
    if (!TestTrue(TEXT("停手前先收到一条续战台词"),PrepareUpdate(StopNow,EDeliveryNpcAction::Fight,8,UpdateRequest,Update))) return false;
    Update.Speech=TEXT("你给我站住！");
    Update.bHasSpeechTarget=true; Update.SpeechTargetId=Update.TargetId;
    const FGuid OldFightRequest=Update.RequestId;
    if (!TestTrue(TEXT("接受带台词的续战"),StoppingAgent->ReceiveStreamDecision(OldFightRequest,Update))) return false;
    const FGuid OldFightReply=StoppingAgent->GetPublicState().GetReplyId();
    FDeliveryNpcSentence FightLine; FightLine.Text=Update.Speech;
    StoppingAgent->ReceiveSentence(OldFightRequest,FightLine);
    if (!TestTrue(TEXT("续战决定已完成，旧语音仍可继续传输"),
        StoppingAgent->FinishModelReply(OldFightRequest,1,TOptional<FString>(FightLine.Text)))) return false;
    StoppingPresentation->TickSentences();
    TestTrue(TEXT("停手前确实在显示追打台词"),StoppingPresentation->Speech.bActive);
    TestTrue(TEXT("停手前旧请求仍可接收后续语音"),StoppingAgent->CanReceiveReply(OldFightRequest));
    TestTrue(TEXT("语音还在播放时对方已经收到台词"),Turns->WasNpcMessageDelivered(OldFightReply,StopNow.Target));
    if (!TestTrue(TEXT("出拳时模型也能选择Wait停手"),PrepareUpdate(StopNow,EDeliveryNpcAction::Wait,1,UpdateRequest,Update))) return false;
    Update.Emotion=TEXT("Calm");
    const FGuid StoppedAction=StopNow.ActionId;
    if (!TestTrue(TEXT("接受无台词的停手决定"),StoppingAgent->ReceiveStreamDecision(Update.RequestId,Update))) return false;
    const FGuid WaitAction=StoppingAgent->GetPublicState().ActionId;
    TestNotEqual(TEXT("停手作为新行动替换追打"),WaitAction,StoppedAction);
    TestFalse(TEXT("停手后停止显示上一轮追打台词"),StoppingPresentation->Speech.bActive);
    TestTrue(TEXT("停手后向客户端取消上一轮追打声音"),StoppingPresentation->CancelledVoiceActions.Contains(OldFightReply));
    TestFalse(TEXT("迟到的追打音频不能恢复播放"),StoppingAgent->CanReceiveReply(OldFightRequest));
    TestTrue(TEXT("停手只取消声音，对方仍可引用收到的台词"),Turns->GetNpcMessageHandle(OldFightReply,StopNow.Target).IsValid());
    TestTrue(TEXT("取消旧台词时保留本次停手请求"),StoppingAgent->CanReceiveReply(Update.RequestId));
    TestEqual(TEXT("换动作即使没有台词也使用本次模型的表情"),StopNow.Attacker->Agent->GetPublicState().Emotion,FName(TEXT("Calm")));
    TestFalse(TEXT("停手决定立即取消未命中的拳"),StopNow.Attacker->Combat->IsStriking());
    StoppingAgent->FinishReply(Update.RequestId,true,0);
    Tick(StopNow,.6f);
    TestEqual(TEXT("旧拳不会在停手后补算命中"),StopNow.Attacker->Combat->GetFightHitCount(),0);
    TestFalse(TEXT("停手后的目标没有凭空受击"),StopNow.Target->Combat->IsReactingToHit());
    StopNow.Attacker->Agent->ReceiveDecision(Update.RequestId,&Update,NAME_None);
    TestEqual(TEXT("旧交战决定重放不会替换停手行动"),StopNow.Attacker->Agent->GetPublicState().ActionId,WaitAction);
    Close(StopNow);

    auto Cancelled=SpawnCase(); Tick(Cancelled,.2f);
    if (!TestTrue(TEXT("准备取消蓄力中的追打"),Begin(Cancelled,EDeliveryNpcAction::Fight))) return false;
    Tick(Cancelled,.1f);
    TestTrue(TEXT("取消前确实已开始挥拳"),Cancelled.Attacker->Combat->IsStriking());
    Cancelled.Attacker->Agent->CancelPlan(TEXT("PlayerInterrupted")); Tick(Cancelled,.6f);
    TestEqual(TEXT("取消不会稍后补一次命中"),Cancelled.Attacker->Combat->GetFightHitCount(),0);
    TestFalse(TEXT("被取消的目标没有后仰"),Cancelled.Target->Combat->IsReactingToHit());
    TestEqual(TEXT("取消后清除客户端出拳时间"),Cancelled.Attacker->Combat->GetStrikeStartTime(Cancelled.ActionId),-1.0);
    Close(Cancelled);

    auto Escaped=SpawnCase(); Tick(Escaped,.2f);
    if (!TestTrue(TEXT("准备目标逃离的追打"),Begin(Escaped,EDeliveryNpcAction::Fight))) return false;
    Tick(Escaped,.1f); Escaped.Target->SetActorLocation(FVector(800,0,90)); Tick(Escaped,.6f);
    TestEqual(TEXT("目标跑远后结束追赶"),Escaped.Attacker->Agent->GetPublicState().Reason,FName(TEXT("TargetEscaped")));
    TestEqual(TEXT("逃离后不发生延迟命中"),Escaped.Attacker->Combat->GetFightHitCount(),0);
    Close(Escaped);

    auto Missing=SpawnCase(); Tick(Missing,.2f);
    if (!TestTrue(TEXT("准备目标销毁的追打"),Begin(Missing,EDeliveryNpcAction::Fight))) return false;
    Tick(Missing,.1f); Missing.Target->Destroy(); Missing.Target=nullptr; Tick(Missing,.6f);
    TestEqual(TEXT("目标销毁后结束动作"),Missing.Attacker->Agent->GetPublicState().Reason,FName(TEXT("TargetLost")));
    TestFalse(TEXT("目标销毁后没有残留挥拳"),Missing.Attacker->Combat->IsStriking());
    Close(Missing);

    auto Blocked=SpawnCase(); Tick(Blocked,.2f);
    Blocked.Target->SetActorLocation(FVector(400,0,90));
    Blocked.Attacker->ObserveFact(TEXT("SeenNpc"),Blocked.Target,TEXT("对方在没有可用路径的远处"),EDeliveryNpcEvidenceSource::Seen);
    FDeliveryNpcDecisionRequest NoPath;
    TestTrue(TEXT("没有路径时仍能生成其他候选"),Blocked.Attacker->Agent->BuildDecisionRequest(FGuid::NewGuid(),NoPath));
    TestFalse(TEXT("没有导航路径时不向模型提供Fight"),NoPath.Candidates.ContainsByPredicate([](const auto& Item)
        { return Item.Action==EDeliveryNpcAction::Fight; }));
    Blocked.Target->SetActorLocation(FVector(Blocked.Attacker->Combat->GetStrikeApproachDistance(Blocked.Target)-.5f,0,90));
    if (!TestTrue(TEXT("近身时可以开始，无需路径"),Begin(Blocked,EDeliveryNpcAction::Fight))) return false;
    Blocked.Target->SetActorLocation(FVector(400,0,90)); Tick(Blocked,.1f);
    TestEqual(TEXT("执行时路不可用会结束动作"),Blocked.Attacker->Agent->GetPublicState().Reason,FName(TEXT("PathUnavailable")));
    TestEqual(TEXT("路不可用时没有开始挥拳"),Blocked.Attacker->Combat->GetFightPunchCount(),0);
    Close(Blocked);

    auto Limited=SpawnCase(); Tick(Limited,.2f);
    if (!TestTrue(TEXT("可以执行较短的一轮交战"),Begin(Limited,EDeliveryNpcAction::Fight,1.2f))) return false;
    Tick(Limited,1.5f);
    TestEqual(TEXT("时间用完时停止，不再开始第二拳"),Limited.Attacker->Agent->GetPublicState().Reason,FName(TEXT("FightTimeLimit")));
    TestEqual(TEXT("短轮次只挥出一拳"),Limited.Attacker->Combat->GetFightPunchCount(),1);
    TestTrue(TEXT("结束时间遵守本轮时长"),Limited.FinishedAt>=0 && Limited.FinishedAt-Limited.StartedAt<=1.24);
    Close(Limited);

    auto Heavy=SpawnCase(); Tick(Heavy,.2f);
    if (!TestTrue(TEXT("原StrikeOnce仍能通过相同校验执行"),Begin(Heavy,EDeliveryNpcAction::StrikeOnce))) return false;
    Tick(Heavy,.44f);
    TestTrue(TEXT("原StrikeOnce仍保留一次重击倒地的规则"),Heavy.Target->IsKnockedDown());
    Close(Heavy);

    auto* PlayerClass=LoadClass<ADeliveryCharacter>(nullptr,
        TEXT("/Game/Blueprint/Character/DeliverMan/BP_DeliveryMan.BP_DeliveryMan_C"));
    if (!TestNotNull(TEXT("装有布娃娃的正式玩家蓝图"),PlayerClass)) return false;
    const auto AttachLocalCamera=[&](APlayerController* Controller,APawn* Pawn)
    {
        Controller->SetPlayer(NewObject<ULocalPlayer>(GEngine));
        if (!Controller->PlayerCameraManager) Controller->SpawnPlayerCameraManager();
        if (!Controller->PlayerCameraManager) return false;
        Controller->Possess(Pawn);
        Controller->SetViewTarget(Pawn);
        return Pawn->IsLocallyControlled() && Controller->GetViewTarget()==Pawn;
    };
    const auto HasHitShake=[](const APlayerController* Controller)
    {
        auto* Manager=Controller->PlayerCameraManager.Get();
        auto* Modifier=Manager?Cast<UCameraModifier_CameraShake>(Manager->FindCameraModifierByClass(UCameraModifier_CameraShake::StaticClass())):nullptr;
        TArray<FActiveCameraShakeInfo> Shakes;
        if (Modifier) Modifier->GetActiveCameraShakes(Shakes);
        return Shakes.ContainsByPredicate([](const FActiveCameraShakeInfo& Shake)
            { return Shake.ShakeInstance && Shake.ShakeInstance->IsA<UDeliveryDoorHitCameraShake>(); });
    };
    auto* ObserverController=World->SpawnActor<APlayerController>();
    auto* Observer=World->SpawnActor<APawn>();
    if (!TestTrue(TEXT("旁观者有独立的本机镜头"),ObserverController && Observer && AttachLocalCamera(ObserverController,Observer))) return false;
    ON_SCOPE_EXIT { ObserverController->UnPossess(); ObserverController->Destroy(); Observer->Destroy(); };

    enum class EPlayerStrike { Harmful, Harmless, Blocked };
    const auto CheckPlayerStrike=[&](EPlayerStrike Kind)
    {
        // 三个近身间距用于覆盖正式玩家和居民的骨架差异
        for (const double Distance:{72.,62.,52.})
        {
            auto Contact=SpawnCase();
            Contact.Target->Destroy(); Contact.Target=nullptr;
            ON_SCOPE_EXIT { Close(Contact); };
            FActorSpawnParameters Spawn;
            Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            auto* Player=World->SpawnActor<ADeliveryCharacter>(PlayerClass,FVector(Distance,0,90),FRotator(0,180,0),Spawn);
            auto* State=World->SpawnActor<ADeliverPlayerState>();
            auto* Controller=World->SpawnActor<APlayerController>();
            if (!TestTrue(TEXT("创建带技能系统的玩家目标"),Player && State && Controller)) return false;
            ON_SCOPE_EXIT { Controller->UnPossess(); Player->Destroy(); State->Destroy(); Controller->Destroy(); };
            State->SetOwner(Controller);
            Controller->PlayerState=State;
            if (!TestTrue(TEXT("受击者有实际使用的本机镜头"),AttachLocalCamera(Controller,Player))) return false;
            if (!TestTrue(TEXT("服务器能从被控制的玩家找到同一 PlayerState"),Player->GetPlayerState()==State)) return false;
            auto* ASC=State->GetDeliverAbilitySystemComponent();
            ASC->InitializeAbilityActor(State,Player);
            const FGameplayAttribute Health=UDeliverAttributeSet::GetHealthAttribute();
            ASC->SetNumericAttributeBase(Health,100.f);
            if (!TestTrue(TEXT("玩家处于原主动布娃娃状态"),Player->GetActiveRagdoll()->IsRagdollActive())) return false;
            float LowestHealth=100.f;
            const FDelegateHandle HealthChanged=ASC->GetGameplayAttributeValueChangeDelegate(Health).AddLambda(
                [&LowestHealth](const FOnAttributeChangeData& Change) { LowestHealth=FMath::Min(LowestHealth,Change.NewValue); });
            ON_SCOPE_EXIT { ASC->GetGameplayAttributeValueChangeDelegate(Health).Remove(HealthChanged); };
            Contact.Attacker->Combat->bHarmlessStrikes=Kind==EPlayerStrike::Harmless;
            if (!Begin(Contact,EDeliveryNpcAction::StrikeOnce,5.f,nullptr,Player)) continue;
            AActor* Blocker=nullptr;
            ON_SCOPE_EXIT { if (Blocker) Blocker->Destroy(); };
            if (Kind==EPlayerStrike::Blocked)
            {
                // 已经起拳才加遮挡，让命中窗口得出一次真实未命中
                Blocker=World->SpawnActor<AActor>();
                auto* Shield=NewObject<UBoxComponent>(Blocker);
                Blocker->SetRootComponent(Shield); Shield->SetBoxExtent(FVector(2,100,100));
                Shield->SetCollisionProfileName(TEXT("BlockAll")); Shield->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
                Shield->RegisterComponent(); Blocker->SetActorLocation((Player->GetActorLocation()+Contact.Attacker->GetActorLocation())*.5f);
            }
            bool bPlayerSawShake=false;
            bool bObserverSawShake=false;
            for (int32 Step=0;Step<23;++Step)
            {
                Tick(Contact,.02f);
                bPlayerSawShake|=HasHitShake(Controller);
                bObserverSawShake|=HasHitShake(ObserverController);
            }
            bool bContact=false;
            if (!TestTrue(TEXT("正式 BT 拳头与动画共用动作编号"),Contact.bParentTimeMatches && Contact.PunchIds.Contains(Contact.ActionId))) return false;
            if (!TestTrue(TEXT("真实近身挥拳产生了判定结果"),Contact.Attacker->Combat->GetStrikeResult(Contact.ActionId,bContact))) return false;
            const bool bHarmfulContact=bContact && Kind==EPlayerStrike::Harmful;
            TestEqual(TEXT("只有有效近战命中才让受击者的镜头抖动"),bPlayerSawShake,bHarmfulContact);
            TestFalse(TEXT("旁观者镜头不抖动"),bObserverSawShake);
            TestTrue(TEXT("只有有效近战命中才通过原 GAS 扣血"),FMath::IsNearlyEqual(LowestHealth,
                bHarmfulContact?100.f-Contact.Attacker->Combat->OutgoingPlayerDamage:100.f));
            if (Kind==EPlayerStrike::Blocked) return !bContact;
            if (!bContact) continue;
            if (bHarmfulContact)
            {
                // 主动结束镜头效果，继续让同一拳的后续骨骼帧运行，以检查重复触发
                Controller->PlayerCameraManager->StopAllInstancesOfCameraShake(UDeliveryDoorHitCameraShake::StaticClass(),true);
                for (int32 Step=0;Step<15;++Step)
                {
                    Tick(Contact,.02f);
                    TestFalse(TEXT("旧拳后续帧不会重新抖动镜头"),HasHitShake(Controller));
                }
            }
            return true;
        }
        return false;
    };
    TestTrue(TEXT("近身拳头实际扫到正式主角的布娃娃网格"),CheckPlayerStrike(EPlayerStrike::Harmful));
    TestTrue(TEXT("无伤拳实际接触玩家但不抖镜头"),CheckPlayerStrike(EPlayerStrike::Harmless));
    TestTrue(TEXT("遮挡时挥空并保持镜头安静"),CheckPlayerStrike(EPlayerStrike::Blocked));
    TestFalse(TEXT("此离线世界关闭声音，未验证实际音频播放"),World->AllowAudioPlayback());
    TestFalse(TEXT("整个测试没有启用AI后台"),Settings->bEnabled);
    return true;
}
#endif
