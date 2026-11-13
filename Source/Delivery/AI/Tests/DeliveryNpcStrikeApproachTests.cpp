#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/BehaviorTree/BTTask_DeliveryExecuteNpcAction.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcStrikeApproachTest,"Delivery.AI.StrikeApproach",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryNpcStrikeApproachTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool bOldEnabled=Settings->bEnabled; Settings->bEnabled=false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
        Settings->bEnabled=bOldEnabled;
    };
    auto* Floor=World->SpawnActor<AActor>(); auto* FloorBox=NewObject<UBoxComponent>(Floor);
    Floor->SetRootComponent(FloorBox); FloorBox->SetBoxExtent(FVector(2000,2000,20));
    FloorBox->SetCollisionProfileName(TEXT("BlockAll")); FloorBox->RegisterComponent();
    Floor->SetActorLocation(FVector(0,0,-20));
    FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* ManClass=LoadClass<ADeliveryNpcCharacter>(nullptr,TEXT("/Game/NPC/Residents/BP_ResidentLoud.BP_ResidentLoud_C"));
    auto* WomanClass=LoadClass<ADeliveryNpcCharacter>(nullptr,TEXT("/Game/NPC/Residents/BP_ResidentShrewd.BP_ResidentShrewd_C"));
    if (!TestNotNull(TEXT("正式男NPC蓝图"),ManClass) || !TestNotNull(TEXT("正式女NPC蓝图"),WomanClass)) return false;
    auto* A=World->SpawnActor<ADeliveryNpcCharacter>(ManClass,FVector(0,0,90),FRotator::ZeroRotator,Spawn);
    auto* B=World->SpawnActor<ADeliveryNpcCharacter>(WomanClass,FVector(208,0,90),FRotator(0,180,0),Spawn);
    A->SetActorTickEnabled(false); B->SetActorTickEnabled(false);
    for (auto* Npc:{A,B})
        if (auto* AI=Cast<AAIController>(Npc->GetController()); AI && AI->BrainComponent)
            AI->BrainComponent->StopLogic(TEXT("IsolatedContactTest"));
    const auto Tick=[&](float Seconds)
    {
        for (int32 Index=0;Index<FMath::CeilToInt(Seconds/.02f);++Index)
        { ++GFrameCounter; World->Tick(LEVELTICK_All,.02f); }
    };
    const auto Place=[&](float Distance)
    {
        A->SetActorLocation(FVector(0,0,90)); B->SetActorLocation(FVector(Distance,0,90));
        A->GetCharacterMovement()->StopMovementImmediately(); B->GetCharacterMovement()->StopMovementImmediately();
    };
    const auto Approve=[&]()
    {
        A->Agent->CancelPlan(TEXT("NextStrikeTest"));
        A->ObserveFact(TEXT("SeenNpc"),B,TEXT("对方站在面前"),EDeliveryNpcEvidenceSource::Seen);
        Settings->bEnabled=true; const bool bQueued=A->Agent->RequestDecision(); Settings->bEnabled=false;
        if (!bQueued) return false;
        const auto Request=A->Agent->ActiveRequest;
        World->GetSubsystem<UDeliveryAiGatewaySubsystem>()->Cancel(A->Agent);
        const auto* Candidate=Request.Candidates.FindByPredicate([](const auto& Item)
            { return Item.Action==EDeliveryNpcAction::StrikeOnce; });
        if (!Candidate) return false;
        FDeliveryNpcDecisionProposal Proposal;
        Proposal.WorldEpoch=Request.WorldEpoch; Proposal.NpcId=Request.NpcId; Proposal.RequestId=Request.RequestId;
        Proposal.ContextRevision=Request.ContextRevision; Proposal.PlanRevision=Request.PlanRevision;
        Proposal.Action=Candidate->Action; Proposal.TargetId=Candidate->TargetId; Proposal.EvidenceIds=Candidate->EvidenceIds;
        Proposal.Duration=5;
        A->Agent->ReceiveDecision(Proposal.RequestId,&Proposal,NAME_None);
        return A->Agent->GetPublicState().Status==EDeliveryNpcActionStatus::Accepted;
    };
    const auto BeginStrike=[&](FGuid& OutId)
    {
        if (!Approve()) return false;
        FDeliveryNpcPublicState Action;
        if (!A->Agent->StartAction(Action)) return false;
        OutId=Action.ActionId;
        return A->ExecuteAction(Action);
    };
    const auto Stop=[&](FGuid Id)
    {
        A->StopAction(Id); A->Agent->CancelPlan(TEXT("StrikeTestStopped"));
    };

    TestFalse(TEXT("旧版停步距离208厘米仍不允许隔空命中"),A->Combat->CanStrike(B));
    TestFalse(TEXT("没有导航时不提供远距离挥拳"),A->CanExecute(EDeliveryNpcAction::StrikeOnce,B));
    Place(400);
    TestFalse(TEXT("没有导航时不提供走向目标"),A->CanExecute(EDeliveryNpcAction::MoveTo,B));
    A->ObserveFact(TEXT("SeenNpc"),B,TEXT("对方站在面前"),EDeliveryNpcEvidenceSource::Seen);
    FDeliveryNpcDecisionRequest NoNavRequest;
    if (!TestTrue(TEXT("仍可生成交谈请求"),A->Agent->BuildDecisionRequest(FGuid::NewGuid(),NoNavRequest))) return false;
    TestFalse(TEXT("无路径的攻击和移动不会发给模型选择"),NoNavRequest.Candidates.ContainsByPredicate([](const auto& Item)
        { return Item.Action==EDeliveryNpcAction::StrikeOnce || Item.Action==EDeliveryNpcAction::MoveTo; }));
    Place(A->Combat->GetStrikeApproachDistance(B)-.5f);
    TestTrue(TEXT("已在近战范围内时不要求导航"),A->Combat->CanApproachStrike(B));
    if (!TestTrue(TEXT("没有导航也能选择近身挥拳"),Approve())) return false;
    B->SetActorLocation(FVector(400,0,90));
    auto* AI=Cast<AAIController>(A->GetController());
    if (!TestNotNull(TEXT("攻击由AI控制器执行"),AI)) return false;
    auto* Tree=NewObject<UBehaviorTreeComponent>(AI); Tree->RegisterComponent();
    auto* Task=NewObject<UBTTask_DeliveryExecuteNpcAction>(Tree);
    TStrongObjectPtr<UBehaviorTreeComponent> KeepTree(Tree);
    TStrongObjectPtr<UBTTask_DeliveryExecuteNpcAction> KeepTask(Task);
    TestEqual(TEXT("模型回复后目标退到无路径位置时拒绝启动"),Task->ExecuteTask(*Tree,nullptr),EBTNodeResult::Failed);
    TestFalse(TEXT("目标位置变化后不会提前出拳"),A->Combat->IsStriking());
    Task->OnTaskFinished(*Tree,nullptr,EBTNodeResult::Failed);
    Tick(.5f);
    TestFalse(TEXT("路径失败后目标没有受到攻击"),B->IsKnockedDown());

    Place(400);
    FGuid Attack;
    TestFalse(TEXT("无路径时不能开始接近阶段"),A->Combat->CanApproachStrike(B));
    Place(A->Combat->GetStrikeApproachDistance(B)-.5f); Tick(.5f);
    TestFalse(TEXT("拒绝启动后即使目标靠近也不会补打一拳"),B->IsKnockedDown());

    Place(A->Combat->GetStrikeApproachDistance(B)-.5f);
    if (!TestTrue(TEXT("近身后服务器开始挥拳"),BeginStrike(Attack))) return false;
    TestTrue(TEXT("只有实际挥拳才同步出拳时间"),A->Combat->GetStrikeStartTime(Attack)>=0);
    TestFalse(TEXT("同一拳的重复开始被拒绝"),A->Combat->StartStrike(B,Attack));
    Tick(.5f);
    bool bHit=false;
    TestTrue(TEXT("命中判定结果属于当前动作"),A->Combat->GetStrikeResult(Attack,bHit));
    TestTrue(TEXT("命中结果为成功"),bHit);
    TestTrue(TEXT("目标实际倒地"),B->IsKnockedDown());
    TestFalse(TEXT("倒地目标不再提供攻击候选"),A->CanExecute(EDeliveryNpcAction::StrikeOnce,B));
    Stop(Attack); Tick(6);
    TestFalse(TEXT("恢复后可以继续交互"),B->IsKnockedDown());

    Place(A->Combat->GetStrikeApproachDistance(B)-.5f);
    if (!TestTrue(TEXT("准备会被目标躲开的攻击"),BeginStrike(Attack))) return false;
    B->SetActorLocation(FVector(250,0,90)); Tick(.5f);
    TestTrue(TEXT("未命中也结束本拳的判定"),A->Combat->GetStrikeResult(Attack,bHit));
    TestFalse(TEXT("目标离开距离后记录未命中"),bHit);
    TestFalse(TEXT("未命中不让目标倒地"),B->IsKnockedDown());
    Stop(Attack); Tick(1);

    Place(A->Combat->GetStrikeApproachDistance(B)-.5f);
    if (!TestTrue(TEXT("准备被取消的挥拳"),BeginStrike(Attack))) return false;
    Stop(Attack); Tick(.5f);
    TestFalse(TEXT("取消挥拳不留下稍后发生的命中"),B->IsKnockedDown());
    TestEqual(TEXT("取消时清除客户端出拳时间"),A->Combat->GetStrikeStartTime(Attack),-1.0);
    Tick(1);
    Place(700);
    TestFalse(TEXT("追近距离之外不提供攻击"),A->CanExecute(EDeliveryNpcAction::StrikeOnce,B));
    Place(A->Combat->GetStrikeApproachDistance(B)-.5f);
    auto* Wall=World->SpawnActor<AActor>(); auto* WallBox=NewObject<UBoxComponent>(Wall);
    Wall->SetRootComponent(WallBox); WallBox->SetBoxExtent(FVector(10,100,120));
    WallBox->SetCollisionProfileName(TEXT("BlockAll")); WallBox->RegisterComponent(); Wall->SetActorLocation(FVector(60,0,90));
    TestFalse(TEXT("近身不要求导航，但仍不能隔墙出拳"),A->CanExecute(EDeliveryNpcAction::StrikeOnce,B));
    Wall->Destroy(); B->Destroy();
    TestFalse(TEXT("目标销毁后不能再申请攻击"),A->Combat->CanApproachStrike(B));
    return true;
}
#endif
