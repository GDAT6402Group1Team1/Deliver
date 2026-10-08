#include "Vehicle/DeliveryVehicleSummonComponent.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Components/BoxComponent.h"
#include "DeliveryCharacter.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Vehicle/DeliveryMotorbike.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryVehicleSummonTest, "Delivery.Vehicle.Summon.EmptyMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryVehicleSummonTest::RunTest(const FString&)
{
	const auto Settings = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
		.ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
		ERHIFeatureLevel::Num, &Settings);
	if (!TestNotNull(TEXT("test world"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};

	ADeliveryCharacter* Player = World->SpawnActor<ADeliveryCharacter>();
	if (!TestNotNull(TEXT("player"), Player)) return false;
	Player->SetActorLocation(FVector(0, 0, 200));
	UDeliveryVehicleSummonComponent* Summon = Player->FindComponentByClass<UDeliveryVehicleSummonComponent>();
	if (!TestNotNull(TEXT("summon component"), Summon)) return false;
	if (!TestNotNull(TEXT("default bike asset loads"), Summon->DefaultVehicleClass.LoadSynchronous())) return false;
	const auto CountBikes = [&]()
	{
		int32 Count = 0;
		for (TActorIterator<ADeliveryMotorbike> It(World); It; ++It)
			if (IsValid(*It)) ++Count;
		return Count;
	};

	// First summon must not leave a stranded vehicle or start cooldown when there is no ground.
	Summon->RequestSummon();
	TestEqual(TEXT("failed placement cleans up new bike"), CountBikes(), 0);
	TestEqual(TEXT("failed placement has no cooldown"), Summon->GetCooldownRemaining(), 0.f);

	AActor* FloorActor = World->SpawnActor<AActor>();
	UBoxComponent* Floor = NewObject<UBoxComponent>(FloorActor);
	FloorActor->SetRootComponent(Floor);
	Floor->SetBoxExtent(FVector(2000, 2000, 25));
	Floor->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Floor->SetCollisionObjectType(ECC_WorldStatic);
	Floor->SetCollisionResponseToAllChannels(ECR_Block);
	Floor->RegisterComponent();
	FloorActor->SetActorLocation(FVector(0, 0, -25));

	Summon->Cooldown = 0.f;
	Summon->RequestSummon();
	TestEqual(TEXT("empty map spawns exactly one bike"), CountBikes(), 1);
	TActorIterator<ADeliveryMotorbike> BikeIt(World);
	ADeliveryMotorbike* Bike = BikeIt ? *BikeIt : nullptr;
	if (!TestNotNull(TEXT("spawned bike"), Bike)) return false;
	TestTrue(TEXT("spawn uses configured blueprint"), Bike->IsA(Summon->DefaultVehicleClass.Get()));
	TestTrue(TEXT("bike replicates to clients"), Bike->GetIsReplicated() && Bike->IsReplicatingMovement());
	TestTrue(TEXT("bike placed in front of player"), FMath::IsNearlyEqual(Bike->GetActorLocation().X, double(Summon->PlaceDistance)));

	Bike->Driver = Player;
	const FVector OccupiedLocation = Bike->GetActorLocation();
	Player->SetActorLocation(FVector(0, 500, 200));
	Summon->RequestSummon();
	TestEqual(TEXT("occupied bike does not spawn a duplicate"), CountBikes(), 1);
	TestTrue(TEXT("occupied bike stays put"), Bike->GetActorLocation().Equals(OccupiedLocation));
	TestEqual(TEXT("occupied rejection has no cooldown"), Summon->GetCooldownRemaining(), 0.f);
	Bike->Driver = nullptr;

	Summon->Cooldown = 10.f;
	Summon->RequestSummon();
	TestEqual(TEXT("repeat summon reuses existing bike"), CountBikes(), 1);
	TestTrue(TEXT("existing bike follows new player position"), FMath::IsNearlyEqual(Bike->GetActorLocation().Y, 500.0));
	TestEqual(TEXT("successful summon starts cooldown"), Summon->GetCooldownRemaining(), 10.f);
	const FVector CoolingLocation = Bike->GetActorLocation();
	Player->SetActorLocation(FVector(0, 1000, 200));
	Summon->RequestSummon();
	TestTrue(TEXT("cooldown blocks immediate recall"), Bike->GetActorLocation().Equals(CoolingLocation));
	TestEqual(TEXT("cooldown creates no extra bikes"), CountBikes(), 1);
	return true;
}
#endif
