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
#include "Inventory/Items/DeliveryHandheldItem.h"
#include "Inventory/Items/DeliveryPackageItem.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/Items/DeliveryTestAxe.h"
#include "Carry/DeliveryCarryComponent.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "UI/Controllers/DeliveryHotbarController.h"
#include "UI/Controllers/DeliveryInteractionController.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Game/DeliveryGameState.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/World/DeliveryTargetComponent.h"
#include "Task/World/DeliveryPickupPointComponent.h"
#include "World/Location/DeliveryLocationComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryInteractionFlowTest, "Delivery.Systems.Interaction.PlayerFlow",
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
    auto* WalkerInventory=Walker->GetInventoryComponent();
    WalkerInventory->RequestSlotAction(UDeliveryInventoryComponent::FirstBackpackSlot);
    TestNull(TEXT("普通手持物仍可收进空背包槽"), WalkerInventory->GetHeldItem());
    TestTrue(TEXT("收纳后普通物品仍归原玩家"),
        WalkerInventory->GetItemInSlot(UDeliveryInventoryComponent::FirstBackpackSlot)==Item && Item->GetOwner()==Walker);
    WalkerInventory->RequestSlotAction(UDeliveryInventoryComponent::FirstBackpackSlot);
    TestTrue(TEXT("从背包可以装备同一个普通物品"), WalkerInventory->GetHeldItem()==Item);

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

    auto* Inventory=Character->GetInventoryComponent();
    constexpr int32 BackpackSlot=UDeliveryInventoryComponent::FirstBackpackSlot;
    TestNull(TEXT("第一格背包当前为空"), Inventory->GetItemInSlot(BackpackSlot));
    Inventory->RequestSlotAction(BackpackSlot);
    TestTrue(TEXT("按空背包槽会收好原包裹"),
        !Inventory->GetHeldItem() && Inventory->GetItemInSlot(BackpackSlot)==WalkingPackage &&
        WalkingPackage->GetOwner()==Character && WalkingPackage->IsHidden());
    TestFalse(TEXT("背包里已有原件时不能重新领件"), PickupPoint->TryDispense(Character));
    Inventory->RequestSlotAction(BackpackSlot);
    TestTrue(TEXT("按同一背包槽重新手持原件"), Inventory->GetHeldItem()==WalkingPackage && !WalkingPackage->IsHidden());

    auto* Axe=World->SpawnActor<ADeliveryTestAxe>(Spawn);
    if (!TestTrue(TEXT("持餐时捡来的斧头进背包"), IsValid(Axe) && Inventory->TryPickup(Axe) &&
            Inventory->GetItemInSlot(BackpackSlot)==Axe)) return false;
    {
        const FTransform PreviousPose=Character->GetActorTransform();
        ON_SCOPE_EXIT { Character->SetActorTransform(PreviousPose, false, nullptr, ETeleportType::TeleportPhysics); };
        // 将夹具移离重叠生成的角色和车辆，模拟已走到一扇闭门前
        Character->SetActorLocationAndRotation(FVector(2000,2000,500), FRotator::ZeroRotator,
                                               false, nullptr, ETeleportType::TeleportPhysics);
        const FVector Reference=Character->GetActorLocation()+FVector::UpVector*Inventory->GetDropUpOffset();
        const auto AddDropBlocker=[&](const FVector& Offset, const FVector& Extent) -> AActor*
        {
            AActor* Blocker=World->SpawnActor<AActor>(Spawn);
            if (!Blocker)
                return nullptr;
            UBoxComponent* Box=NewObject<UBoxComponent>(Blocker);
            Blocker->SetRootComponent(Box);
            Blocker->AddInstanceComponent(Box);
            Box->SetBoxExtent(Extent);
            Box->SetCollisionProfileName(TEXT("BlockAll"));
            Box->RegisterComponent();
            Blocker->SetActorLocation(Reference+Offset, false, nullptr, ETeleportType::TeleportPhysics);
            return Blocker;
        };
        AActor* Door=AddDropBlocker(FVector(54,0,0), FVector(3,150,100));
        if (!TestNotNull(TEXT("放件路径上的完整门碰撞"), Door)) return false;
        const auto IsPackageOnPlayerSide=[&]()
        {
            return WalkingPackage->GetComponentsBoundingBox(true).Max.X < Door->GetActorLocation().X-3.0;
        };

        Inventory->RequestSlotAction(0);
        TestNull(TEXT("快捷栏第一格仍放下手持快递"), Inventory->GetHeldItem());
        TestNull(TEXT("放下快递后解除玩家持有"), WalkingPackage->GetOwner());
        TestTrue(TEXT("闭门前显式放件后整件快递仍在玩家一侧"), IsPackageOnPlayerSide());
        if (!TestTrue(TEXT("放下后可以在同侧重新取回原包裹"),
                      Character->GetCarryComponent()->TryReceivePackage(WalkingPackage, false))) return false;

        AActor* Left=AddDropBlocker(FVector(0,-50,0), FVector(28,3,100));
        AActor* Right=AddDropBlocker(FVector(0,50,0), FVector(28,3,100));
        AActor* Behind=AddDropBlocker(FVector(-50,0,0), FVector(3,150,100));
        if (!TestTrue(TEXT("三条备选放置路线均有阻挡"), Left && Right && Behind)) return false;
        Inventory->RequestSlotAction(0);
        TestTrue(TEXT("无可用落点时按第一格仍持原包裹"),
                 Inventory->GetHeldItem()==WalkingPackage && WalkingPackage->GetOwner()==Character);
        Inventory->RequestSlotAction(BackpackSlot);
        TestTrue(TEXT("闭门前可以收起快递并装备原斧头"),
                 Inventory->GetHeldItem()==Axe && Axe->GetOwner()==Character &&
                 Inventory->GetItemInSlot(BackpackSlot)==WalkingPackage && WalkingPackage->GetOwner()==Character &&
                 WalkingPackage->IsHidden());
        Left->Destroy(); Right->Destroy(); Behind->Destroy();

        Inventory->RequestSlotAction(BackpackSlot);
        TestTrue(TEXT("切回原包裹时原斧头收回背包"), Inventory->GetHeldItem()==WalkingPackage &&
            WalkingPackage->GetOwner()==Character && Inventory->GetItemInSlot(BackpackSlot)==Axe);
        Door->Destroy();
    }

    UDeliveryTargetComponent *DropOff = NewObject<UDeliveryTargetComponent>(Target);
    Target->AddInstanceComponent(DropOff);
    DropOff->RegisterComponent();
    DropOff->ExpectedTask = Definition;
    DropOff->EnsureInteractableConfigured();
    TestTrue(TEXT("dropoff registers availability"), Action->CanUse.IsBound());
    TestTrue(TEXT("dropoff registers behavior"), Action->Use.IsBound());
    // 独立世界没有在线登录，交付仍需有效的任务结算玩家
    if (!IsValid(Character->GetTaskPlayerState()))
        Character->SetPlayerState(World->SpawnActor<APlayerState>());
    if (!TestTrue(TEXT("交付夹具拥有有效玩家身份并处于步行状态"),Character->CanHandleTaskPackage())) return false;
    Character->SetActorLocationAndRotation(FVector(2500,2500,500),FRotator::ZeroRotator,
        false,nullptr,ETeleportType::TeleportPhysics);
    Inventory->RefreshItemPresentation(WalkingPackage);
    Target->SetActorLocation(WalkingPackage->GetActorLocation());
    Inventory->RequestSlotAction(BackpackSlot);
    TestFalse(TEXT("原件留在背包时不能隔着库存交付"),Action->CanUse.Execute(Character,true));
    Inventory->RequestSlotAction(BackpackSlot);
    WalkingPackage->SetOwner(Bike);
    TestFalse(TEXT("vehicle-owned cargo cannot be delivered directly"),
              DropOff->TryDeliver(WalkingPackage, Character->GetTaskPlayerState()));
    WalkingPackage->SetOwner(Character);
    Inventory->RefreshItemPresentation(WalkingPackage);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryCarriedPackagePawnDestroyedTest,
    "Delivery.Systems.Interaction.PackageHolderDestroyed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryCarriedPackagePawnDestroyedTest::RunTest(const FString&)
{
    const auto Settings=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Settings);
    if (!TestNotNull(TEXT("真实角色销毁测试世界"),World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT
    {
        GEngine->DestroyWorldContext(World);
        World->EndPlay(EEndPlayReason::Quit);
        World->DestroyWorld(false);
    };
    auto* State=World->SpawnActor<ADeliveryGameState>();
    World->SetGameState(State);
    auto* Tasks=State->GetTaskManager();
    auto* Definition=NewObject<UDeliveryTaskDefinition>(State);
    Definition->TaskId=TEXT("PackageHolderDestroyed");
    Definition->PickupLocationId=TEXT("PackageHolderDestroyed_Pickup");
    auto* DefinitionProperty=FindFProperty<FArrayProperty>(Tasks->GetClass(),TEXT("TaskDefinitions"));
    if (!TestNotNull(TEXT("任务配置存在"),DefinitionProperty)) return false;
    DefinitionProperty->ContainerPtrToValuePtr<TArray<TObjectPtr<UDeliveryTaskDefinition>>>(Tasks)->Add(Definition);
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    if (!TestTrue(TEXT("正式任务流程接听并开始计时"),Tasks->StartTaskTimerFromAcceptedCall(Definition))) return false;

    auto* PickupActor=World->SpawnActor<AActor>();
    auto* PickupLocation=NewObject<UDeliveryLocationComponent>(PickupActor);
    PickupActor->SetRootComponent(PickupLocation);
    PickupLocation->LocationId=Definition->PickupLocationId;
    PickupLocation->RegisterComponent();
    auto* Pickup=NewObject<UDeliveryPickupPointComponent>(PickupActor);
    PickupActor->AddInstanceComponent(Pickup);
    Pickup->RegisterComponent();
    Pickup->EnsureInteractableConfigured();

    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    const auto MakeCarrier=[&](FName Name,const FVector& Position) -> ADeliveryCharacter*
    {
        Spawn.Name=Name;
        // 测试收纳和原件回收的原生角色没有物理资产
        AddExpectedError(FString::Printf(TEXT("%s: Skeletal mesh has no Physics Asset."),*Name.ToString()),
            EAutomationExpectedErrorFlags::Contains,1);
        auto* Pawn=World->SpawnActor<ADeliveryCharacter>(Position,FRotator::ZeroRotator,Spawn);
        if (!Pawn) return nullptr;
        Pawn->GetActiveRagdoll()->StopRagdoll();
        Pawn->SetPlayerState(World->SpawnActor<APlayerState>());
        return Pawn;
    };
    auto* First=MakeCarrier(TEXT("StoredPackageHolder"),FVector(0,0,100));
    auto* Second=MakeCarrier(TEXT("GroundPackageHolder"),FVector(0,300,100));
    if (!TestTrue(TEXT("首个步行玩家领到真实任务物品"),First && Second && Pickup->TryDispense(First))) return false;
    auto* Original=First->GetCarriedTaskPackage();
    if (!TestNotNull(TEXT("真实包裹占用即将销毁角色的手槽"),Original)) return false;
    First->GetInventoryComponent()->RequestSlotAction(UDeliveryInventoryComponent::FirstBackpackSlot);
    TestTrue(TEXT("销毁前原件已进入真实背包槽"),First->IsTaskPackageInBackpack(Original) && Original->IsHidden());
    World->Tick(LEVELTICK_All,.2f);
    const auto PickupClock=Tasks->GetTimeSnapshot(Definition);
    TestTrue(TEXT("模拟退出前已经计时"),PickupClock.bRunning && PickupClock.ElapsedSeconds>0.f);

    First->Destroy();
    if (!TestTrue(TEXT("有安全落点时角色销毁后原包裹留在世界"),IsValid(Original) &&
        !Original->GetOwner() && !Original->GetAttachParentActor() && Original->GetActorEnableCollision())) return false;
    const auto* OriginalPickup=UDeliveryInteractableComponent::FindOn(Original,EDeliveryInteractionKey::PickupE);
    TestTrue(TEXT("原包裹恢复物理查询与拾取提示"),
        Original->GetItemMesh()->GetCollisionEnabled()==ECollisionEnabled::QueryAndPhysics &&
        OriginalPickup && OriginalPickup->bInteractEnabled);
    TestEqual(TEXT("保留原包裹就不开放重复领件"),Tasks->GetTaskStatus(Definition),EDeliveryTaskStatus::InProgress);
    Second->SetActorLocation(Original->GetActorLocation(),false,nullptr,ETeleportType::TeleportPhysics);
    if (!TestTrue(TEXT("另一位玩家可拾起同一个原件"),Original->CanPickup(Second,true) && Original->Pickup(Second) &&
        Second->GetCarriedTaskPackage()==Original)) return false;

    // 让本次角色处于封闭碰撞内部，复用补领规则，避免在墙另一边生成落点
    auto* Blocker=World->SpawnActor<AActor>();
    auto* Box=NewObject<UBoxComponent>(Blocker);
    Blocker->SetRootComponent(Box);
    Box->SetBoxExtent(FVector(120));
    Box->SetCollisionProfileName(TEXT("BlockAll"));
    Box->RegisterComponent();
    const FVector SealedPosition(1200,0,100);
    Blocker->SetActorLocation(SealedPosition);
    Second->SetActorLocation(SealedPosition,false,nullptr,ETeleportType::TeleportPhysics);
    Second->Destroy();
    TestFalse(TEXT("原包裹无安全位置时通过真实销毁触发丢件"),IsValid(Original));
    TestEqual(TEXT("丢件仅退回待补取"),Tasks->GetTaskStatus(Definition),EDeliveryTaskStatus::AwaitingPickup);
    TestEqual(TEXT("补领取前保留原 elapsed 计时"),Tasks->GetTimeSnapshot(Definition).ElapsedSeconds,PickupClock.ElapsedSeconds);

    auto* ReplacementCarrier=MakeCarrier(TEXT("ReplacementPackageHolder"),FVector(0,0,100));
    if (!TestTrue(TEXT("无处落地时其他玩家可从原取货点补领"),ReplacementCarrier && Pickup->TryDispense(ReplacementCarrier) &&
        IsValid(ReplacementCarrier->GetCarriedTaskPackage()) && ReplacementCarrier->GetCarriedTaskPackage()!=Original)) return false;
    TestEqual(TEXT("补领同一订单继续进行中"),Tasks->GetTaskStatus(Definition),EDeliveryTaskStatus::InProgress);
    TestEqual(TEXT("补领不会重置接听时的计时"),Tasks->GetTimeSnapshot(Definition).ElapsedSeconds,PickupClock.ElapsedSeconds);
    return true;
}
#endif
