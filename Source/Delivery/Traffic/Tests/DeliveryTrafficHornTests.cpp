#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Traffic/DeliveryTrafficCarComponent.h"
#include "Audio/DeliveryVehicleAudioComponent.h"
#include "Components/AudioComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Sound/SoundWave.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryTrafficHornTest, "Delivery.Traffic.Horn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryTrafficHornTest::RunTest(const FString&)
{
    const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
        ERHIFeatureLevel::Num, &Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);

    auto AddRoot = [](AActor* Actor)
    {
        auto* Root = NewObject<USceneComponent>(Actor);
        Actor->SetRootComponent(Root);
        Root->RegisterComponent();
    };
    AActor* Car = World->SpawnActor<AActor>();
    AddRoot(Car);
    auto* Traffic = NewObject<UDeliveryTrafficCarComponent>(Car);
    Traffic->bScaleInOnSpawn = false;
    Traffic->bScaleOutOnRouteLost = false;
    Traffic->bSpawnExtraWaves = false;
    Traffic->RegisterComponent();
    Traffic->SetComponentTickEnabled(false);
    TestEqual(TEXT("新组件自带一条鸣笛资源"), Traffic->HornSounds.Num(), 1);
    if (Traffic->HornSounds.Num() == 1)
    {
        TestNotNull(TEXT("beep-beep 默认资源可加载"), Traffic->HornSounds[0].Get());
        if (Traffic->HornSounds[0])
        {
            TestFalse(TEXT("正式鸣笛不是循环音效"), Traffic->HornSounds[0]->IsLooping());
        }
    }

    auto* FirstSound = NewObject<USoundWave>(Traffic);
    auto* SecondSound = NewObject<USoundWave>(Traffic);
    FirstSound->Duration = 1.0f;
    SecondSound->Duration = 1.0f;
    Traffic->HornSounds = {FirstSound, nullptr, SecondSound, FirstSound};
    APlayerController* FirstPlayer = World->SpawnActor<APlayerController>();
    APlayerController* SecondPlayer = World->SpawnActor<APlayerController>();
    APawn* Walker = World->SpawnActor<APawn>();
    APawn* Rider = World->SpawnActor<APawn>();
    APawn* OtherPawn = World->SpawnActor<APawn>();
    AddRoot(Walker);
    AddRoot(Rider);
    AddRoot(OtherPawn);
    Walker->SetActorLocation(FVector(2000, 0, 0));
    OtherPawn->SetActorLocation(FVector(2000, 0, 0));
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    // 按真实登录顺序在初始化后 Possess，否则 PostInitializeComponents 会把测试玩家重置成旁观状态
    FirstPlayer->Possess(Walker);
    SecondPlayer->Possess(OtherPawn);
    TestTrue(TEXT("第一名玩家已进入游戏状态"), FirstPlayer->IsInState(NAME_Playing));
    TestTrue(TEXT("第二名玩家已进入游戏状态"), SecondPlayer->IsInState(NAME_Playing));

    Traffic->UpdatePlayerProximityHorn(0.2f);
    TestFalse(TEXT("远处不鸣笛"), Traffic->LastHornSound.IsValid());
    for (const FVector& Position : {FVector(-500, 0, 0), FVector(0, 500, 0), FVector(100, 500, 0), FVector(0, 0, 500)})
    {
        Walker->SetActorLocation(Position);
        Traffic->UpdatePlayerProximityHorn(0.2f);
        TestFalse(TEXT("后方、侧面、扇区外和正上方均不触发"), Traffic->LastHornSound.IsValid());
    }
    Car->SetActorRotation(FRotator(15, 90, 0));
    Walker->SetActorLocation(FVector(500, 0, 0));
    Traffic->UpdatePlayerProximityHorn(0.2f);
    TestFalse(TEXT("前方跟随车头旋转而不是世界 X 轴"), Traffic->LastHornSound.IsValid());
    Walker->SetActorLocation(FVector(0, 500, 100));
    OtherPawn->SetActorLocation(FVector(0, 600, 0));
    Traffic->UpdatePlayerProximityHorn(0.2f);
    TestTrue(TEXT("进入范围发出鸣笛"), Traffic->LastHornSound.IsValid());
    TestEqual(TEXT("同一次鸣笛覆盖两名玩家"), Traffic->HonkedPlayers.Num(), 2);
    const double FirstDeadline = Traffic->NextHornTime;
    TestFalse(TEXT("直接请求也受同一冷却约束"), Traffic->TryHonk());
    TestEqual(TEXT("重复请求不延长冷却"), Traffic->NextHornTime, FirstDeadline);

    Car->SetActorRotation(FRotator::ZeroRotator);
    Walker->SetActorLocation(FVector(500, 0, 0));
    OtherPawn->SetActorLocation(FVector(600, 0, 0));

    // 消除等待时间后继续留在附近也不能连响
    Traffic->NextHornTime = 0;
    Traffic->UpdatePlayerProximityHorn(0.2f);
    TestEqual(TEXT("持续贴近不会再次触发"), Traffic->NextHornTime, 0.0);
    Walker->SetActorLocation(FVector(900, 0, 0));
    Traffic->UpdatePlayerProximityHorn(0.2f);
    Walker->SetActorLocation(FVector(500, 0, 0));
    Traffic->UpdatePlayerProximityHorn(0.2f);
    TestEqual(TEXT("在触发边界来回移动不会连响"), Traffic->NextHornTime, 0.0);

    Rider->SetActorLocation(Walker->GetActorLocation());
    FirstPlayer->Possess(Rider);
    Traffic->UpdatePlayerProximityHorn(0.2f);
    TestEqual(TEXT("上下车换 Pawn 不视为新玩家"), Traffic->NextHornTime, 0.0);
    USoundBase* Previous = Traffic->LastHornSound.Get();
    Rider->SetActorLocation(FVector(1200, 0, 0));
    Traffic->UpdatePlayerProximityHorn(0.2f);
    TestFalse(TEXT("离开后重新具备触发资格"), Traffic->HonkedPlayers.Contains(FirstPlayer));
    Rider->SetActorLocation(FVector(500, 0, 0));
    Traffic->NextHornTime = World->GetTimeSeconds() + 8.0;
    Traffic->UpdatePlayerProximityHorn(0.2f);
    TestFalse(TEXT("冷却中不吞掉新靠近玩家的资格"), Traffic->HonkedPlayers.Contains(FirstPlayer));
    Traffic->NextHornTime = 0;
    Traffic->UpdatePlayerProximityHorn(0.2f);
    TestTrue(TEXT("冷却结束仍在附近可以触发"), Traffic->HonkedPlayers.Contains(FirstPlayer));
    TestNotEqual(TEXT("多个不同声音避免连续重复"), Traffic->LastHornSound.Get(), Previous);

    // 车重置不绕过冷却，出现消失阶段也不能鸣笛
    const double BeforeReset = Traffic->NextHornTime;
    Traffic->NotifyResetToStart();
    TestEqual(TEXT("重置清除附近玩家记录"), Traffic->HonkedPlayers.Num(), 0);
    TestEqual(TEXT("重置不清除冷却"), Traffic->NextHornTime, BeforeReset);
    Traffic->NextHornTime = 0;
    Traffic->bScaleInRunning = true;
    TestFalse(TEXT("缩放出现阶段拒绝鸣笛"), Traffic->TryHonk());
    Traffic->bScaleInRunning = false;
    Traffic->bRouteLostPending = true;
    TestFalse(TEXT("等待重置阶段拒绝鸣笛"), Traffic->TryHonk());
    Traffic->bRouteLostPending = false;
    Traffic->bAutoHonkNearPlayers = false;
    Traffic->UpdatePlayerProximityHorn(0.2f);
    TestEqual(TEXT("关闭自动鸣笛后不靠距离触发"), Traffic->NextHornTime, 0.0);
    Traffic->HornSounds = {nullptr};
    TestFalse(TEXT("空声音槽不占用冷却"), Traffic->TryHonk());
    FirstSound->bLooping = true;
    Traffic->HornSounds = {FirstSound};
    TestFalse(TEXT("拒绝会不停播放的循环音效"), Traffic->TryHonk());
    FirstSound->bLooping = false;
    TestTrue(TEXT("单个声音仍能通过手动接口触发"), Traffic->TryHonk());

    // 音源跟随当前车辆，默认衰减不依赖 SoundWave 自己的设置
    auto* Audio = Traffic->TrafficAudio.Get();
    if (TestNotNull(TEXT("复用车辆音效组件"), Audio))
    {
        auto* Horn = Audio->HornAudio.Get();
        if (TestNotNull(TEXT("创建独立鸣笛音源"), Horn))
        {
            TestTrue(TEXT("默认音量随机范围"), Horn->VolumeMultiplier >= 0.9f && Horn->VolumeMultiplier <= 1.1f);
            TestTrue(TEXT("默认音调随机范围"), Horn->PitchMultiplier >= 0.95f && Horn->PitchMultiplier <= 1.05f);

            // 固定上下限检查真实音源收到的参数，降低音调时冷却不能短于音效时长
            Traffic->HornVolume = 0.7f;
            Traffic->HornVolumeRange = FVector2D(1.1f, 1.1f);
            Traffic->HornPitchRange = FVector2D(0.8f, 0.8f);
            Traffic->HornCooldown = 0.1f;
            Traffic->NextHornTime = 0;
            FirstSound->Duration = 2.0f;
            TestTrue(TEXT("可通过相同上下限固定变化量"), Traffic->TryHonk());
            TestEqual(TEXT("随机倍率乘到基础音量"), Horn->VolumeMultiplier, 0.77f);
            TestEqual(TEXT("音源收到服务器选定的音调"), Horn->PitchMultiplier, 0.8f);
            TestEqual(TEXT("降调后的两秒声音需要等待两秒半"), Traffic->NextHornTime - World->GetTimeSeconds(), 2.5);
            Traffic->HornVolumeRange = FVector2D(1.1f, 0.9f);
            Traffic->HornPitchRange = FVector2D(1.05f, 0.95f);
            Traffic->NextHornTime = 0;
            TestTrue(TEXT("反填上下限仍可请求鸣笛"), Traffic->TryHonk());
            TestTrue(TEXT("反填后音量仍在范围内"), Horn->VolumeMultiplier >= 0.63f && Horn->VolumeMultiplier <= 0.77f);
            TestTrue(TEXT("反填后音调仍在范围内"), Horn->PitchMultiplier >= 0.95f && Horn->PitchMultiplier <= 1.05f);
            TestEqual(TEXT("音源挂在车上"), Horn->GetAttachParent(), Car->GetRootComponent());
            TestTrue(TEXT("未指定资产也启用距离衰减"), Horn->bOverrideAttenuation != 0);
            TestTrue(TEXT("鸣笛采用空间声"), Horn->AttenuationOverrides.bSpatialize != 0);
            TestTrue(TEXT("随车辆销毁停止"), Horn->bStopWhenOwnerDestroyed != 0);
            TWeakObjectPtr<UAudioComponent> SavedHorn = Horn;
            Traffic->DestroyComponent();
            TestTrue(TEXT("交通组件移除后清理创建的音源"), !SavedHorn.IsValid() || !SavedHorn->IsRegistered());
        }
    }

    World->EndPlay(EEndPlayReason::Quit);
    World->DestroyWorld(false);
    GEngine->DestroyWorldContext(World);
    return true;
}
#endif
