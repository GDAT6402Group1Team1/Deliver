#include "Interaction/DeliveryInteractionComponent.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"
#include "Character/DeliveryCharacter.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Player/DeliveryPlayerController.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Interaction/DeliveryInteractionProbeComponent.h"
#include "Interaction/DeliveryInteractionRegistry.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Inventory/DeliveryHandheldItem.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Carry/DeliveryCarryComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "UI/Controllers/DeliveryHotbarController.h"
#include "UI/Controllers/DeliveryInteractionController.h"
#include "Components/SceneComponent.h"
#include "Game/DeliveryGameState.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/World/DeliveryTargetComponent.h"
#include "Task/World/DeliveryPickupPointComponent.h"
#include "World/DeliveryLocationComponent.h"

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
    // 独立测试世界没有登录流程，显式标为本机玩家才能运行提示探测
    Player->SetAsLocalPlayerController();
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
    TestTrue(TEXT("bike remains locally controlled"), Bike->IsLocallyControlled());
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

    // 骑车到取货点必须保留目标并显示下车提示，不能提前领取或直接塞入车厢
    Definition->PickupLocationId = TEXT("PickupPromptTest");
    UDeliveryLocationComponent *Location = NewObject<UDeliveryLocationComponent>(Target);
    Target->AddInstanceComponent(Location);
    Location->SetupAttachment(Root);
    Location->LocationId = Definition->PickupLocationId;
    Location->RegisterComponent();
    UDeliveryPickupPointComponent *PickupPoint = NewObject<UDeliveryPickupPointComponent>(Target);
    Target->AddInstanceComponent(PickupPoint);
    PickupPoint->InteractRadius = 1000.f;
    PickupPoint->RegisterComponent();
    PickupPoint->EnsureInteractableConfigured();
    // 此测试世界不启动角色 BeginPlay，手动登记目标以验证真实探测与长按路径
    World->GetSubsystem<UDeliveryInteractionRegistry>()->Register(Action);
    Target->SetActorLocation(Character->GetActorLocation());
    UDeliveryInteractionProbeComponent *Probe = Character->FindComponentByClass<UDeliveryInteractionProbeComponent>();
    UDeliveryInteractionController *Prompts = NewObject<UDeliveryInteractionController>();
    if (!TestNotNull(TEXT("interaction probe"), Probe))
        return false;
    TestTrue(TEXT("riding pickup remains a prompt candidate"), Character->CanUsePickupTarget(Target, false));
    TestFalse(TEXT("riding pickup cannot execute"), Character->CanUsePickupTarget(Target, true));
    Probe->TickComponent(0.05f, LEVELTICK_All, nullptr);
    TestEqual(TEXT("probe keeps pickup target while driving"), Probe->GetFocusedPickupActor(), Target);
    const FText RidingPrompt = Prompts->GetPrompt(Action, true, Character);
    TestFalse(TEXT("riding prompt differs from normal pickup prompt"), RidingPrompt.EqualTo(Action->PromptText));
    TestTrue(TEXT("riding prompt uses this player's blocked reason"),
             RidingPrompt.EqualTo(PickupPoint->GetBlockedReason(Character)));
    TestTrue(TEXT("another walking player receives the normal pickup prompt"),
             Prompts->GetPrompt(Action, true, Walker).EqualTo(Action->PromptText));
    TestFalse(TEXT("direct dispense cannot bypass dismount rule"), PickupPoint->TryDispense(Character));
    TestNull(TEXT("rejected pickup leaves vehicle empty"), Bike->GetCargo());
    TestEqual(TEXT("rejected pickup does not advance task"), Tasks->GetTaskStatus(Definition),
              EDeliveryTaskStatus::AwaitingPickup);

    // 在同一个骑车夹具里加入T提示竞争，实际E/F/左键目标保持各自原来的选择
    auto* Prompt=World->GetSubsystem<UDeliveryPromptSubsystem>();
    auto* Conversation=Player->FindComponentByClass<UDeliveryConversationComponent>();
    // 夹具没有启动角色初始化，使用引擎入口创建并初始化相机管理器
    if (!Player->PlayerCameraManager)
        Player->SpawnPlayerCameraManager();
    if (!TestNotNull(TEXT("prompt subsystem"),Prompt) || !TestNotNull(TEXT("conversation component"),Conversation) ||
        !TestNotNull(TEXT("player camera manager"),Player->PlayerCameraManager.Get())) return false;
    const FVector NpcLocation=Bike->GetActorLocation()+FVector(300,150,90);
    auto* Npc=World->SpawnActor<ADeliveryNpcCharacter>(NpcLocation,FRotator::ZeroRotator,Spawn);
    Npc->SetActorTickEnabled(false);
    Npc->SetActorEnableCollision(false);
    Npc->ConversationInteractable->bPromptAtTarget=true;
    Npc->ConversationInteractable->bUseConversationIcon=true;
    const auto AddPromptTarget=[&](EDeliveryInteractionKey Key,const TCHAR* Text)
    {
        auto* Actor=World->SpawnActor<AActor>(Spawn);
        auto* Anchor=NewObject<USceneComponent>(Actor); Actor->SetRootComponent(Anchor); Anchor->RegisterComponent();
        Actor->SetActorLocation(Character->GetActorLocation()+FVector(0,30,0));
        auto* Interactable=NewObject<UDeliveryInteractableComponent>(Actor); Actor->AddInstanceComponent(Interactable);
        Interactable->InteractionKey=Key; Interactable->PromptText=FText::FromString(Text); Interactable->InteractRadius=1000;
        Interactable->CanUse.BindLambda([](APawn*,bool){return true;});
        Interactable->Use.BindLambda([](APawn*){return true;});
        Interactable->RegisterComponent(); World->GetSubsystem<UDeliveryInteractionRegistry>()->Register(Interactable);
        return Actor;
    };
    auto* GeneralPromptActor=AddPromptTarget(EDeliveryInteractionKey::GeneralF,TEXT("普通交互"));
    auto* LeftPromptActor=AddPromptTarget(EDeliveryInteractionKey::ActionLeft,TEXT("左键操作"));
    auto* ViewActor=World->SpawnActor<AActor>(Spawn);
    auto* ViewCamera=NewObject<UCameraComponent>(ViewActor); ViewActor->SetRootComponent(ViewCamera); ViewCamera->RegisterComponent();
    ViewActor->SetActorLocation(Bike->GetActorLocation()+FVector(-150,0,140));
    auto* PreviousView=Player->GetViewTarget();
    const bool WalkerCollision=Walker->GetActorEnableCollision();
    const bool CharacterCollision=Character->GetActorEnableCollision();
    const bool BikeCollision=Bike->GetActorEnableCollision();
    // 夹具角色和车辆生成在同一点，先移除它们的遮挡以单独检查提示竞争
    Walker->SetActorEnableCollision(false); Character->SetActorEnableCollision(false); Bike->SetActorEnableCollision(false);
    auto CheckConversationPrompt=[&](const TCHAR* Label)
    {
        GeneralPromptActor->SetActorLocation(Character->GetActorLocation()+FVector(0,10,0));
        LeftPromptActor->SetActorLocation(Character->GetActorLocation()+FVector(0,20,0));
        Player->SetViewTarget(ViewActor);
        ViewActor->SetActorRotation(FRotator(0,180,0)); Player->PlayerCameraManager->UpdateCamera(0);
        Probe->TickComponent(.05f,LEVELTICK_All,nullptr);
        const auto PreviousPickup=Probe->GetFocusedPickupActor();
        const auto PreviousGeneral=Probe->GetFocusedActor();
        const auto PreviousAction=Probe->GetFocusedActionActor();
        TestFalse(TEXT("镜头转开时显示原操作提示"),Prompt->bPromptConversationIcon);
        ViewActor->SetActorRotation((NpcLocation-ViewActor->GetActorLocation()).Rotation());
        Player->PlayerCameraManager->UpdateCamera(0);
        TestTrue(Label,Conversation->GetAimedNpc()==Npc);
        Probe->TickComponent(.05f,LEVELTICK_All,nullptr);
        TestTrue(TEXT("对准NPC时T图示优先于身边物品和左键操作"),Prompt->bPromptConversationIcon &&
            Prompt->PromptAnchor.Equals(Npc->ConversationInteractable->GetPromptLocation(),.1));
        TestTrue(TEXT("交谈提示不会改写E/F/左键的实际目标"),Probe->GetFocusedPickupActor()==PreviousPickup &&
            Probe->GetFocusedActor()==PreviousGeneral && Probe->GetFocusedActionActor()==PreviousAction &&
            PreviousPickup==Target && PreviousGeneral==GeneralPromptActor && PreviousAction==LeftPromptActor);
    };
    CheckConversationPrompt(TEXT("骑车时交谈提示通过车辆当前控制器选择NPC"));
    Player->SetViewTarget(PreviousView); Player->PlayerCameraManager->UpdateCamera(0);
    Walker->SetActorEnableCollision(WalkerCollision); Character->SetActorEnableCollision(CharacterCollision); Bike->SetActorEnableCollision(BikeCollision);

    // 无模型夹具下车时会尝试重启布娃娃，仅预期这一条缺少物理资产日志
    AddExpectedError(TEXT("DeliveryCharacter_0: Skeletal mesh has no Physics Asset."),
                     EAutomationExpectedErrorFlags::Contains, 1);
    Bike->ExitVehicle();
    TestFalse(TEXT("character is no longer riding"), Character->IsRidingVehicle());
    TestTrue(TEXT("controller returns to character"), Player->GetPawn() == Character);
    Mode->GetUnderlyingProperty()->SetIntPropertyValue(
        Mode->ContainerPtrToValuePtr<void>(Character->GetActiveRagdoll()),
        uint64(EDeliveryRagdollControlMode::Active));
    Target->SetActorLocation(Character->GetActorLocation());
    Probe->TickComponent(0.05f, LEVELTICK_All, nullptr);
    TestEqual(TEXT("probe keeps pickup target after dismount"), Probe->GetFocusedPickupActor(), Target);
    TestTrue(TEXT("normal E prompt returns after dismount"),
             Prompts->GetPrompt(Action, true, Character).EqualTo(Action->PromptText));
    Walker->SetActorEnableCollision(false); Character->SetActorEnableCollision(false); Bike->SetActorEnableCollision(false);
    CheckConversationPrompt(TEXT("下车后交谈提示使用角色当前控制器"));
    Player->SetViewTarget(Character); Player->PlayerCameraManager->UpdateCamera(0);
    Walker->SetActorEnableCollision(WalkerCollision); Character->SetActorEnableCollision(CharacterCollision); Bike->SetActorEnableCollision(BikeCollision);
    GeneralPromptActor->Destroy(); LeftPromptActor->Destroy(); ViewActor->Destroy(); Npc->Destroy();
    TestTrue(TEXT("walking pickup is available"), Character->CanUsePickupTarget(Target, true));
    Send(EDeliveryInteractionRequest::BeginHold);
    Advance();
    Send(EDeliveryInteractionRequest::CompleteHold);
    ADeliveryPackageItem *WalkingPackage = Character->GetCarriedTaskPackage();
    if (!TestNotNull(TEXT("holding E after dismount dispenses package"), WalkingPackage))
        return false;
    Send(EDeliveryInteractionRequest::CompleteHold);
    TestFalse(TEXT("pickup point cannot dispense a duplicate package"), PickupPoint->TryDispense(Character));
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
    WalkingPackage->SetOwner(Bike);
    TestFalse(TEXT("vehicle-owned cargo cannot be delivered directly"),
              DropOff->TryDeliver(WalkingPackage, Character->GetTaskPlayerState()));
    WalkingPackage->SetOwner(Character);
    TestTrue(TEXT("dropoff accepts correct package"), Action->CanUse.Execute(Character, true));
    TestTrue(TEXT("dropoff completes delivery"), Action->Use.Execute(Character));
    TestEqual(TEXT("task completed"), Tasks->GetTaskStatus(Definition), EDeliveryTaskStatus::Completed);
    TestNull(TEXT("completed package removed from inventory"), Character->GetCarriedTaskPackage());
    TestFalse(TEXT("completed package destroyed"), IsValid(WalkingPackage));
    TestFalse(TEXT("task cannot pay twice"), Tasks->TryCompleteDelivery(Definition, nullptr));

    Target->Destroy();
    TestFalse(TEXT("destroyed target rejected"), Interaction->ValidateTarget(Target));
    return true;
}
#endif
