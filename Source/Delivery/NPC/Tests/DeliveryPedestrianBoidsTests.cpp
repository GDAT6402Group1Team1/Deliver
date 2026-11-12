#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "NPC/DeliveryPedestrian.h"
#include "NPC/DeliveryPedestrianRoute.h"
#include "Components/BoxComponent.h"
#include "Components/SplineComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/WorldSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryPedestrianBoidsTest, "Delivery.NPC.Boids",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryPedestrianBoidsTest::RunTest(const FString&)
{
    const auto Settings = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Settings);
    if (!TestNotNull(TEXT("测试世界"), World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); };
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    auto* Route = World->SpawnActor<ADeliveryPedestrianRoute>();
    Route->SetActorTickEnabled(false); Route->bRoamInArea = false; Route->bUseBoids = true;
    Route->bPauseWhileWalking = false; Route->bAllowHandoff = false; Route->bBothDirections = false;
    auto* Spline = Route->GetSpline();
    Spline->SetLocationAtSplinePoint(1, FVector(5000,0,0), ESplineCoordinateSpace::World);
    auto* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Characters/NPC/SK_NpcBoy.SK_NpcBoy"));
    if (!TestNotNull(TEXT("正式小学生模型"), Mesh)) return false;
    FDeliveryPedestrianKind Kind; Kind.Mesh = Mesh; Kind.Weight = 1.f; Kind.HeightScale = .6f;
    Route->Kinds.Add(Kind);
    auto* A = Route->SpawnOnePedestrian(true); auto* B = Route->SpawnOnePedestrian(true);
    if (!TestNotNull(TEXT("第一个学生"), A) || !TestNotNull(TEXT("第二个学生"), B)) return false;
    for (auto* Child : {A,B})
    {
        Child->BeginWalk(Route, Mesh, nullptr, 500.f, true, 130.f, 150.f);
        Child->bYieldToObstacles = false;
        Child->SetActorLocation(FVector(500,0,0));
    }
    for (int Frame=0; Frame<90; ++Frame)
        for (auto* Child : {A,B})
        {
            const FVector Before = Child->GetActorLocation(); Child->Tick(1.f/30.f);
            TestTrue(TEXT("群体横移不越过路线走廊"), FMath::Abs(Child->GetActorLocation().Y) <= Route->BoidsCorridorHalfWidth + .1f);
            TestTrue(TEXT("转向不会瞬移"), FVector::Dist2D(Before, Child->GetActorLocation()) < 18.f);
        }
    TestTrue(TEXT("完全重叠的学生主动分开"), FVector::Dist2D(A->GetActorLocation(), B->GetActorLocation()) > 45.f);
    TestTrue(TEXT("分离后仍朝终点前进"), A->GetDistanceAlongRoute() > 700.f && B->GetDistanceAlongRoute() > 700.f);
    TestTrue(TEXT("学生确实有侧向分离"), FMath::Abs(A->GetActorLocation().Y - B->GetActorLocation().Y) > 20.f);
    auto* Wall = World->SpawnActor<AActor>(); auto* Box = NewObject<UBoxComponent>(Wall);
    Wall->SetRootComponent(Box); Box->SetBoxExtent(FVector(10,150,100));
    Box->SetCollisionProfileName(TEXT("BlockAll")); Box->RegisterComponent();
    const float WallX = FMath::Max(A->GetActorLocation().X, B->GetActorLocation().X) + 100.f;
    Wall->SetActorLocation(FVector(WallX,0,70));
    for (int Frame=0; Frame<45; ++Frame) { A->Tick(1.f/15.f); B->Tick(1.f/15.f); }
    TestTrue(TEXT("15 FPS 群体移动不穿墙"), A->GetActorLocation().X < WallX - 10.f && B->GetActorLocation().X < WallX - 10.f);
    const FVector HitOrigin = A->GetActorLocation();
    A->ReceiveImpact(FVector(0,45000,15000), HitOrigin);
    for (int Frame=0; Frame<10; ++Frame) { A->Tick(.06f); B->Tick(.06f); }
    TestTrue(TEXT("受击时脱离走廊，不被 Boids 拉回"), A->GetActorLocation().Y > HitOrigin.Y + 100.f);
    TestTrue(TEXT("受击时保持倒地"), A->IsKnockedDown());

    auto* Crosswalk = World->SpawnActor<ADeliveryPedestrianRoute>();
    Crosswalk->SetActorTickEnabled(false);
    Crosswalk->bRoamInArea = false;
    Crosswalk->bPauseWhileWalking = false;
    Crosswalk->bAllowHandoff = false;
    Crosswalk->bBothDirections = false;
    Crosswalk->CrosswalkActor = World->SpawnActor<AActor>();
    Crosswalk->Kinds.Add(Kind);
    Crosswalk->GetSpline()->SetLocationAtSplinePoint(0, FVector(0, 2000, 0), ESplineCoordinateSpace::World);
    Crosswalk->GetSpline()->SetLocationAtSplinePoint(1, FVector(850, 2000, 0), ESplineCoordinateSpace::World);
    const FName LightTag(TEXT("ClaudeGenCrosswalkLight"));
    Wall->SetActorLocation(FVector(535, 2000, 70));
    for (const bool bBoids : {false, true})
    {
        Crosswalk->bUseBoids = bBoids;
        auto* Child = Crosswalk->SpawnOnePedestrian(true);
        if (!TestNotNull(TEXT("过街测试行人"), Child)) return false;
        Child->BeginWalk(Crosswalk, Mesh, nullptr, 500.f, true, 130.f, 150.f);
        Child->bYieldToObstacles = !bBoids;
        Wall->Tags.AddUnique(LightTag);
        TestFalse(TEXT("过街行人前向探测跳过红绿灯"), Child->IsBlockedAhead());
        Child->Tick(.1f);
        TestTrue(TEXT("红绿灯不会挡住过街推进"), Child->GetDistanceAlongRoute() > 500.f);

        Wall->Tags.Remove(LightTag);
        Child->BeginWalk(Crosswalk, Mesh, nullptr, 500.f, true, 130.f, 150.f);
        for (int Frame = 0; Frame < 29; ++Frame) Child->Tick(.1f);
        TestEqual(TEXT("过街行人阻挡未满三秒时等待"), Child->GetDistanceAlongRoute(), 500.f, .1f);
        for (int Frame = 0; Frame < 4; ++Frame) Child->Tick(.1f);
        TestTrue(TEXT("过街行人连续卡住三秒后恢复前进"), Child->GetDistanceAlongRoute() > 500.f);
        for (int Frame = 0; Frame < 60 && IsValid(Child) && !Child->IsActorBeingDestroyed(); ++Frame)
        {
            Child->Tick(.1f);
        }
        TestTrue(TEXT("恢复前进后能抵达终点退场"), !IsValid(Child) || Child->IsActorBeingDestroyed());
    }
    return true;
}
#endif
