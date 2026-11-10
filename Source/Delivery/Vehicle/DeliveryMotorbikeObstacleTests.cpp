#if WITH_DEV_AUTOMATION_TESTS
#include "Vehicle/DeliveryMotorbike.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryMotorbikeObstacleTest,
                                 "Delivery.Vehicle.ObstacleEscape",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)

bool FDeliveryMotorbikeObstacleTest::RunTest(const FString&)
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

	AActor* Post = World->SpawnActor<AActor>();
	UBoxComponent* PostBody = NewObject<UBoxComponent>(Post);
	Post->SetRootComponent(PostBody);
	PostBody->SetBoxExtent(FVector(30.f, 30.f, 120.f));
	PostBody->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	PostBody->SetCollisionObjectType(ECC_WorldStatic);
	PostBody->SetCollisionResponseToAllChannels(ECR_Block);
	PostBody->RegisterComponent();
	Post->SetActorLocation(FVector(170.f, 0.f, 100.f));

	ADeliveryMotorbike* Bike = World->SpawnActor<ADeliveryMotorbike>();
	if (!TestNotNull(TEXT("摩托车"), Bike))
	{
		return false;
	}
	Bike->MaxStepHeight = 0.f;
	for (const ECollisionChannel ObstacleType : {ECC_WorldStatic, ECC_WorldDynamic})
	{
		// 地形类和花坛、电线杆等独立网格使用不同对象类型，都应能脱困
		PostBody->SetCollisionObjectType(ObstacleType);
		Bike->SetActorLocationAndRotation(FVector(0.f, 0.f, 100.f), FRotator::ZeroRotator,
		                                  false, nullptr, ETeleportType::TeleportPhysics);
		Bike->CurrentSpeed = 500.f;
		Bike->UpdateGroundAndMove(0.1f);
		TestTrue(TEXT("障碍仍会挡住摩托车"), Bike->GetActorLocation().X < 50.f);
		TestTrue(TEXT("正面顶住后记录受阻状态"), Bike->bBlockedByWall);

		Bike->CurrentSpeed = 20.f;
		Bike->SteerInput = 1.f;
		Bike->UpdateSteering(0.1f);
		TestTrue(TEXT("低速受阻时仍能明显转向"), Bike->GetActorRotation().Yaw > 4.f);
	}
	return true;
}
#endif
