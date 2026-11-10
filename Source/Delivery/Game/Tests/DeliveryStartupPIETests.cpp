#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Game/DeliveryStartupFlow.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "UI/DeliveryHUD.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/StrongObjectPtr.h"

// 使用真实入口和原地图，验证双人加载、任务门控、出生和菜单输入的协作
class FDeliveryStartupPIECommand : public IAutomationLatentCommand
{
public:
    explicit FDeliveryStartupPIECommand(FAutomationTestBase* InTest) : Test(InTest) {}
    virtual bool Update() override
    {
        TArray<UWorld*> Worlds;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
            if (Context.WorldType == EWorldType::PIE && Context.World()) Worlds.Add(Context.World());
        if (bEnding) return Worlds.IsEmpty();
        if (!bRequested)
        {
            if (!Worlds.IsEmpty()) { Test->AddError(TEXT("请先退出正在运行的 PIE")); return true; }
            if (!FEditorFileUtils::LoadMap(TEXT("/Game/Level/L_MainMenu"), false, false))
            { Test->AddError(TEXT("启动测试地图不存在，请执行 setup_startup_map.py")); return true; }
            Settings.Reset(DuplicateObject<ULevelEditorPlaySettings>(GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage()));
            Settings->SetPlayNetMode(PIE_ListenServer);
            Settings->SetPlayNumberOfClients(2);
            Settings->SetRunUnderOneProcess(true);
            FRequestPlaySessionParams Params;
            Params.EditorPlaySettings = Settings.Get();
            Params.bAllowOnlineSubsystem = false;
            GEditor->RequestPlaySession(Params);
            bRequested = true;
            StartedAt = FPlatformTime::Seconds();
            return false;
        }
        if (FPlatformTime::Seconds() - StartedAt > 180)
        { Test->AddError(TEXT("启动流程双人 PIE 超时")); return End(); }
        if (Worlds.Num() < 2) return false;
        ADeliveryStartupFlow* Host = nullptr;
        APlayerController* HostPlayer = nullptr;
        bool bAllReady = true;
        bool bAllPlaying = true;
        for (UWorld* World : Worlds)
        {
            auto* Flow = ADeliveryStartupFlow::GetDeliveryStartupFlow(World);
            if (!Flow || !Flow->HasActorBegunPlay()) return false;
            if (Flow->GetPhase() == EDeliveryStartupPhase::Failed)
            { Test->AddError(Flow->GetLoadingError().ToString()); return End(); }
            const float Progress = Flow->GetLoadingProgress();
            if (Progress < 0.f || Progress > (Flow->IsSceneReady() ? 1.f : 0.9f))
            { Test->AddError(TEXT("显示进度越界或先于真实就绪填满")); return End(); }
            if (Flow->IsSceneReady() && Progress != 1.f)
            { Test->AddError(TEXT("真实就绪后进度没有完成")); return End(); }
            bAllReady &= Flow->IsSceneReady();
            bAllPlaying &= Flow->GetPhase() == EDeliveryStartupPhase::Playing;
            if (World->GetNetMode() == NM_ListenServer)
            {
                Host = Flow;
                HostPlayer = World->GetFirstPlayerController();
            }
        }
        if (!Host || !HostPlayer) return false;
        if (!bStartedGame)
        {
            if (!bAllReady) return false;
            if (ReadyAt == 0) { ReadyAt = FPlatformTime::Seconds(); return false; }
            if (FPlatformTime::Seconds() - ReadyAt < 0.5) return false;
            for (UWorld* World : Worlds)
            {
                auto* Flow = ADeliveryStartupFlow::GetDeliveryStartupFlow(World);
                auto* Tasks = UDeliveryTaskManagerComponent::Get(World);
                Test->TestNotNull(TEXT("使用原 GameState 的任务组件"), Tasks);
                if (Tasks)
                {
                    Test->TestTrue(TEXT("任务清单来自原游戏配置"), !Tasks->GetAllTaskDefinitions().IsEmpty());
                    Test->TestEqual(TEXT("加载完成但未开始时任务时钟仍为零"), Tasks->GetTimeSinceLevelStart(), 0.f);
                }
                for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
                {
                    APlayerController* Player = It->Get();
                    if (!Player || !Player->IsLocalController()) continue;
                    Test->TestTrue(TEXT("未点击开始前不生成物理角色"), Player->GetPawn() == nullptr);
                    if (!Flow->HasAuthority())
                        Test->TestFalse(TEXT("远端客户端不能替主机开始游戏"), Flow->RequestStartGame(Player));
                }
            }
            Test->TestTrue(TEXT("主机开始请求被接收"), Host->RequestStartGame(HostPlayer));
            bStartedGame = true;
            return false;
        }
        if (!bAllPlaying) return false;
        if (PlayingAt == 0) { PlayingAt = FPlatformTime::Seconds(); return false; }
        if (FPlatformTime::Seconds() - PlayingAt < 0.3) return false;
        for (UWorld* World : Worlds)
        {
            for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
            {
                APlayerController* Player = It->Get();
                if (!Player || !Player->IsLocalController()) continue;
                Test->TestTrue(TEXT("每端控制自己的游戏角色"), Player->GetPawn() != nullptr);
                auto* HUD = Cast<ADeliveryHUD>(Player->GetHUD());
                Test->TestNotNull(TEXT("沿用 DeliveryHUD"), HUD);
                if (HUD) Test->TestFalse(TEXT("开始后注销启动菜单输入"), HUD->IsPanelOpen(TEXT("Startup")));
            }
        }
        APawn* Pawn = HostPlayer->GetPawn();
        Test->TestTrue(TEXT("重复点击开始被安全接收"), Host->RequestStartGame(HostPlayer));
        Test->TestTrue(TEXT("重复开始不会重新生成角色"), HostPlayer->GetPawn() == Pawn);
        Test->TestTrue(TEXT("开局后任务时钟运行"), UDeliveryTaskManagerComponent::Get(Host)->GetTimeSinceLevelStart() > 0.f);
        return End();
    }
private:
    bool End() { GEditor->RequestEndPlayMap(); bEnding = true; return false; }
    FAutomationTestBase* Test;
    TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
    bool bRequested = false;
    bool bStartedGame = false;
    bool bEnding = false;
    double StartedAt = 0;
    double ReadyAt = 0;
    double PlayingAt = 0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryStartupPIETest, "Delivery.Startup.ListenServerFlow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryStartupPIETest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FDeliveryStartupPIECommand(this));
    return true;
}
#endif
