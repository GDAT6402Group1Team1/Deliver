#include "Carry/Tests/DeliveryPackageTransferProbe.h"
#include "Character/DeliveryCharacter.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryPackageItem.h"

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
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Carry/DeliveryPackageCarrierInterface.h"
#include "Game/DeliveryGameState.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

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
#endif
