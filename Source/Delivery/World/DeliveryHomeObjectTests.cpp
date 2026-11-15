#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "World/DeliveryHome.h"
#include "World/DeliveryHomeObjectComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryHomeInterruptedScaleTest,"Delivery.AI.Companion.InterruptedFurnitureScale",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryHomeInterruptedScaleTest::RunTest(const FString&)
{
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(true).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    auto* Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("隔离的房屋物理世界"),World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit);
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
    };
    if (!TestNotNull(TEXT("原尺寸检查使用真实刚体网格"),Cube)) return false;
    auto* Home=World->SpawnActor<ADeliveryHome>();
    if (!TestNotNull(TEXT("共享房屋"),Home)) return false;
    Home->bClaimed=true;

    const auto MakeBox=[World](const FVector& Location,const FVector& Extent,bool bSimulates)
    {
        auto* Actor=World->SpawnActor<AActor>();
        auto* Box=NewObject<UBoxComponent>(Actor);
        Actor->SetRootComponent(Box);
        Actor->AddInstanceComponent(Box);
        Box->SetBoxExtent(Extent);
        Box->SetCollisionProfileName(bSimulates?TEXT("PhysicsActor"):TEXT("BlockAll"));
        Box->RegisterComponent();
        Actor->SetActorLocation(Location);
        Box->SetEnableGravity(false);
        Box->SetSimulatePhysics(bSimulates);
        return Box;
    };
    const auto RegisterFurniture=[Home](AActor* Actor)
    {
        auto* Metadata=NewObject<UDeliveryHomeObjectComponent>(Actor);
        Actor->AddInstanceComponent(Metadata);
        Metadata->Home=Home;
        Metadata->ObjectId=FGuid::NewGuid();
        Metadata->DisplayName=FText::FromString(Actor->GetName());
        Home->Furnishings.Add(Actor);
        Metadata->RegisterComponent();
        return Metadata;
    };
    MakeBox(FVector(0,0,-15),FVector(800,800,15),false);
    auto* TableBody=MakeBox(FVector(0,0,25),FVector(150,150,25),true);
    auto* Table=RegisterFurniture(TableBody->GetOwner());
    const auto MakeCompactProp=[World,Cube,&RegisterFurniture](const FVector& Location)
    {
        auto* Actor=World->SpawnActor<AActor>();
        auto* Body=NewObject<UStaticMeshComponent>(Actor);
        Actor->SetRootComponent(Body);
        Actor->AddInstanceComponent(Body);
        Body->SetStaticMesh(Cube);
        Body->SetCollisionProfileName(TEXT("PhysicsActor"));
        Body->SetIsReplicated(true);
        Body->RegisterComponent();
        Actor->SetActorLocation(Location);
        Actor->SetActorScale3D(FVector(.2));
        Body->SetEnableGravity(false);
        Body->SetSimulatePhysics(true);
        auto* Grab=NewObject<UDeliveryGrabbableComponent>(Actor);
        Actor->AddInstanceComponent(Grab);
        Grab->RegisterComponent();
        auto* Metadata=RegisterFurniture(Actor);
        Metadata->AllowedUses.Add(EDeliveryHomeOperation::TakeProp);
        // 从中断后的小物件开始，缩放快照与实际模型仍须走正常恢复检查
        Metadata->ScaleRoot=Body;
        Metadata->CarryScaleState.OriginalActorScale=FVector(1.2,1,1);
        Metadata->SetCarryScalePhase(EDeliveryHomeCarryScalePhase::CarriedSmall);
        Metadata->QueueScaleRestore();
        return Metadata;
    };
    auto* OnTable=MakeCompactProp(FVector(0,0,60));
    auto* NearWall=MakeCompactProp(FVector(400,0,10));
    MakeBox(FVector(448,0,100),FVector(10,150,100),false);

    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    if (!TestTrue(TEXT("正式共享房屋和动态桌面已登记"),ADeliveryHome::FindSharedHome(World)==Home &&
        Table->IsStablePlacementSurface() && TableBody->IsSimulatingPhysics())) return false;
    const FTransform Target(FQuat::Identity,FVector(0,0,100),FVector(1.2,1,1));
    FName Reason;
    TestFalse(TEXT("普通查询仍不接受未点名的动态支撑"),OnTable->CanRestoreScaleAt(Target,Reason));
    TestTrue(TEXT("指定的真实桌面足以支撑完整尺寸"),OnTable->CanRestoreScaleAt(Target,Reason,Table));

    Table->Home=nullptr;
    OnTable->UpdateScaleRestore();
    TestTrue(TEXT("相同形状的无归属动态物件不能触发恢复"),OnTable->IsCarryScaleActive());
    Table->Home=Home;
    TableBody->SetPhysicsLinearVelocity(FVector(30,0,0));
    OnTable->UpdateScaleRestore();
    TestTrue(TEXT("仍在移动的登记家具不能触发恢复"),OnTable->IsCarryScaleActive());
    TableBody->SetPhysicsLinearVelocity(FVector::ZeroVector);
    OnTable->UpdateScaleRestore();
    TestTrue(TEXT("松手后同一实物在静止桌面原位恢复精确非均匀原尺度"),
        !OnTable->IsCarryScaleActive() && OnTable->GetOwner()->GetActorScale3D().Equals(FVector(1.2,1,1)) &&
        OnTable->GetOwner()->GetActorLocation().Equals(FVector(0,0,100),1.));

    NearWall->UpdateScaleRestore();
    TestTrue(TEXT("原位被墙阻挡时等待，不强行放大"),NearWall->IsCarryScaleActive() &&
        NearWall->GetOwner()->GetActorLocation().Equals(FVector(400,0,10),1.) &&
        NearWall->NextScaleRestoreOffsetAt>World->GetTimeSeconds());
    NearWall->NextScaleRestoreOffsetAt=World->GetTimeSeconds();
    NearWall->UpdateScaleRestore();
    TestTrue(TEXT("附近同层净空足够才移向远离墙的一侧并恢复原尺度"),!NearWall->IsCarryScaleActive() &&
        NearWall->GetOwner()->GetActorScale3D().Equals(FVector(1.2,1,1)) &&
        NearWall->GetOwner()->GetActorLocation().Equals(FVector(376,0,50),1.));
    return true;
}
#endif
