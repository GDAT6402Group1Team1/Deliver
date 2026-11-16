#include "Carry/Tests/DeliveryPackageTransferProbe.h"
#include "Character/DeliveryCharacter.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/Items/DeliveryPackageItem.h"

void UDeliveryPackageTransferProbe::OnInventoryChanged()
{
    if (bAttempted || !ThirdParty.IsValid() || !Package.IsValid() || !Receiver.IsValid()) return;
    bAttempted = true;
    bThirdPartyReceived = ThirdParty->GetInventoryComponent()->TryPickup(Package.Get());
    Receiver->SetActorLocation(FVector(1000, 0, 88), false, nullptr, ETeleportType::TeleportPhysics);
}

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Carry/DeliveryPackageCarrierInterface.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Game/DeliveryGameState.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshCompiler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryPackageTransferTest, "Delivery.Carry.PackageTransfer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryPackageTransferTest::RunTest(const FString&)
{
    auto* Settings = GetMutableDefault<UDeliveryAiSettings>();
    const bool bAiEnabled = Settings->bEnabled;
    Settings->bEnabled = false;
    const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit);
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        Settings->bEnabled = bAiEnabled;
    };
    auto* State = World->SpawnActor<ADeliveryGameState>();
    World->SetGameState(State);
    auto* Tasks = State->GetTaskManager();
    auto* Task = NewObject<UDeliveryTaskDefinition>(State);
    Task->TaskId = TEXT("PackageTransfer_Test");
    Tasks->TaskDefinitions.Add(Task);
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    auto* TaskState = Tasks->FindState(Task);
    if (!TestNotNull(TEXT("真实任务管理器已登记测试任务"), TaskState)) return false;
    TaskState->Status = EDeliveryTaskStatus::InProgress;
    TaskState->bTimerStarted = true;
    TaskState->StartServerTime = -17.f;
    const float OriginalStart = TaskState->StartServerTime;

    const auto MakePlayer = [&](FName Name, const FVector& Location)
    {
        FActorSpawnParameters Spawn;
        Spawn.Name = Name;
        Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        // 此夹具只检查交接规则，原生角色启动时会报告没有物理资产
        AddExpectedError(FString::Printf(TEXT("%s: Skeletal mesh has no Physics Asset."), *Name.ToString()),
            EAutomationExpectedErrorFlags::Contains, 1);
        auto* Character = World->SpawnActor<ADeliveryCharacter>(Location, FRotator::ZeroRotator, Spawn);
        Character->GetActiveRagdoll()->StopRagdoll();
        Character->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
        Character->SetPlayerState(World->SpawnActor<APlayerState>());
        return Character;
    };
    auto* Owner = MakePlayer(TEXT("PackageTransferOwner"), FVector(0, 0, 88));
    auto* Stranger = MakePlayer(TEXT("PackageTransferStranger"), FVector(0, 400, 88));
    auto* Npc = World->SpawnActor<ADeliveryCompanionCharacter>(FVector(150, 0, 88), FRotator::ZeroRotator);
    // 此处从结识后的服务器状态开始，扶起按键与视角仍由实机验收覆盖
    Npc->Encounter.EncounterStartedAt = 0;
    Npc->Encounter.Phase = EDeliveryCompanionEncounterPhase::Joined;
    Npc->Companion->OwnerPlayerState = Owner->GetPlayerState();
    Npc->Companion->Mode = EDeliveryCompanionMode::Follow;
    Npc->OnRep_Encounter();
    ADeliveryPackageItem* Package = World->SpawnActor<ADeliveryTestPackage>(FVector(0, 0, 150), FRotator::ZeroRotator);
    auto* Item = Package->GetDeliveryItemComponent();
    Item->OwningTask = Task;
    if (!TestTrue(TEXT("进行中任务的原实物可以由玩家拾回"), Owner->TryReceiveTaskPackage(Package))) return false;
    FName Reason;
    TestTrue(TEXT("同一玩家身份允许把原包裹交给同伴"), DeliveryPackageTransfer::TryTransfer(Owner, Npc, Reason));
    TestEqual(TEXT("同伴取得同一个实物指针"), Npc->GetCarriedTaskPackage(), Package);
    TestEqual(TEXT("实物所有者与同伴持件一致"), Package->GetOwner(), static_cast<AActor*>(Npc));
    TestNull(TEXT("成功交接后原手持槽已释放"), Owner->GetCarriedTaskPackage());
    TestEqual(TEXT("换手仍沿用动态任务身份"), Item->OwningTask.Get(), Task);

    auto* Held = World->SpawnActor<ADeliveryHandheldItem>(FVector(0, 500, 150), FRotator::ZeroRotator);
    TestTrue(TEXT("主人先拿普通物品"), Owner->GetInventoryComponent()->TryPickup(Held));
    TestFalse(TEXT("交还不能自动丢掉主人原手物"), DeliveryPackageTransfer::TryTransfer(Npc, Owner, Reason));
    TestEqual(TEXT("手不空时包裹仍由同伴持有"), Npc->GetCarriedTaskPackage(), Package);
    TestEqual(TEXT("原手物保持不变"), Owner->GetInventoryComponent()->GetHeldItem(), Held);
    Owner->GetInventoryComponent()->ConsumeHeldItem(Held);
    Held->Destroy();
    TestTrue(TEXT("腾出手后可交还原实物"), DeliveryPackageTransfer::TryTransfer(Npc, Owner, Reason));
    TestEqual(TEXT("交还后的实物所有者是主人"), Package->GetOwner(), static_cast<AActor*>(Owner));
    TestFalse(TEXT("不同 PlayerState 不得转移同伴包裹"), DeliveryPackageTransfer::TryTransfer(Owner, Stranger, Reason));
    TestEqual(TEXT("身份拒绝发生在释放之前"), Owner->GetCarriedTaskPackage(), Package);

    auto* Probe = NewObject<UDeliveryPackageTransferProbe>(Owner);
    Probe->ThirdParty = Stranger;
    Probe->Package = Package;
    Probe->Receiver = Npc;
    Owner->GetInventoryComponent()->OnInventoryChanged.AddDynamic(Probe, &UDeliveryPackageTransferProbe::OnInventoryChanged);
    TestFalse(TEXT("释放回调中目标远移必须取消并回滚"), DeliveryPackageTransfer::TryTransfer(Owner, Npc, Reason));
    Owner->GetInventoryComponent()->OnInventoryChanged.RemoveDynamic(Probe, &UDeliveryPackageTransferProbe::OnInventoryChanged);
    TestTrue(TEXT("夹具实际经过同步背包通知"), Probe->bAttempted);
    TestFalse(TEXT("预约期间第三方不能抢走临时无主的实物"), Probe->bThirdPartyReceived);
    TestEqual(TEXT("回调后重新核验距离"), Reason, FName(TEXT("OutOfRange")));
    TestEqual(TEXT("失败回滚恢复主人原包裹"), Owner->GetCarriedTaskPackage(), Package);
    TestEqual(TEXT("失败回滚的实物所有者一致"), Package->GetOwner(), static_cast<AActor*>(Owner));
    TestNull(TEXT("回滚不遗留同伴持件"), Npc->GetCarriedTaskPackage());
    TestFalse(TEXT("返回后释放本次预约"), Item->IsTransferInProgress());

    Npc->SetActorLocation(FVector(150, 0, 88), false, nullptr, ETeleportType::TeleportPhysics);
    TestTrue(TEXT("回滚后同一实物可再次正常交接"), DeliveryPackageTransfer::TryTransfer(Owner, Npc, Reason));
    TestTrue(TEXT("主人可以取消当前安排"), Npc->Companion->ApplyCommand(
        EDeliveryNpcAction::CancelCompanionTask, Owner->GetPlayerState(), Npc->Companion->GetCommandRevision()));
    TestTrue(TEXT("取消安排保留实际包裹"), IsValid(Package) && Npc->GetCarriedTaskPackage() == Package);
    TestEqual(TEXT("换手与取消没有完成或重置任务"), Tasks->GetTaskStatus(Task), EDeliveryTaskStatus::InProgress);
    TestEqual(TEXT("所有换手与失败回滚沿用原计时"), Tasks->FindState(Task)->StartServerTime, OriginalStart);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryProductionPackagePhysicsTest,"Delivery.Carry.ProductionPackageGroundPhysics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryProductionPackagePhysicsTest::RunTest(const FString&)
{
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(true).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    if (!TestNotNull(TEXT("实际包裹刚体检查世界"),World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit);
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
    };
    auto* Floor=World->SpawnActor<AActor>();
    auto* Collision=NewObject<UBoxComponent>(Floor);
    Floor->SetRootComponent(Collision);
    Collision->SetBoxExtent(FVector(250,250,25));
    Collision->SetCollisionProfileName(TEXT("BlockAll"));
    Collision->RegisterComponent();
    Floor->SetActorLocation(FVector(0,0,-25));
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    UClass* PackageClass=LoadClass<ADeliveryPackageItem>(nullptr,TEXT("/Game/Blueprint/Item/BP_DeliveryPackage.BP_DeliveryPackage_C"));
    if (!TestNotNull(TEXT("加载正式外卖蓝图"),PackageClass)) return false;
    const auto* Defaults=PackageClass->GetDefaultObject<ADeliveryPackageItem>();
    UStaticMesh* Food=Defaults && Defaults->GetItemMesh()?Defaults->GetItemMesh()->GetStaticMesh().Get():nullptr;
    if (!TestTrue(TEXT("正式外卖继续使用原食盒网格"),Food && Food->GetPathName()==TEXT("/Game/model/food.food"))) return false;
    // 独立世界没有地图加载阶段，食盒完成异步编译后再按正式蓝图创建刚体
    UStaticMesh* Assets[]={Food};
    FStaticMeshCompilingManager::Get().FinishCompilation(Assets);
    if (!TestFalse(TEXT("生成外卖前正式网格已编译"),Food->IsCompiling())) return false;
    auto* Setup=Food->GetBodySetup();
    if (!TestTrue(TEXT("正式食盒同时保留复杂查询和实际可模拟的简单刚体碰撞"),
        Setup && Setup->GetCollisionTraceFlag()==CTF_UseSimpleAndComplex && Setup->AggGeom.GetElementCount()>0)) return false;
    Setup->CreatePhysicsMeshes();
    const FVector ReleasePoint(0,0,140);
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Package=World->SpawnActor<ADeliveryPackageItem>(PackageClass,ReleasePoint,FRotator::ZeroRotator,Spawn);
    if (!TestNotNull(TEXT("按正式蓝图生成实物外卖"),Package)) return false;
    auto* Mesh=Package->GetItemMesh();
    if (!TestTrue(TEXT("正式外卖的活实例使用同一已编译网格作为可移动根部"),Mesh && Mesh==Package->GetRootComponent() &&
        Mesh->GetStaticMesh()==Food && Mesh->IsRegistered() && Mesh->Mobility==EComponentMobility::Movable)) return false;
    Package->SetInventoryPresentation(false,true,nullptr);
    if (!TestTrue(TEXT("同一正式外卖收纳后隐藏并关闭实物碰撞"),Package->IsHidden() &&
        !Package->GetActorEnableCollision() && !Mesh->IsSimulatingPhysics())) return false;
    Package->DropFromInventory(ReleasePoint,FVector::ZeroVector);
    if (!TestTrue(TEXT("落地恢复正式外卖有效动态刚体与碰撞"),Mesh->IsSimulatingPhysics() &&
        Mesh->GetCollisionEnabled()==ECollisionEnabled::QueryAndPhysics && Mesh->IsPhysicsStateCreated())) return false;

    FHitResult FloorHit;
    const FCollisionQueryParams Query(SCENE_QUERY_STAT(ProductionPackagePhysicsFloor),false,Package);
    if (!TestTrue(TEXT("掉落点下方存在会阻挡正式包裹的真实地面"),
        World->LineTraceSingleByChannel(FloorHit,ReleasePoint,ReleasePoint-FVector(0,0,220),ECC_Visibility,Query) &&
        FloorHit.GetComponent()==Collision && Collision->IsPhysicsCollisionEnabled() &&
        Collision->GetCollisionResponseToChannel(Mesh->GetCollisionObjectType())==ECR_Block &&
        Mesh->GetCollisionResponseToChannel(Collision->GetCollisionObjectType())==ECR_Block)) return false;
    auto* Body=Mesh->GetBodyInstance();
    if (!TestTrue(TEXT("正式包裹刚体启用重力"),Body && Body->IsValidBodyInstance() &&
        Mesh->IsGravityEnabled() && World->GetGravityZ()<0.f)) return false;
    const FVector ReleaseBody=Body->GetUnrealWorldTransform().GetLocation();
    // 手动推进隔离世界时同步推进引擎帧号，让物理开始与结束 Tick 在每一步都入队
    for (int32 Frame=0; Frame<120; ++Frame)
    {
        ++GFrameCounter;
        World->Tick(LEVELTICK_All,1.f/60.f);
    }
    const FVector FinalBody=Body->GetUnrealWorldTransform().GetLocation();
    const double FinalBottom=Mesh->Bounds.GetBox().Min.Z;
    const double FinalSpeed=Mesh->GetPhysicsLinearVelocity().Size();
    AddInfo(FString::Printf(TEXT("PackageGroundPhysics release=%s body=%s actor=%s mesh=%s bottom=%.2f support=%.2f speed=%.2f"),
        *ReleaseBody.ToCompactString(),*FinalBody.ToCompactString(),*Package->GetActorLocation().ToCompactString(),
        *Mesh->GetComponentLocation().ToCompactString(),FinalBottom,FloorHit.ImpactPoint.Z,FinalSpeed));
    TestTrue(TEXT("真实刚体从释放点在重力下下降"),ReleaseBody.Z-FinalBody.Z>70.);
    TestTrue(TEXT("同一件外卖仍可见、归属为空且变换跟随根刚体"),Package->GetOwner()==nullptr &&
        Package->GetActorEnableCollision() && !Package->IsHidden() &&
        Package->GetActorLocation().Equals(Mesh->GetComponentLocation(),1.) &&
        Mesh->GetComponentLocation().Equals(FinalBody,1.));
    TestTrue(TEXT("同一件外卖落在实际支撑面并静稳"),
        FMath::Abs(FinalBottom-FloorHit.ImpactPoint.Z)<10. && FinalSpeed<5.);
    return true;
}
#endif
