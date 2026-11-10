#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Traffic/DeliveryTrafficCarComponent.h"
#include "NPC/DeliveryPedestrian.h"
#include "Character/DeliveryCharacter.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryTrafficWaveSpawnTest,
                                 "Delivery.Traffic.WaveSpawnClearance",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FDeliveryTrafficWaveSpawnTest::RunTest(const FString&)
{
	const auto Settings = UWorld::InitializationValues()
	                          .AllowAudioPlayback(false)
	                          .CreatePhysicsScene(true)
	                          .ShouldSimulatePhysics(false)
	                          .CreateNavigation(false)
	                          .CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
	                                    ERHIFeatureLevel::Num, &Settings);
	if (!TestNotNull(TEXT("测试世界"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->EndPlay(EEndPlayReason::Quit);
		World->DestroyWorld(false);
	};

	AActor* SourceCar = World->SpawnActor<AActor>();
	USceneComponent* Root = NewObject<USceneComponent>(SourceCar);
	SourceCar->SetRootComponent(Root);
	Root->RegisterComponent();
	SourceCar->SetActorLocation(FVector(2000.f, 0.f, 0.f));
	UDeliveryTrafficCarComponent* Traffic = NewObject<UDeliveryTrafficCarComponent>(SourceCar);
	Traffic->bSpawnExtraWaves = false;
	Traffic->bScaleInOnSpawn = false;
	Traffic->RegisterComponent();
	Traffic->SecondWaveTransform = FTransform::Identity;
	Traffic->WaveSpawnPersonClearance = 400.f;
	Traffic->WaveSpawnHeightTolerance = 200.f;

	auto CountWaves = [World]()
	{
		int32 Count = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->ActorHasTag(TEXT("TrafficWave1")))
			{
				++Count;
			}
		}
		return Count;
	};

	ADeliveryPedestrian* Pedestrian = World->SpawnActor<ADeliveryPedestrian>();
	Pedestrian->SetActorLocation(FVector(100.f, 0.f, 0.f));
	Traffic->SpawnNextWave();
	TestEqual(TEXT("出生点有人时不生成车辆"), CountWaves(), 0);

	Pedestrian->SetActorLocation(FVector(450.f, 0.f, 0.f));
	ADeliveryCharacter* Player = World->SpawnActor<ADeliveryCharacter>();
	Player->SetActorLocation(FVector(100.f, 0.f, 0.f));
	Traffic->SpawnNextWave();
	TestEqual(TEXT("出生点有玩家时不生成车辆"), CountWaves(), 0);

	Player->SetActorLocation(FVector(450.f, 0.f, 0.f));
	Traffic->SpawnNextWave();
	TestEqual(TEXT("人离开后可以生成车辆"), CountWaves(), 1);
	return true;
}
#endif
