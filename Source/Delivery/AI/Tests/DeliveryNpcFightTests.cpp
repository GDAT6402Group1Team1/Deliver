#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/BehaviorTree/BTTask_DeliveryExecuteNpcAction.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AI/Presentation/DeliveryNpcBodyAnimInstance.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
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
        for (auto* Npc:{Case.Attacker,Case.Target})
        {
            Npc->SetActorTickEnabled(false);
            Npc->GetMesh()->VisibilityBasedAnimTickOption=EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
            Npc->GetMesh()->bEnableUpdateRateOptimizations=false;
            // 正式蓝图的树先停下，本检查只让下面这一份任务执行动作
            if (auto* AI=Cast<AAIController>(Npc->GetController()))
                if (auto* Tree=Cast<UBehaviorTreeComponent>(AI->GetBrainComponent()))
                { Tree->StopTree(EBTStopMode::Forced); Tree->SetComponentTickEnabled(false); }
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
    const auto Begin=[&](FCase& Case,EDeliveryNpcAction Kind,float Duration=5.f)
    {
        if (!Case.Tree) return false;
        if (Case.Task) Case.Task->OnTaskFinished(*Case.Tree,nullptr,EBTNodeResult::Succeeded);
        Case.Attacker->Agent->CancelPlan(TEXT("NextFightTest"));
        Case.Attacker->ObserveFact(TEXT("SeenNpc"),Case.Target,TEXT("对方就在面前"),EDeliveryNpcEvidenceSource::Seen);
        FDeliveryNpcDecisionRequest Request;
        if (!Case.Attacker->Agent->BuildDecisionRequest(World->GetSubsystem<UDeliveryAiGatewaySubsystem>()->GetWorldEpoch(),Request)) return false;
        const auto* Candidate=Request.Candidates.FindByPredicate([&](const auto& Item)
        {
            return Item.Action==Kind && Case.Attacker->Agent->KnownSubjects.FindRef(Item.TargetId).Get()==Case.Target;
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
    TestEqual(TEXT("一轮只开始三拳"),Main.Attacker->Combat->GetFightPunchCount(),3);
    TestEqual(TEXT("近身三拳分别发生真实命中"),Main.Attacker->Combat->GetFightHitCount(),3);
    TestEqual(TEXT("每拳使用不同编号"),Main.PunchIds.Num(),3);
    TestFalse(TEXT("整轮编号不充当单拳编号"),Main.PunchIds.Contains(Main.ActionId));
    TestTrue(TEXT("三拳均能按同一个父动作读取播放时间"),Main.bParentTimeMatches);
    TestFalse(TEXT("第三拳后仍然站立"),Main.Target->IsKnockedDown());
    TestTrue(TEXT("连续轻拳不会反复取消已经了解攻击的新请求"),Main.Target->Agent->IsDecisionPending());
    TestTrue(TEXT("五秒内停止且没有等到Agent超时兜底"),Main.FinishedAt>=0 && Main.FinishedAt-Main.StartedAt<=5.02);
    TestEqual(TEXT("按挥拳次数停止"),Main.Attacker->Agent->GetPublicState().Reason,FName(TEXT("FightPunchLimit")));
    FDeliveryNpcDecisionRequest Followup;
    TestTrue(TEXT("下一轮上下文可正常生成"),Main.Attacker->Agent->BuildDecisionRequest(FGuid::NewGuid(),Followup));
    TestTrue(TEXT("结果记录挥拳和命中次数，不声称打赢"),Followup.Evidence.ContainsByPredicate([](const auto& Item)
    {
        return Item.Kind==TEXT("ActionFinished") && Item.Description.Contains(TEXT("挥拳 3 次")) &&
            Item.Description.Contains(TEXT("命中 3 次")) && !Item.Description.Contains(TEXT("打赢"));
    }));
    Tick(Main,3.f);
    if (!TestTrue(TEXT("冷却后可以开始下一轮，累计轻拳仍有效"),Begin(Main,EDeliveryNpcAction::Fight))) return false;
    Tick(Main,.44f);
    TestTrue(TEXT("第四次累计轻拳使居民倒地"),Main.Target->IsKnockedDown());
    TestEqual(TEXT("目标倒地后立即停手"),Main.Attacker->Agent->GetPublicState().Reason,FName(TEXT("TargetDown")));
    TestFalse(TEXT("倒地目标不会留下待命中的拳"),Main.Attacker->Combat->IsStriking());
    Close(Main);

    auto Pair=SpawnCase(); Tick(Pair,.2f);
    if (!TestTrue(TEXT("双方交战先由一人开始"),Begin(Pair,EDeliveryNpcAction::Fight))) return false;
    Tick(Pair,.44f);
    FCase Reply;
    Reply.Attacker=Pair.Target; Reply.Target=Pair.Attacker;
    Reply.Tree.Reset(NewObject<UBehaviorTreeComponent>(CastChecked<AAIController>(Reply.Attacker->GetController())));
    Reply.Tree->RegisterComponent(); Reply.Tree->SetComponentTickEnabled(false);
    // 对方模型独立选择还手，短暂后仰不作废它的决定
    if (!TestTrue(TEXT("受轻拳后仍能接受自己的交战决定"),Begin(Reply,EDeliveryNpcAction::Fight))) return false;
    for (int32 Step=0;Step<270;++Step)
    {
        Tick(Pair,.02f);
        if (Reply.Attacker->Agent->GetPublicState().Status==EDeliveryNpcActionStatus::Running)
            Reply.Task->TickTask(*Reply.Tree,nullptr,.02f);
    }
    TestTrue(TEXT("双方各自挥出了拳"),Pair.Attacker->Combat->GetFightPunchCount()>0 && Reply.Attacker->Combat->GetFightPunchCount()>0);
    TestTrue(TEXT("后仰结束后双方仍能实际命中"),Pair.Attacker->Combat->GetFightHitCount()>0 && Reply.Attacker->Combat->GetFightHitCount()>0);
    TestTrue(TEXT("两人都遵守各自的挥拳次数"),Pair.Attacker->Combat->GetFightPunchCount()<=3 && Reply.Attacker->Combat->GetFightPunchCount()<=3);
    TestTrue(TEXT("双方在各自的时限后结束"),DeliveryNpc::IsTerminal(Pair.Attacker->Agent->GetPublicState().Status) &&
        DeliveryNpc::IsTerminal(Reply.Attacker->Agent->GetPublicState().Status));
    Reply.Task->OnTaskFinished(*Reply.Tree,nullptr,EBTNodeResult::Succeeded);
    Reply.Tree->DestroyComponent(); Reply.Task.Reset(); Reply.Tree.Reset();
    Close(Pair);

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
    Blocked.Target->SetActorLocation(FVector(120,0,90));
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
    TestFalse(TEXT("整个测试没有启用AI后台"),Settings->bEnabled);
    return true;
}
#endif
