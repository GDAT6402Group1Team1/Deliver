#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Characters/DeliveryNpcAIController.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationData.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcObstacleTraversalTest,"Delivery.AI.Navigation.ObstacleTraversal",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryNpcObstacleTraversalTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool bWasEnabled=Settings->bEnabled;
    Settings->bEnabled=false;
    ON_SCOPE_EXIT { Settings->bEnabled=bWasEnabled; };
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    if (!TestNotNull(TEXT("创建独立的物理碰撞测试世界"),World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit);
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
    };
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();

    const auto MakeBox=[&](const FVector& Center,const FVector& Extent)
    {
        auto* Actor=World->SpawnActor<AActor>();
        auto* Box=NewObject<UBoxComponent>(Actor);
        Actor->SetRootComponent(Box);
        Box->SetBoxExtent(Extent);
        Box->SetCollisionProfileName(TEXT("BlockAll"));
        Box->RegisterComponent();
        Actor->SetActorLocation(Center);
        return Box;
    };
    MakeBox(FVector(280,0,-25),FVector(650,200,25));
    auto* Barrier=MakeBox(FVector(104,0,25),FVector(12,90,25));
    auto* Npc=World->SpawnActor<ADeliveryNpcCharacter>(FVector(0,0,91),FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("创建走真实胶囊的居民"),Npc)) return false;
    if (!Npc->GetController()) Npc->SpawnDefaultController();
    auto* AI=Cast<ADeliveryNpcAIController>(Npc->GetController());
    auto* Path=AI?AI->GetPathFollowingComponent():nullptr;
    if (!TestNotNull(TEXT("使用居民原来的行为控制器和路径组件"),Path)) return false;
    auto* Movement=Npc->GetCharacterMovement();
    const auto Step=[&]
    {
        ++GFrameCounter;
        World->Tick(LEVELTICK_All,.025f);
    };
    Step();
    if (!TestTrue(TEXT("起跳前真实地面已支撑行人胶囊"),Movement->IsMovingOnGround() && Movement->CurrentFloor.IsWalkableFloor())) return false;

    const FVector LaunchVelocity(200,0,420);
    constexpr double FlightTime=.84;
    TestTrue(TEXT("有实体低墙时全身胶囊能够越过障碍"),AI->HasClearObstacleJumpArc(*Npc,LaunchVelocity,FlightTime));
    auto* Ceiling=MakeBox(FVector(80,0,223),FVector(60,100,10));
    TestFalse(TEXT("相同跳弧在真实低天花板下无头顶净空"),AI->HasClearObstacleJumpArc(*Npc,LaunchVelocity,FlightTime));
    Ceiling->GetOwner()->Destroy();
    Barrier->SetBoxExtent(FVector(12,90,70));
    Barrier->GetOwner()->SetActorLocation(FVector(104,0,70));
    TestFalse(TEXT("相同跳弧无法越过过高的实体墙"),AI->HasClearObstacleJumpArc(*Npc,LaunchVelocity,FlightTime));
    Barrier->SetBoxExtent(FVector(12,90,25));
    Barrier->GetOwner()->SetActorLocation(FVector(104,0,25));

    const FVector Feet=FVector::ZeroVector;
    const FVector Edge(22,0,0);
    FNavigationPath Waypoints(TArray<FVector>{Feet,Edge,FVector(100,0,45),FVector(165,0,45),FVector(180,0,45)});
    bool bStopsBeforeCustomLink=false;
    const auto Ahead=[&](const FNavLinkId& WalkThroughLink=FNavLinkId::Invalid)
    {
        return AI->ExtendObstacleRouteTarget(Waypoints,1,Feet,FVector::ForwardVector,Edge,WalkThroughLink,bStopsBeforeCustomLink);
    };
    TestTrue(TEXT("路点贴障时最多前瞻两个同向点"),Ahead().Equals(FVector(165,0,45)) && !bStopsBeforeCustomLink);
    auto& RoutePoints=Waypoints.GetPathPoints();
    RoutePoints[3].Location=FVector(600,0,45);
    TestTrue(TEXT("远处同向路段只用于近距离跨越"),Ahead().Equals(FVector(260,0,45)) && !bStopsBeforeCustomLink);
    RoutePoints[2].Location=FVector(22,100,0);
    TestTrue(TEXT("转角后的路点不能拉长原方向的跳跃"),Ahead().Equals(Edge) && !bStopsBeforeCustomLink);
    RoutePoints[2].Location=FVector(100,0,45);
    RoutePoints[2].CustomNavLinkId=FNavLinkId(123);
    TestTrue(TEXT("前瞻和投影都要约束在下一条自定义导航连接之前"),Ahead().Equals(Edge) && bStopsBeforeCustomLink);
    TestTrue(TEXT("仅已经核准的行人门洞可以沿原路线前瞻落点"),
        Ahead(FNavLinkId(123)).Equals(FVector(260,0,45)) && !bStopsBeforeCustomLink);
    RoutePoints[1].CustomNavLinkId=FNavLinkId(124);
    TestTrue(TEXT("当前路点是其他导航连接起点时门洞许可不放行"),
        Ahead(FNavLinkId(123)).Equals(Edge) && bStopsBeforeCustomLink);
    RoutePoints[2].CustomNavLinkId=FNavLinkId::Invalid;
    TestTrue(TEXT("门洞入口正是当前路点时也可前瞻同向的内侧落点"),
        Ahead(FNavLinkId(124)).Equals(FVector(260,0,45)) && !bStopsBeforeCustomLink);

    const auto RequestWalk=[&](const FVector& Goal)
    {
        FAIMoveRequest Move(Goal);
        Move.SetAcceptanceRadius(15.f);
        const auto Route=MakeShared<FNavigationPath,ESPMode::ThreadSafe>(TArray<FVector>{Npc->GetNavAgentLocation(),Goal});
        return Path->RequestMove(Move,Route);
    };
    const auto StartAcceptedArc=[&](FAIRequestID Request)
    {
        Path->PauseMove(Request,EPathFollowingVelocityMode::Keep);
        AI->ObstacleTraversal.Pawn=Npc;
        AI->ObstacleTraversal.Request=Request;
        AI->ObstacleTraversal.LaunchVelocity=LaunchVelocity;
        AI->ObstacleTraversal.LandingFeet=Npc->GetNavAgentLocation()+FVector(LaunchVelocity.X*FlightTime,0,0);
        AI->ObstacleTraversal.ExpiresAt=World->GetTimeSeconds()+2.;
        Npc->LaunchCharacter(LaunchVelocity,true,true);
    };
    const FAIRequestID First=RequestWalk(FVector(520,0,0));
    if (!TestTrue(TEXT("当前路线获得实际导航请求编号"),First.IsValid())) return false;
    const FVector BarrierCenter=Barrier->GetComponentLocation();
    AI->ObstacleRetreatAttempts.Add({First,Barrier,Feet,BarrierCenter});
    TestTrue(TEXT("同一路线退步后靠近同一墙仍保留尝试记录"),AI->HasTriedObstacleRetreat(First,*Barrier,FVector(55,0,0)));
    TestFalse(TEXT("路线编号已变时旧障碍记录失效"),AI->HasTriedObstacleRetreat(FAIRequestID::InvalidRequest,*Barrier,Feet));
    TestFalse(TEXT("相距较远的跨越由当前位置重新判断"),AI->HasTriedObstacleRetreat(First,*Barrier,FVector(150,0,0)));
    Barrier->GetOwner()->SetActorLocation(BarrierCenter+FVector(0,55,0));
    TestFalse(TEXT("实物移动后同一墙的旧站位记录失效"),AI->HasTriedObstacleRetreat(First,*Barrier,Feet));
    Barrier->GetOwner()->SetActorLocation(BarrierCenter);
    AI->ObstacleRetreatAttempts.Reset();
    StartAcceptedArc(First);
    TestTrue(TEXT("跃起到落地之前保留当前路线的暂停所有权"),AI->IsTraversingObstacle(First));
    bool bSawFalling=false;
    for (int32 Index=0;Index<48 && AI->IsTraversingObstacle(First);++Index)
    {
        Step();
        bSawFalling|=Movement->IsFalling();
    }
    TestTrue(TEXT("单次 Launch 经过真实下落并在矮墙另一边落地"),bSawFalling && Movement->IsMovingOnGround() &&
        Movement->CurrentFloor.IsWalkableFloor() && Npc->GetActorLocation().X>140.);
    TestTrue(TEXT("落地后恢复同一个导航请求"),!AI->IsTraversingObstacle(First) && Path->GetCurrentRequestId()==First &&
        Path->GetStatus()==EPathFollowingStatus::Moving);
    Path->AbortMove(*AI,FPathFollowingResultFlags::UserAbort,First,EPathFollowingVelocityMode::Keep);

    const FAIRequestID Leaving=RequestWalk(FVector(540,0,0));
    if (!TestTrue(TEXT("新一轮使用另一个实际移动编号"),Leaving.IsValid() && Leaving!=First)) return false;
    StartAcceptedArc(Leaving);
    Path->AbortMove(*AI,FPathFollowingResultFlags::UserAbort,Leaving,EPathFollowingVelocityMode::Keep);
    const FAIRequestID NewMove=RequestWalk(FVector(490,80,0));
    AI->UpdateObstacleTraversal();
    TestTrue(TEXT("取消旧路线只撤回旧起跳并保留新请求"),NewMove.IsValid() && NewMove!=Leaving &&
        Path->GetCurrentRequestId()==NewMove && Path->GetStatus()==EPathFollowingStatus::Moving &&
        Movement->PendingLaunchVelocity.IsZero() && !AI->IsTraversingObstacle(Leaving));
    return true;
}
#endif
