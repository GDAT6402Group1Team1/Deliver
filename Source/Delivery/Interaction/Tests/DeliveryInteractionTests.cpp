#include "Interaction/DeliveryInteractionComponent.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"
#include "Character/DeliveryCharacter.h"
#include "Player/DeliveryPlayerController.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Carry/DeliveryCarryComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "UI/Controllers/DeliveryHotbarController.h"
#include "Components/SceneComponent.h"
#include "Game/DeliveryGameState.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/World/DeliveryTargetComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryInteractionFlowTest, "Delivery.Systems.Interaction",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryInteractionFlowTest::RunTest(const FString &)
{
    const auto Settings = UWorld::InitializationValues()
                              .AllowAudioPlayback(false)
                              .CreatePhysicsScene(true)
                              .ShouldSimulatePhysics(false)
                              .CreateNavigation(false)
                              .CreateAISystem(false);
    UWorld *World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
                                        ERHIFeatureLevel::Num, &Settings);
    if (!TestNotNull(TEXT("test world"), World))
        return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT
    {
        GEngine->DestroyWorldContext(World);
        World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false);
    };

    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ADeliveryCharacter *Character = World->SpawnActor<ADeliveryCharacter>(Spawn);
    ADeliveryPlayerController *Player = World->SpawnActor<ADeliveryPlayerController>(Spawn);
    if (!TestNotNull(TEXT("character"), Character) || !TestNotNull(TEXT("controller"), Player))
        return false;
    Player->Possess(Character);

    // 本测试检查交互规则，不启动身体物理模拟
    FEnumProperty *Mode = FindFProperty<FEnumProperty>(UDeliveryActiveRagdollComponent::StaticClass(),
                                                       TEXT("ReplicatedControlMode"));
    if (!TestNotNull(TEXT("body mode"), Mode))
        return false;
    Mode->GetUnderlyingProperty()->SetIntPropertyValue(
        Mode->ContainerPtrToValuePtr<void>(Character->GetActiveRagdoll()),
        uint64(EDeliveryRagdollControlMode::Active));

    AActor *Target = World->SpawnActor<AActor>(Spawn);
    USceneComponent *Root = NewObject<USceneComponent>(Target);
    Target->SetRootComponent(Root);
    Root->RegisterComponent();
    UDeliveryInteractableComponent *Action = NewObject<UDeliveryInteractableComponent>(Target);
    Target->AddInstanceComponent(Action);
    Action->RegisterComponent();
    Action->InteractionKey = EDeliveryInteractionKey::PickupE;
    Action->HoldDuration = 0.5f;
    Action->InteractRadius = 100.f;
    bool bAllowed = true;
    int32 Executions = 0;
    Action->CanUse.BindLambda([&](APawn *, bool) { return bAllowed; });
    Action->Use.BindLambda(
        [&](APawn *)
        {
            ++Executions;
            return true;
        });
    UDeliveryInteractionComponent *Interaction = Character->GetInteractionComponent();
    TestTrue(TEXT("target is available before requests"), Interaction->CanUse(Target));
    TestTrue(TEXT("player possesses character"), Player->GetPawn() == Character);
    AddInfo(FString::Printf(
        TEXT("initial mode=%d distance=%.1f location=%s target=%s"),
        int32(Character->GetActiveRagdoll()->GetControlMode()), Action->GetInteractionDistance(Character),
        *Character->GetActorLocation().ToString(), *Target->GetActorLocation().ToString()));
    const auto Send = [&](EDeliveryInteractionRequest Request)
    {
        // 空测试世界没有客户端连接，直接验证 RPC 的服务器实现
        Player->ServerRequestInteraction_Implementation(Character, Request, Target);
    };
    const auto Advance = [&]()
    {
        for (int32 Step = 0; Step < 6; ++Step)
            World->Tick(LEVELTICK_All, 0.1f);
    };

    Send(EDeliveryInteractionRequest::Use);
    TestEqual(TEXT("immediate request cannot bypass hold"), Executions, 0);
    Send(EDeliveryInteractionRequest::CompleteHold);
    TestEqual(TEXT("completion requires start"), Executions, 0);
    Send(EDeliveryInteractionRequest::BeginHold);
    Send(EDeliveryInteractionRequest::CompleteHold);
    TestEqual(TEXT("early completion rejected"), Executions, 0);

    Send(EDeliveryInteractionRequest::BeginHold);
    Advance();
    TestTrue(TEXT("target is available after time advances"), Interaction->CanUse(Target));
    AddInfo(FString::Printf(TEXT("after tick time=%.2f mode=%d distance=%.1f"), World->GetTimeSeconds(),
                            int32(Character->GetActiveRagdoll()->GetControlMode()),
                            Action->GetInteractionDistance(Character)));
    Send(EDeliveryInteractionRequest::CompleteHold);
    Send(EDeliveryInteractionRequest::CompleteHold);
    TestEqual(TEXT("completed hold executes once"), Executions, 1);

    Send(EDeliveryInteractionRequest::BeginHold);
    Send(EDeliveryInteractionRequest::CancelHold);
    Advance();
    Send(EDeliveryInteractionRequest::CompleteHold);
    TestEqual(TEXT("cancelled hold cannot execute"), Executions, 1);

    Send(EDeliveryInteractionRequest::BeginHold);
    Advance();
    Target->SetActorLocation(FVector(10000, 0, 0));
    Send(EDeliveryInteractionRequest::CompleteHold);
    TestEqual(TEXT("leaving range cancels execution"), Executions, 1);
    Target->SetActorLocation(Character->GetActorLocation());

    Send(EDeliveryInteractionRequest::BeginHold);
    Advance();
    bAllowed = false;
    Send(EDeliveryInteractionRequest::CompleteHold);
    TestEqual(TEXT("target rules are checked again"), Executions, 1);
    bAllowed = true;

    ADeliveryPlayerController *Other = World->SpawnActor<ADeliveryPlayerController>(Spawn);
    Action->HoldDuration = 0.f;
    Other->ServerRequestInteraction_Implementation(Character, EDeliveryInteractionRequest::Use, Target);
    TestEqual(TEXT("another player cannot act for this character"), Executions, 1);
    Send(EDeliveryInteractionRequest::Use);
    TestEqual(TEXT("owning player can perform immediate action"), Executions, 2);

    // 控制权转移到车辆后，同一个入口仍能找到驾驶员
    ADeliveryMotorbike *Bike = World->SpawnActor<ADeliveryMotorbike>(Spawn);
    if (!TestNotNull(TEXT("bike"), Bike))
        return false;
    TestTrue(TEXT("enter vehicle"), Bike->TryEnter(Character));
    Target->SetActorLocation(Character->GetActorLocation());
    Send(EDeliveryInteractionRequest::Use);
    TestEqual(TEXT("driver interaction works after possession changes"), Executions, 3);

    ADeliveryHandheldItem *Item = World->SpawnActor<ADeliveryHandheldItem>(Spawn);
    TestFalse(TEXT("ordinary item cannot be picked up while driving"), Item->CanPickup(Character, true));
    ADeliveryPackageItem *Package = World->SpawnActor<ADeliveryPackageItem>(Spawn);
    TestTrue(TEXT("vehicle accepts package"), Bike->AttachCargo(Package));
    TestEqual(TEXT("carry component sees vehicle cargo"), Character->GetCarryComponent()->GetPackage(),
              Package);
    TestTrue(TEXT("carry component clears vehicle cargo"),
             Character->GetCarryComponent()->ReleasePackage(Package));
    TestNull(TEXT("vehicle cargo cleared"), Bike->GetCargo());
    TestFalse(TEXT("unregistered task cannot be acquired"), Character->ReceiveTaskPackage(Package, false));
    TestNull(TEXT("failed receive clears cargo"), Bike->GetCargo());
    TestNull(TEXT("failed receive clears package owner"), Package->GetOwner());

    // 角色接口与界面数据读取同一份背包，重新绑定不会保留旧物品
    ADeliveryCharacter *Walker = World->SpawnActor<ADeliveryCharacter>(Spawn);
    TestTrue(TEXT("ordinary item enters inventory"), Walker->GetInventoryComponent()->TryPickup(Item));
    UDeliveryHotbarController *Hotbar = NewObject<UDeliveryHotbarController>();
    Hotbar->SetInventory(Walker->GetInventoryComponent());
    TestTrue(TEXT("hotbar displays current item"), Hotbar->GetSlot(0).bOccupied);
    Hotbar->SetInventory(nullptr);
    TestFalse(TEXT("hotbar rebind clears old item"), Hotbar->GetSlot(0).bOccupied);

    // 使用真实任务管理器验证领取和交付，初始订单设为已发出
    ADeliveryGameState *State = World->SpawnActor<ADeliveryGameState>(Spawn);
    World->SetGameState(State);
    UDeliveryTaskManagerComponent *Tasks = State->GetTaskManager();
    UDeliveryTaskDefinition *Definition = NewObject<UDeliveryTaskDefinition>(State);
    Definition->TaskId = TEXT("RefactorDeliveryTest");
    FArrayProperty *TaskProperty = FindFProperty<FArrayProperty>(Tasks->GetClass(), TEXT("Tasks"));
    if (!TestNotNull(TEXT("task state storage"), TaskProperty))
        return false;
    TArray<FDeliveryTaskState> &TaskStates =
        *TaskProperty->ContainerPtrToValuePtr<TArray<FDeliveryTaskState>>(Tasks);
    FDeliveryTaskState &Task = TaskStates.AddDefaulted_GetRef();
    Task.Definition = Definition;
    Task.Status = EDeliveryTaskStatus::Offered;
    TestTrue(TEXT("accept task starts timer"), Tasks->StartTaskTimerFromAcceptedCall(Definition));

    ADeliveryPackageItem *WalkingPackage = World->SpawnActor<ADeliveryPackageItem>(Spawn);
    WalkingPackage->GetDeliveryItemComponent()->OwningTask = Definition;
    TestTrue(TEXT("walking player receives task package"), Walker->ReceiveTaskPackage(WalkingPackage, false));
    TestEqual(TEXT("walking package is in carry query"), Walker->GetCarriedTaskPackage(), WalkingPackage);
    TestEqual(TEXT("pickup advances task"), Tasks->GetTaskStatus(Definition),
              EDeliveryTaskStatus::InProgress);

    UDeliveryTargetComponent *DropOff = NewObject<UDeliveryTargetComponent>(Target);
    Target->AddInstanceComponent(DropOff);
    DropOff->RegisterComponent();
    DropOff->ExpectedTask = Definition;
    DropOff->EnsureInteractableConfigured();
    TestTrue(TEXT("dropoff registers availability"), Action->CanUse.IsBound());
    TestTrue(TEXT("dropoff registers behavior"), Action->Use.IsBound());
    Target->SetActorLocation(WalkingPackage->GetActorLocation());
    TestTrue(TEXT("dropoff accepts correct package"), Action->CanUse.Execute(Walker, true));
    TestTrue(TEXT("dropoff completes delivery"), Action->Use.Execute(Walker));
    TestEqual(TEXT("task completed"), Tasks->GetTaskStatus(Definition), EDeliveryTaskStatus::Completed);
    TestNull(TEXT("completed package removed from inventory"), Walker->GetCarriedTaskPackage());
    TestFalse(TEXT("completed package destroyed"), IsValid(WalkingPackage));
    TestFalse(TEXT("task cannot pay twice"), Tasks->TryCompleteDelivery(Definition, nullptr));

    Target->Destroy();
    TestFalse(TEXT("destroyed target rejected"), Interaction->ValidateTarget(Target));
    return true;
}
#endif
