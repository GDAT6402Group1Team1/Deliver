#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Character/DeliveryCharacter.h"
#include "NPC/DeliveryPedestrian.h"
#include "NPC/DeliveryPedestrianRoute.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryBikeHornTest, "Delivery.Vehicle.HeldHorn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryBikeHornTest::RunTest(const FString&)
{
    const auto Settings = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Settings);
    if (!TestNotNull(TEXT("测试世界"), World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); };
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    auto* Bike = World->SpawnActor<ADeliveryMotorbike>();
    auto* Person = World->SpawnActor<ADeliveryPedestrian>();
    auto* PC = World->SpawnActor<APlayerController>();
    auto* Driver = World->SpawnActor<ADeliveryCharacter>(LoadClass<ADeliveryCharacter>(nullptr, TEXT("/Game/Blueprint/Character/DeliverMan/BP_DeliveryMan.BP_DeliveryMan_C")), FVector(0,0,500), FRotator::ZeroRotator);
    if (!Bike || !Person || !PC || !Driver) return false;
    Bike->ServerRequestHorn_Implementation(true);
    TestFalse(TEXT("无人驾驶不能鸣笛"), Bike->bHornActive);
    Bike->Driver = Driver;
    PC->Possess(Bike);
    Bike->ServerRequestHorn_Implementation(true);
    TestTrue(TEXT("按下持续鸣笛"), Bike->bHornActive);
    Person->BaseWalkSpeed = Person->WalkSpeed = 200.f;
    Person->SetActorScale3D(FVector::OneVector);
    Person->SetActorLocation(Bike->GetActorLocation() + FVector(500,0,0));
    Bike->UpdateHorn(.2f);
    TestEqual(TEXT("普通行人不受鸣笛影响"), Person->GetWalkSpeed(), 200.f);
    auto* Route = World->SpawnActor<ADeliveryPedestrianRoute>();
    Route->bRoamInArea = false;
    Route->RedLightWalkSpeedMultiplier = 1.f;
    Route->CrosswalkActor = World->SpawnActor<AActor>();
    Person->Route = Route;
    Route->Alive.Add(Person);
    auto* Other = World->SpawnActor<ADeliveryPedestrian>();
    Other->BaseWalkSpeed = Other->WalkSpeed = 160.f;
    Other->Route = Route;
    Other->SetActorLocation(Bike->GetActorLocation() + FVector(2000,0,0));
    Route->Alive.Add(Other);
    Bike->UpdateHorn(.2f);
    TestEqual(TEXT("前方过街行人加速"), Person->GetWalkSpeed(), 400.f);
    TestEqual(TEXT("同一条线范围外的人一起加速"), Other->GetWalkSpeed(), 320.f);
    Bike->UpdateHorn(.2f);
    TestEqual(TEXT("重复鸣笛不叠乘"), Person->GetWalkSpeed(), 400.f);
    Route->HornBoostUntil = -1.f;
    Route->UpdateCrosswalkSpeed();
    TestEqual(TEXT("整条线恢复各自原速"), Other->GetWalkSpeed(), 160.f);
    Person->SetActorLocation(Bike->GetActorLocation() + FVector(-500,0,0));
    Bike->UpdateHorn(.2f);
    TestEqual(TEXT("车后行人不加速"), Person->GetWalkSpeed(), 200.f);
    Person->SetActorLocation(Bike->GetActorLocation() + FVector(1200,0,0));
    Bike->UpdateHorn(.2f);
    TestEqual(TEXT("范围外行人不加速"), Person->GetWalkSpeed(), 200.f);
    Bike->ServerRequestHorn_Implementation(false);
    TestFalse(TEXT("松开停止鸣笛"), Bike->bHornActive);
    Bike->ServerRequestHorn_Implementation(true);
    PC->UnPossess();
    Bike->UpdateHorn(.2f);
    TestFalse(TEXT("失去控制停止鸣笛"), Bike->bHornActive);
    return true;
}
#endif
