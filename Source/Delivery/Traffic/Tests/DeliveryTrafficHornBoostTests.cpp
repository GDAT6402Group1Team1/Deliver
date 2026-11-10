#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Traffic/DeliveryTrafficCarComponent.h"
#include "NPC/DeliveryPedestrian.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryTrafficHornBoostTest, "Delivery.Traffic.HornBoost",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryTrafficHornBoostTest::RunTest(const FString&)
{
    const auto Settings = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Settings);
    if (!TestNotNull(TEXT("测试世界"), World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); };
    auto* Car = World->SpawnActor<AActor>();
    auto* Root = NewObject<USceneComponent>(Car);
    Car->SetRootComponent(Root);
    Root->RegisterComponent();
    auto* Traffic = NewObject<UDeliveryTrafficCarComponent>(Car);
    Traffic->bSpawnExtraWaves = false;
    Traffic->bScaleInOnSpawn = false;
    Traffic->bScaleOutOnRouteLost = false;
    Traffic->SpeedMultiplierChangeRate = 100.f;
    Traffic->FollowAccelerationPause = 0.f;
    Traffic->RegisterComponent();
    Traffic->SetComponentTickEnabled(false);
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    auto Tick = [&] { Traffic->TickComponent(.1f, LEVELTICK_All, nullptr); };
    Traffic->UpdateSpeedState(900.f,1000.f);
    Traffic->RequestHornSpeedBoost(1.2f);
    Tick();
    TestEqual(TEXT("未满速不响应鸣笛"),Traffic->GetSpeedMultiplier(),1.f);
    Traffic->UpdateSpeedState(1000.f,1000.f);
    Traffic->RequestHornSpeedBoost(1.2f);
    Tick();
    TestEqual(TEXT("畅通满速临时超出上限"),Traffic->GetSpeedMultiplier(),1.2f);
    Traffic->RequestHornSpeedBoost(1.2f);
    Tick();
    TestEqual(TEXT("重复鸣笛不叠乘"),Traffic->GetSpeedMultiplier(),1.2f);
    Traffic->UpdateStoppedAtIntersection(true);
    Tick();
    TestTrue(TEXT("红灯状态立即撤销额外倍率"),Traffic->GetSpeedMultiplier() <= 1.f);
    Traffic->UpdateStoppedAtIntersection(false);
    auto* Person = World->SpawnActor<ADeliveryPedestrian>();
    Person->SetActorScale3D(FVector::OneVector);
    Person->SetActorLocation(FVector(Traffic->TraceStartForwardOffset+150.f,0,0));
    Traffic->RequestHornSpeedBoost(1.2f);
    Tick();
    TestTrue(TEXT("鸣笛不能覆盖行人停车"),Traffic->IsYieldingToPedestrian());
    TestEqual(TEXT("有人仍完全停住"),Traffic->GetSpeedMultiplier(),0.f);
    Person->Destroy();
    Traffic->SpeedMultiplier = 1.f;
    Traffic->HornBoostUntil = -1.f;
    Tick();
    TestEqual(TEXT("租约到期恢复正常上限"),Traffic->GetSpeedMultiplier(),1.f);
    Traffic->UpdateSpeedState(1000.f,1000.f);
    Traffic->RequestHornSpeedBoost(1.2f);
    Traffic->NotifyResetToStart();
    TestTrue(TEXT("路线重置清除鸣笛租约"),Traffic->HornBoostUntil < 0.f);
    return true;
}
#endif
