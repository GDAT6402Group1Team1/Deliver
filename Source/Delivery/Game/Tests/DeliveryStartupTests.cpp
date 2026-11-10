#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Game/DeliveryStartupFlow.h"
#include "Game/DeliveryGameState.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Phone/DeliveryPhoneCallQueueComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"
#include "Engine/LevelStreamingDynamic.h"
#include "GameFramework/PlayerController.h"

namespace
{
UWorld* MakeStartupTestWorld()
{
    const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
        ERHIFeatureLevel::Num, &Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    return World;
}
void DestroyStartupTestWorld(UWorld* World)
{
    World->EndPlay(EEndPlayReason::Quit);
    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryStartupFlowTest, "Delivery.Startup.Requests",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryStartupFlowTest::RunTest(const FString&)
{
    UWorld* World = MakeStartupTestWorld();
    UWorld* OtherWorld = MakeStartupTestWorld();
    auto* Flow = World->SpawnActor<ADeliveryStartupFlow>();
    auto* OtherFlow = OtherWorld->SpawnActor<ADeliveryStartupFlow>();
    auto* Player = World->SpawnActor<APlayerController>();
    Player->Player = NewObject<ULocalPlayer>(GEngine);
    auto* ForeignPlayer = OtherWorld->SpawnActor<APlayerController>();
    ForeignPlayer->Player = NewObject<ULocalPlayer>(GEngine);
    TestEqual(TEXT("每个世界只取得自己的加载服务"), ADeliveryStartupFlow::GetDeliveryStartupFlow(OtherWorld), OtherFlow);
    TestFalse(TEXT("拒绝其他世界的开始请求"), Flow->RequestStartGame(ForeignPlayer));
    Flow->ReportPlayerReady(ForeignPlayer);
    TestTrue(TEXT("其他世界的玩家不能记入就绪集合"), Flow->ReadyPlayers.IsEmpty());
    TestTrue(TEXT("允许在加载前记住开始请求"), Flow->RequestStartGame(Player));
    TestTrue(TEXT("重复请求保持幂等"), Flow->RequestStartGame(Player));
    TestFalse(TEXT("提前点击不会直接开局"), Flow->HasGameStarted());
    TestNull(TEXT("提前点击不生成角色"), Player->GetPawn());
    Flow->ReportPlayerReady(Player);
    TestFalse(TEXT("报告就绪也不能绕过服务器场景检查"), Flow->CanSpawnPlayer(Player));
    Flow->bLocalSceneReady = true;
    Flow->UpdatePhase();
    TestEqual(TEXT("场景就绪后等待全体玩家与生成"), Flow->GetPhase(), EDeliveryStartupPhase::WaitingForPlayers);
    Flow->LocalError = FText::FromString(TEXT("test failure"));
    Flow->UpdatePhase();
    TestEqual(TEXT("失败不能显示为就绪"), Flow->GetPhase(), EDeliveryStartupPhase::Failed);
    TestFalse(TEXT("失败期间不能接收开始请求"), Flow->RequestStartGame(Player));
    Flow->LocalError = FText::GetEmpty();
    Flow->bGameStarted = true;
    Flow->Phase = EDeliveryStartupPhase::Playing;
    Flow->UpdatePhase();
    TestEqual(TEXT("游戏中换 Pawn 不会重新进入菜单"), Flow->GetPhase(), EDeliveryStartupPhase::Playing);
    DestroyStartupTestWorld(OtherWorld);
    DestroyStartupTestWorld(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryStartupTaskGateTest, "Delivery.Startup.TaskGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryStartupTaskGateTest::RunTest(const FString&)
{
    UWorld* World = MakeStartupTestWorld();
    World->SpawnActor<ADeliveryStartupFlow>();
    auto* State = World->SpawnActor<ADeliveryGameState>();
    World->SetGameState(State);
    auto* Tasks = State->GetTaskManager();
    auto* Definition = NewObject<UDeliveryTaskDefinition>(State);
    Definition->TaskId = TEXT("Startup_Test");
    Tasks->TaskDefinitions.Add(Definition);
    State->DispatchBeginPlay();
    World->Tick(LEVELTICK_All, 2.f);
    Tasks->ReevaluateUnlocks();
    TestEqual(TEXT("菜单期间无条件任务仍保持锁定"), Tasks->GetTaskStatus(Definition), EDeliveryTaskStatus::Locked);
    TestEqual(TEXT("菜单期间关卡时钟为零"), Tasks->GetTimeSinceLevelStart(), 0.f);
    TestEqual(TEXT("菜单期间没有来电"), State->GetPhoneCallQueue()->GetCallState(), EDeliveryPhoneCallState::Idle);
    Tasks->StartGameplay();
    TestEqual(TEXT("正式开局后任务才解锁"), Tasks->GetTaskStatus(Definition), EDeliveryTaskStatus::Offered);
    const float StartedAt = Tasks->LevelStartServerTime;
    World->Tick(LEVELTICK_All, 0.25f);
    Tasks->StartGameplay();
    TestEqual(TEXT("重复开局不会重置计时基准"), Tasks->LevelStartServerTime, StartedAt);
    TestEqual(TEXT("正式开局触发正常来电"), State->GetPhoneCallQueue()->GetCallState(), EDeliveryPhoneCallState::Ringing);
    DestroyStartupTestWorld(World);

    World = MakeStartupTestWorld();
    State = World->SpawnActor<ADeliveryGameState>();
    World->SetGameState(State);
    Tasks = State->GetTaskManager();
    Definition = NewObject<UDeliveryTaskDefinition>(State);
    Definition->TaskId = TEXT("Direct_Map_Test");
    Tasks->TaskDefinitions.Add(Definition);
    State->DispatchBeginPlay();
    TestEqual(TEXT("直接打开原地图仍自动开始"), Tasks->GetTaskStatus(Definition), EDeliveryTaskStatus::Offered);
    DestroyStartupTestWorld(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryStartupProgressTest, "Delivery.Startup.Progress",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryStartupProgressTest::RunTest(const FString&)
{
    UWorld* World = MakeStartupTestWorld();
    auto* Flow = World->SpawnActor<ADeliveryStartupFlow>();
    auto* Player = World->SpawnActor<APlayerController>();
    Player->Player = NewObject<ULocalPlayer>(GEngine);
    // 提供流送配置但不推进世界加载，用真实 Tick 检查显示进度不能释放玩法
    auto* Streaming = NewObject<ULevelStreamingDynamic>(World);
    Streaming->SetWorldAssetByPackageName(TEXT("/Game/Level/TestForCharacter"));
    World->AddStreamingLevel(Streaming);
    Flow->BeginLoading();
    TestEqual(TEXT("每次加载开始时进度为零"), Flow->GetLoadingProgress(), 0.f);
    Flow->LoadStartedAt = FPlatformTime::Seconds() - 5.0;
    Flow->Tick(0.1f);
    const float Partial = Flow->GetLoadingProgress();
    TestTrue(TEXT("尚未就绪时可以显示部分估算进度"), Partial > 0.f && Partial < 0.9f);
    Flow->EstimatedLoadSeconds *= 2.f;
    Flow->Tick(0.1f);
    TestTrue(TEXT("估计耗时改变也不倒退"), Flow->GetLoadingProgress() >= Partial);
    Flow->LoadStartedAt = FPlatformTime::Seconds() - 60.0;
    Flow->Tick(0.1f);
    TestEqual(TEXT("超过预估耗时也只能到九成"), Flow->GetLoadingProgress(), 0.9f);
    TestFalse(TEXT("九成进度不等于场景就绪"), Flow->IsSceneReady());
    Flow->RequestStartGame(Player);
    TestFalse(TEXT("显示进度不能代替开局条件"), Flow->HasGameStarted());
    TestNull(TEXT("提前开始仍不生成玩家"), Player->GetPawn());

    Flow->LocalError = FText::FromString(TEXT("test failure"));
    Flow->LoadingProgress = Partial;
    Flow->Tick(0.1f);
    TestEqual(TEXT("错误期间进度停止"), Flow->GetLoadingProgress(), Partial);
    TestTrue(TEXT("重试使用原有加载入口"), Flow->RetryLoading());
    TestEqual(TEXT("重试从零重新计"), Flow->GetLoadingProgress(), 0.f);
    TestTrue(TEXT("重试后保留提前开始的意图"), Flow->IsStartRequested());
    Flow->bLocalSceneReady = true;
    Flow->UpdateLoadingProgress();
    TestEqual(TEXT("真实就绪立即达到一百，不等预估时间"), Flow->GetLoadingProgress(), 1.f);
    Flow->LoadStartedAt = FPlatformTime::Seconds() - 100.0;
    Flow->UpdateLoadingProgress();
    TestEqual(TEXT("等待其他玩家时本机保持完成"), Flow->GetLoadingProgress(), 1.f);
    DestroyStartupTestWorld(World);
    return true;
}
#endif
