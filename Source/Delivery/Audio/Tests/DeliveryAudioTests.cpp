#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Audio/Tests/DeliveryAudioTestComponent.h"
#include "Audio/DeliveryVehicleAudioComponent.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Sound/SoundWave.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryAudioEventTest, "Delivery.Audio.Events",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryAudioEventTest::RunTest(const FString&)
{
    const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
        ERHIFeatureLevel::Num, &Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);

    AActor* First = World->SpawnActor<AActor>();
    AActor* Second = World->SpawnActor<AActor>();
    auto* FirstEvents = NewObject<UDeliveryEventComponent>(First);
    auto* SecondEvents = NewObject<UDeliveryEventComponent>(Second);
    FirstEvents->RegisterComponent();
    SecondEvents->RegisterComponent();
    auto* FirstAudio = NewObject<UDeliveryAudioTestComponent>(First);
    auto* SecondAudio = NewObject<UDeliveryAudioTestComponent>(Second);
    auto* Config = NewObject<UDeliveryAudioConfig>(World);
    FDeliverySoundSettings Sound;
    Sound.Sound = NewObject<USoundWave>(Config);
    Sound.Volume = 0.4f;
    Config->Sounds.Add(EDeliveryEvent::PickupSucceeded, Sound);
    FirstAudio->Config = Config;
    SecondAudio->Config = Config;
    FirstAudio->RegisterComponent();
    SecondAudio->RegisterComponent();

    AActor* Vehicle = World->SpawnActor<AActor>();
    auto* Engine = NewObject<UAudioComponent>(Vehicle, TEXT("EngineAudio"));
    Engine->bAutoActivate = false;
    Vehicle->SetRootComponent(Engine);
    Engine->RegisterComponent();
    auto* VehicleAudio = NewObject<UDeliveryVehicleAudioComponent>(Vehicle);
    auto* VehicleConfig = NewObject<UDeliveryVehicleAudioConfig>(VehicleAudio);
    VehicleConfig->EngineSound = Sound.Sound;
    VehicleConfig->IdlePitch = 0.8f;
    VehicleConfig->MaxPitch = 1.7f;
    VehicleConfig->IdleVolume = 0.15f;
    VehicleConfig->MaxVolume = 0.5f;
    VehicleAudio->Config = VehicleConfig;
    VehicleAudio->RegisterComponent();
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();

    // 共用配置不能让两个对象共用事件，未配置的事件也不能误播
    FirstEvents->Publish(EDeliveryEvent::PickupSucceeded);
    TestEqual(TEXT("事件触发一次播放"), FirstAudio->PlayCount, 1);
    TestEqual(TEXT("其他对象不收到事件"), SecondAudio->PlayCount, 0);
    TestEqual(TEXT("使用配置中的音量"), FirstAudio->LastVolume, 0.4f);
    FirstEvents->Publish(EDeliveryEvent::DeliverySucceeded);
    TestEqual(TEXT("缺少配置时不播放"), FirstAudio->PlayCount, 1);
    SecondEvents->Publish(EDeliveryEvent::PickupSucceeded);
    TestEqual(TEXT("另一对象可以独立播放"), SecondAudio->PlayCount, 1);
    TestEqual(TEXT("配置不被播放修改"), Config->Sounds.Num(), 1);

    // 订阅随组件结束而解除，发布者可以继续存在
    FirstAudio->DestroyComponent();
    TestFalse(TEXT("销毁后解除订阅"), FirstEvents->OnEvent.IsBound());
    FirstEvents->Publish(EDeliveryEvent::PickupSucceeded);

    // 发动机在正向和倒车时都按速度大小计算，速度超过上限时不继续升调
    VehicleAudio->UpdateEngine(true, -0.5f);
    TestEqual(TEXT("半速音调"), Engine->PitchMultiplier, 1.25f);
    TestEqual(TEXT("半速音量"), Engine->VolumeMultiplier, 0.325f);
    VehicleAudio->UpdateEngine(true, 2.0f);
    TestEqual(TEXT("音调限制在配置上限"), Engine->PitchMultiplier, 1.7f);
    VehicleAudio->UpdateEngine(false, 1.0f);
    TestFalse(TEXT("下车后停止发动机"), Engine->IsPlaying());

    First->RouteEndPlay(EEndPlayReason::Quit);
    Second->RouteEndPlay(EEndPlayReason::Quit);
    Vehicle->RouteEndPlay(EEndPlayReason::Quit);
    World->SetBegunPlay(false);
    World->DestroyWorld(false);
    GEngine->DestroyWorldContext(World);
    return true;
}
#endif
