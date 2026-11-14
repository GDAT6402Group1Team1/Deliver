#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ActorFactories/ActorFactory.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Characters/DeliveryNpcAIController.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "BrainComponent.h"
#include "Builders/CubeBuilder.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcSpacingTest,"Delivery.AI.StandingSpace",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryNpcSpacingTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool bOldEnabled=Settings->bEnabled;
    Settings->bEnabled=false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(true).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    if (!TestNotNull(TEXT("创建独立站位测试世界"),World))
    { Settings->bEnabled=bOldEnabled; return false; }
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit);
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        Settings->bEnabled=bOldEnabled;
    };
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
    if (!TestNotNull(TEXT("站位测试具有导航系统"),Nav)) return false;

    auto* Floor=World->SpawnActor<AActor>();
    auto* FloorMesh=NewObject<UStaticMeshComponent>(Floor);
    Floor->SetRootComponent(FloorMesh);
    FloorMesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
    FloorMesh->SetCollisionProfileName(TEXT("BlockAll"));
    FloorMesh->SetMobility(EComponentMobility::Static);
    FloorMesh->SetWorldScale3D(FVector(20,20,.4));
    Floor->SetActorLocation(FVector(0,0,-20));
    FloorMesh->RegisterComponent();
    auto* Bounds=World->SpawnActor<ANavMeshBoundsVolume>(FVector(0,0,150),FRotator::ZeroRotator);
    auto* Builder=NewObject<UCubeBuilder>();
    Builder->X=1800; Builder->Y=1800; Builder->Z=500;
    UActorFactory::CreateBrushForVolumeActor(Bounds,Builder);
    Nav->OnNavigationBoundsUpdated(Bounds);
    auto* NavData=Nav->GetDefaultNavDataInstance(FNavigationSystem::Create);
    if (!TestNotNull(TEXT("生成站位测试导航数据"),NavData)) return false;
    auto* Generation=FindFProperty<FEnumProperty>(ANavigationData::StaticClass(),TEXT("RuntimeGeneration"));
    if (!TestNotNull(TEXT("临时导航支持动态生成"),Generation)) return false;
    Generation->GetUnderlyingProperty()->SetIntPropertyValue(Generation->ContainerPtrToValuePtr<void>(NavData),
        static_cast<int64>(ERuntimeGenerationType::Dynamic));
    FNavigationSystem::AddNavigationSystemToWorld(*World,FNavigationSystemRunMode::GameMode);
    FNavigationSystem::UpdateComponentData(*FloorMesh);
    ++GFrameCounter; World->Tick(LEVELTICK_All,.02f);
    Nav->Build();
    FNavLocation Projected;
    if (!TestTrue(TEXT("站位场地位于导航网格内"),Nav->ProjectPointToNavigation(FVector::ZeroVector,Projected,FVector(50,50,100)))) return false;

    auto* ManClass=LoadClass<ADeliveryNpcCharacter>(nullptr,TEXT("/Game/NPC/Residents/BP_ResidentLoud.BP_ResidentLoud_C"));
    auto* WomanClass=LoadClass<ADeliveryNpcCharacter>(nullptr,TEXT("/Game/NPC/Residents/BP_ResidentShrewd.BP_ResidentShrewd_C"));
    if (!TestNotNull(TEXT("正式男NPC蓝图"),ManClass) || !TestNotNull(TEXT("正式女NPC蓝图"),WomanClass)) return false;
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* A=World->SpawnActor<ADeliveryNpcCharacter>(ManClass,FVector(0,0,90),FRotator::ZeroRotator,Spawn);
    auto* B=World->SpawnActor<ADeliveryNpcCharacter>(WomanClass,FVector(400,0,90),FRotator(0,180,0),Spawn);
    if (!TestNotNull(TEXT("创建男NPC"),A) || !TestNotNull(TEXT("创建女NPC"),B)) return false;
    auto* AController=Cast<ADeliveryNpcAIController>(A->GetController());
    auto* BController=Cast<ADeliveryNpcAIController>(B->GetController());
    if (!TestNotNull(TEXT("男NPC控制器"),AController) || !TestNotNull(TEXT("女NPC控制器"),BController)) return false;
    for (auto* Npc:{A,B})
    {
        Npc->SetActorTickEnabled(false);
        auto* Controller=CastChecked<ADeliveryNpcAIController>(Npc->GetController());
        Controller->SetActorTickEnabled(false);
        if (Controller->BrainComponent) Controller->BrainComponent->StopLogic(TEXT("IsolatedStandingTest"));
        Controller->MinStandingGap=80;
        Controller->ComfortableStandingGap=120;
    }
    // 只手动推进控制器，角色移动仍由世界更新，不调用后台或伪造移动结果
    const auto Advance=[&](float Seconds,bool bUpdateA=true,bool bUpdateB=true)
    {
        for (int32 Index=0;Index<FMath::CeilToInt(Seconds/.02f);++Index)
        {
            ++GFrameCounter;
            World->Tick(LEVELTICK_All,.02f);
            if (bUpdateA) AController->Tick(.02f);
            if (bUpdateB) BController->Tick(.02f);
        }
    };
    const float RadiusSum=A->GetSimpleCollisionRadius()+B->GetSimpleCollisionRadius();
    const auto BodyGap=[&]() { return FVector::Dist2D(A->GetActorLocation(),B->GetActorLocation())-RadiusSum; };
    const auto Place=[&](float Distance)
    {
        for (auto* Npc:{A,B})
        {
            auto* Controller=CastChecked<ADeliveryNpcAIController>(Npc->GetController());
            Npc->SetActorHiddenInGame(true);
            Controller->Tick(0);
            Controller->StopMovement();
            Npc->Agent->CancelPlan(TEXT("NextStandingTest"));
            Npc->Combat->CancelStrike();
            Npc->Combat->SetDeliberateProne(false);
            Npc->GetCharacterMovement()->StopMovementImmediately();
        }
        A->SetActorLocation(FVector(0,0,A->GetSimpleCollisionHalfHeight()+2));
        B->SetActorLocation(FVector(Distance,0,B->GetSimpleCollisionHalfHeight()+2));
        A->SetActorRotation(FRotator::ZeroRotator);
        B->SetActorRotation(FRotator(0,180,0));
        // 等上一次检查间隔结束，并让胶囊正常落地
        Advance(1.6f,false,false);
        A->SetActorHiddenInGame(false);
        B->SetActorHiddenInGame(false);
    };

    Place(RadiusSum+30);
    const FVector FirstA=A->GetActorLocation(),FirstB=B->GetActorLocation();
    Advance(3);
    TestTrue(TEXT("两人靠近时实际走开"),FVector::Dist2D(FirstA,A->GetActorLocation())+FVector::Dist2D(FirstB,B->GetActorLocation())>35);
    TestTrue(TEXT("停下时身体之间留出间隔"),BodyGap()>=AController->MinStandingGap+20);
    const FVector StandingA=A->GetActorLocation(),StandingB=B->GetActorLocation();
    Advance(4);
    TestTrue(TEXT("间隔足够后男NPC不持续漂移"),FVector::Dist2D(StandingA,A->GetActorLocation())<5);
    TestTrue(TEXT("间隔足够后女NPC不持续漂移"),FVector::Dist2D(StandingB,B->GetActorLocation())<5);
    TestTrue(TEXT("双方最终停止导航"),AController->GetMoveStatus()==EPathFollowingStatus::Idle && BController->GetMoveStatus()==EPathFollowingStatus::Idle);

    Place(RadiusSum+30);
    TArray<AActor*> Walls;
    const auto AddWall=[&](const FVector& Offset,const FVector& Extent)
    {
        auto* Wall=World->SpawnActor<AActor>();
        auto* Box=NewObject<UBoxComponent>(Wall);
        Wall->SetRootComponent(Box);
        Box->SetBoxExtent(Extent);
        Box->SetCollisionProfileName(TEXT("BlockAll"));
        Box->SetCanEverAffectNavigation(false);
        Wall->SetActorLocation(A->GetActorLocation()+Offset);
        Box->RegisterComponent();
        Walls.Add(Wall);
    };
    // 导航仍然畅通，实际墙体封住后方和两侧，必须由胶囊检查拒绝
    const float WallOffset=A->GetSimpleCollisionRadius()+12;
    AddWall(FVector(-WallOffset,0,0),FVector(8,250,130));
    AddWall(FVector(-100,WallOffset,0),FVector(250,8,130));
    AddWall(FVector(-100,-WallOffset,0),FVector(250,8,130));
    FVector BlockedDestination;
    TestFalse(TEXT("三面受阻时没有可用站位落点"),A->FindStandingSpace(B,AController->ComfortableStandingGap,BlockedDestination));
    const FVector BeforeWall=A->GetActorLocation();
    Advance(2,true,false);
    TestTrue(TEXT("避让不会穿过墙体或直接改位置"),FVector::Dist2D(BeforeWall,A->GetActorLocation())<5);
    TestTrue(TEXT("没有安全落点时保持停止"),AController->GetMoveStatus()==EPathFollowingStatus::Idle);
    for (auto* Wall:Walls) Wall->Destroy();

    Place(RadiusSum+30);
    Advance(.1f,true,false);
    auto* Path=AController->GetPathFollowingComponent();
    const FAIRequestID SpacingId=Path->GetCurrentRequestId();
    if (!TestTrue(TEXT("准备正在执行的站位移动"),SpacingId.IsValid() && AController->GetMoveStatus()==EPathFollowingStatus::Moving)) return false;
    FAIMoveRequest NewMove(A->GetNavAgentLocation()+FVector(0,700,0));
    NewMove.SetAcceptanceRadius(8);
    NewMove.SetReachTestIncludesAgentRadius(false);
    NewMove.SetReachTestIncludesGoalRadius(false);
    NewMove.SetAllowPartialPath(false);
    const auto Replacement=AController->MoveTo(NewMove);
    if (!TestTrue(TEXT("新的移动请求已替换避让"),Replacement.Code==EPathFollowingRequestResult::RequestSuccessful && Replacement.MoveId!=SpacingId)) return false;
    AController->Tick(.02f);
    TestTrue(TEXT("旧避让结束时保留新请求编号"),Path->GetCurrentRequestId()==Replacement.MoveId);
    TestTrue(TEXT("清除旧避让不会停止新导航"),AController->GetMoveStatus()==EPathFollowingStatus::Moving);
    const FVector BeforeNewMove=A->GetActorLocation();
    Advance(1.4f,true,false);
    TestTrue(TEXT("下次站位检查仍不抢新导航"),Path->GetCurrentRequestId()==Replacement.MoveId && AController->GetMoveStatus()==EPathFollowingStatus::Moving);
    TestTrue(TEXT("新导航继续实际移动"),FVector::Dist2D(BeforeNewMove,A->GetActorLocation())>100);

    Place(RadiusSum+30);
    if (!TestTrue(TEXT("准备倒地NPC"),A->Combat->SetDeliberateProne(true))) return false;
    const FVector DownPosition=A->GetActorLocation();
    Advance(.6f,true,false);
    TestFalse(TEXT("倒地期间不能调整站位"),A->CanAdjustStandingPosition());
    TestTrue(TEXT("倒地期间不发起导航"),AController->GetMoveStatus()==EPathFollowingStatus::Idle);
    TestTrue(TEXT("倒地期间不被站位逻辑拖走"),FVector::Dist2D(DownPosition,A->GetActorLocation())<5);

    Place(A->Combat->GetStrikeApproachDistance(B)-.5f);
    const FGuid StrikeId=FGuid::NewGuid();
    if (!TestTrue(TEXT("在真实近战距离内启动挥拳"),A->Combat->StartStrike(B,StrikeId))) return false;
    const FVector PunchPosition=A->GetActorLocation();
    TestFalse(TEXT("挥拳期间不能调整站位"),A->CanAdjustStandingPosition());
    Advance(.12f,true,false);
    TestTrue(TEXT("挥拳期间不发起站位导航"),AController->GetMoveStatus()==EPathFollowingStatus::Idle);
    TestTrue(TEXT("挥拳期间保持原位置"),FVector::Dist2D(PunchPosition,A->GetActorLocation())<5);
    A->Combat->CancelStrike();
    Advance(3,true,false);
    TestTrue(TEXT("停止挥拳后恢复正常站位调整"),BodyGap()>=AController->MinStandingGap);
    TestFalse(TEXT("站位检查不会开启模型请求"),A->Agent->IsDecisionPending() || B->Agent->IsDecisionPending());
    return true;
}
#endif
