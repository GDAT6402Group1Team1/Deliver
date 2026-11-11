#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "NPC/DeliveryPedestrian.h"
#include "NPC/DeliveryPedestrianRoute.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/WorldSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryBikePedestrianImpactTest, "Delivery.Vehicle.PedestrianImpact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryBikePedestrianImpactTest::RunTest(const FString&)
{
    const auto Settings = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Settings);
    if (!TestNotNull(TEXT("测试世界"), World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); };
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    auto* Route = World->SpawnActor<ADeliveryPedestrianRoute>();
    Route->SetActorTickEnabled(false);
    auto* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Characters/NPC/SK_NpcBoy.SK_NpcBoy"));
    if (!TestNotNull(TEXT("正式小学生模型"), Mesh)) return false;
    auto SpawnChild = [&](FVector Position)
    {
        auto* Child = World->SpawnActor<ADeliveryPedestrian>(ADeliveryPedestrian::StaticClass(),
            FTransform(FRotator::ZeroRotator, Position, FVector(.6f)));
        Child->BeginWalk(Route, Mesh, nullptr, 0, true, 130.f, 150.f);
        Child->SetActorLocation(Position);
        Child->SetActorScale3D(FVector(.6f));
        Child->bYieldToObstacles = false;
        return Child;
    };
    auto* Bike = World->SpawnActor<ADeliveryMotorbike>();
    auto* Child = SpawnChild(FVector(0,0,0));
    const FVector Origin = Child->GetActorLocation();
    Bike->CurrentSpeed = 700.f;
    Bike->PreviousPedestrianSweepLocation = FVector(-140,0,50);
    Bike->bHasPedestrianSweepLocation = true;
    Bike->SetActorLocation(FVector(10,0,50));
    Bike->ProcessPedestrianImpacts(.25f);
    TestTrue(TEXT("车身真实扫掠命中 0.6 倍小学生"), Child->IsKnockedDown());
    Child->Tick(.3f);
    TestTrue(TEXT("强撞击途中明显离地"), Child->GetActorLocation().Z > Origin.Z + 25.f);
    Child->Tick(.3f);
    const float Travel = FVector::Dist2D(Origin, Child->GetActorLocation());
    AddInfo(FString::Printf(TEXT("BikeImpact displacement=%.1f cm"), Travel));
    TestTrue(TEXT("正常车速将小学生击离车头至少一米"), Travel > 100.f);
    TestTrue(TEXT("身体确实倾倒"), Child->GetActorUpVector().Z < .5f);
    const FVector Rest = Child->GetActorLocation();
    Child->ReceiveImpact(FVector(48000,0,0), Rest);
    Child->Tick(.1f);
    TestTrue(TEXT("重复接触不重新发射或拉回路线"), Child->GetActorLocation().Equals(Rest, .1f));
    auto* SlowChild = SpawnChild(FVector(0,400,0));
    Bike->CurrentSpeed = 80.f;
    Bike->PreviousPedestrianSweepLocation = FVector(-20,400,50);
    Bike->SetActorLocation(FVector(10,400,50));
    Bike->ProcessPedestrianImpacts(.1f);
    TestFalse(TEXT("低速擦过不会触发撞飞"), SlowChild->IsKnockedDown());
    Bike->CurrentSpeed = 700.f;
    Bike->PreviousPedestrianSweepLocation = FVector(-4000,400,50);
    Bike->ProcessPedestrianImpacts(.1f);
    TestFalse(TEXT("召唤瞬移不沿途撞人"), SlowChild->IsKnockedDown());
    auto* Wall = World->SpawnActor<AActor>();
    auto* Box = NewObject<UBoxComponent>(Wall);
    Wall->SetRootComponent(Box);
    Box->SetBoxExtent(FVector(10,100,200));
    Box->SetCollisionProfileName(TEXT("BlockAll"));
    Box->RegisterComponent();
    Wall->SetActorLocation(FVector(110,800,100));
    auto* WalledChild = SpawnChild(FVector(0,800,0));
    WalledChild->ReceiveImpact(FVector(45000,0,15000), WalledChild->GetActorLocation());
    for (int i=0; i<9; ++i) WalledChild->Tick(1.f/15.f);
    TestTrue(TEXT("低帧率撞飞不能穿过墙"), WalledChild->GetActorLocation().X <= 81.f);
    TestTrue(TEXT("墙前仍有实际击退"), WalledChild->GetActorLocation().X > 25.f);
    return true;
}
#endif
