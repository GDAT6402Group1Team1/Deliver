#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AbilitySystem/DeliverGameplayTags.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Companion/Core/DeliveryCompanionTuning.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AIController.h"
#include "Animation/AnimSequence.h"
#include "Audio/Tests/DeliveryAudioTestComponent.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Carry/Tests/DeliveryPackageTransferProbe.h"
#include "Character/DeliveryCharacter.h"
#include "Carry/DeliveryHandheldCarrierInterface.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Economy/DeliveryWalletComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Game/DeliveryGameState.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryInventoryItemComponent.h"
#include "Inventory/Items/DeliveryPackageItem.h"
#include "Inventory/Items/DeliveryCaterpillarItem.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "NavLinkCustomComponent.h"
#include "NPC/Grandpa/DeliveryGrandpa.h"
#include "Navigation/PathFollowingComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "Player/DeliverPlayerState.h"
#include "Player/DeliveryPlayerController.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Sound/SoundWave.h"
#include "StaticMeshCompiler.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Tracking/DeliveryTaskTrackerComponent.h"
#include "Task/World/DeliveryBreakableDoor.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/World/DeliveryPickupPointComponent.h"
#include "Task/World/DeliveryTargetComponent.h"
#include "Vehicle/Companion/DeliveryCompanionMotorbike.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"
#include "Vehicle/Summon/DeliveryVehicleSummonComponent.h"
#include "World/Home/DeliveryHome.h"
#include "World/Location/DeliveryLocationComponent.h"
#include "World/DeliveryNavigationAreas.h"
#include "UObject/UnrealType.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryCompanionErrandTest, "Delivery.AI.Companion.ErrandPackageAndCommandLifetime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryCompanionErrandTest::RunTest(const FString&)
{
    auto* Settings = GetMutableDefault<UDeliveryAiSettings>();
    const bool bAiEnabled = Settings->bEnabled;
    Settings->bEnabled = false;
    ON_SCOPE_EXIT { Settings->bEnabled = bAiEnabled; };
    const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
    if (!TestNotNull(TEXT("创建独立规则测试世界"), World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit);
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
    };
    auto* State = World->SpawnActor<ADeliveryGameState>();
    if (!TestNotNull(TEXT("创建真实游戏状态"), State)) return false;
    World->SetGameState(State);
    auto* Tasks = State->GetTaskManager();
    auto* Task = NewObject<UDeliveryTaskDefinition>(State);
    Task->TaskId = TEXT("CompanionErrand_Test");
    Task->PickupLocationId = TEXT("CompanionErrand_Pickup");
    Task->DeliveryLocationId = TEXT("CompanionErrand_Delivery");
    Task->ReceiverNpcId = TEXT("CompanionErrand_Receiver");
    Task->BaseReward = 125;
    Task->TimeLimitSeconds = 120;
    // 反射只设置相当于关卡蓝图的任务配置，运行进度由接听和取件接口推进
    auto* Definitions = FindFProperty<FArrayProperty>(Tasks->GetClass(), TEXT("TaskDefinitions"));
    if (!TestNotNull(TEXT("任务配置字段存在"), Definitions)) return false;
    auto* TaskDefinitions=Definitions->ContainerPtrToValuePtr<TArray<TObjectPtr<UDeliveryTaskDefinition>>>(Tasks);
    TaskDefinitions->Add(Task);
    auto* NextTask=NewObject<UDeliveryTaskDefinition>(State);
    NextTask->TaskId=TEXT("CompanionErrand_Next");
    NextTask->ReceiverNpcId=TEXT("CompanionErrand_NextReceiver");
    NextTask->DeliveryLocationId=NextTask->ReceiverNpcId;
    NextTask->BaseReward=85;
    NextTask->TimeLimitSeconds=120;
    TaskDefinitions->Add(NextTask);
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    TestEqual(TEXT("登记任务只到未接听状态"), Tasks->GetTaskStatus(Task), EDeliveryTaskStatus::Offered);
    if (!TestTrue(TEXT("真实接听入口开始订单"), Tasks->StartTaskTimerFromAcceptedCall(Task))) return false;
    TestEqual(TEXT("接听后等待实际取件"), Tasks->GetTaskStatus(Task), EDeliveryTaskStatus::AwaitingPickup);

    const auto* StartRagdoll=FindFProperty<FBoolProperty>(UDeliveryActiveRagdollComponent::StaticClass(),TEXT("bStartOnBeginPlay"));
    if (!TestNotNull(TEXT("原生玩家夹具可配置开局物理驱动"),StartRagdoll)) return false;
    const auto MakePlayer = [&](const FVector& Location,bool bLocal) -> ADeliveryCharacter*
    {
        auto* PC = World->SpawnActor<ADeliveryPlayerController>();
        auto* PlayerState = World->SpawnActor<ADeliverPlayerState>();
        if (!PC || !PlayerState) return nullptr;
        if (bLocal) PC->Player=NewObject<ULocalPlayer>(GEngine);
        PlayerState->SetOwner(PC);
        PC->PlayerState = PlayerState;
        const FTransform SpawnTransform(Location);
        auto* Character=World->SpawnActorDeferred<ADeliveryCharacter>(ADeliveryCharacter::StaticClass(),SpawnTransform,
            nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if (!Character) return nullptr;
        // 规则世界只需要原生玩家的控制、胶囊和交接组件
        StartRagdoll->SetPropertyValue_InContainer(Character->GetActiveRagdoll(),false);
        Character->FinishSpawning(SpawnTransform);
        PC->Possess(Character);
        Character->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
        return Character;
    };
    auto* Owner = MakePlayer(FVector(0, 0, 88),true);
    auto* Stranger = MakePlayer(FVector(0, 500, 88),false);
    if (!TestTrue(TEXT("仅主人具有客户端反馈所需的真实本地控制"),Owner && Owner->IsLocallyControlled() &&
        Stranger && !Stranger->IsLocallyControlled())) return false;
    auto* OwnerState = Owner->GetPlayerState<ADeliverPlayerState>();
    auto* StrangerState = Stranger->GetPlayerState<ADeliverPlayerState>();
    if (!TestNotNull(TEXT("主人角色持有真实玩家状态"), OwnerState) ||
        !TestNotNull(TEXT("访客角色持有独立玩家状态"), StrangerState)) return false;
    auto* OwnerWallet = OwnerState->GetWallet();
    auto* StrangerWallet = StrangerState->GetWallet();
    const int32 OriginalBalance = OwnerWallet->GetBalance();
    const int32 StrangerBalance = StrangerWallet->GetBalance();
    auto* AudioConfig=NewObject<UDeliveryAudioConfig>(World);
    FDeliverySoundSettings ConfirmedSound;
    ConfirmedSound.Sound=NewObject<USoundWave>(AudioConfig);
    AudioConfig->Sounds.Add(EDeliveryEvent::DeliverySucceeded,ConfirmedSound);
    const auto ObserveDeliveryFeedback=[&](ADeliveryCharacter* Character)
    {
        auto* Audio=NewObject<UDeliveryAudioTestComponent>(Character);
        Audio->Config=AudioConfig;
        Audio->RegisterComponent();
        return Audio;
    };
    const auto* OwnerAudio=ObserveDeliveryFeedback(Owner);
    const auto* StrangerAudio=ObserveDeliveryFeedback(Stranger);
    if (!TestTrue(TEXT("双方声音探针开始监听角色事件"),OwnerAudio->HasBegunPlay() && StrangerAudio->HasBegunPlay())) return false;

    auto* Npc = World->SpawnActor<ADeliveryCompanionCharacter>(FVector(150, 0, 88), FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("创建同伴"), Npc)) return false;
    auto* Companion = Npc->Companion.Get();
    // 从已结识的前置状态开始，不把直接设置阶段当作扶起或初遇验证
    Npc->Encounter.EncounterStartedAt = 0;
    Npc->Encounter.Phase = EDeliveryCompanionEncounterPhase::Joined;
    Companion->OwnerPlayerState = OwnerState;
    Companion->Mode = EDeliveryCompanionMode::Follow;
    Npc->OnRep_Encounter();
    ADeliveryPackageItem* Package = World->SpawnActor<ADeliveryTestPackage>(FVector(0, 0, 150), FRotator::ZeroRotator);
    Package->GetDeliveryItemComponent()->OwningTask = Task;
    if (!TestTrue(TEXT("主人实际接收订单包裹"), Owner->TryReceiveTaskPackage(Package))) return false;
    TestEqual(TEXT("实际取件才进入配送中"), Tasks->GetTaskStatus(Task), EDeliveryTaskStatus::InProgress);
    const FDeliveryTaskTimeSnapshot AcceptedTime = Tasks->GetTimeSnapshot(Task);
    const double AcceptedServerTime = State->GetServerWorldTimeSeconds();

    const auto Snapshot = [&]()
    {
        FDeliveryNpcDecisionRequest Request;
        Request.bPlayerRequest=true;
        Companion->ExtendDecisionRequest(Request, OwnerState);
        Companion->ExtendTravelDecisionRequest(Request, OwnerState);
        return Request;
    };
    const auto Apply = [&](EDeliveryNpcAction Action, APlayerState* Requester, const FDeliveryNpcDecisionRequest& Request)
    {
        return Companion->ApplyCommand(Action, Requester, Request.CompanionCommandRevision,
            Request.CompanionTask.Get(), Request.CompanionPackage.Get());
    };
    const auto CheckNotDelivered = [&]()
    {
        TestEqual(TEXT("交接或取消不完成共享订单"), Tasks->GetTaskStatus(Task), EDeliveryTaskStatus::InProgress);
        TestEqual(TEXT("交接或取消不清掉计时订单"), Tasks->GetTimedTask(), Task);
        const auto CurrentTime = Tasks->GetTimeSnapshot(Task);
        const float ExpectedElapsed = AcceptedTime.ElapsedSeconds + static_cast<float>(State->GetServerWorldTimeSeconds()-AcceptedServerTime);
        TestTrue(TEXT("原订单计时持续有效"), CurrentTime.bRunning);
        TestEqual(TEXT("交接和暂停期间订单沿原起点随服务器时钟推进"), CurrentTime.ElapsedSeconds, ExpectedElapsed);
        TestEqual(TEXT("接受和结束跑腿不向主人发奖"), OwnerWallet->GetBalance(), OriginalBalance);
        TestEqual(TEXT("接受和结束跑腿不向访客发奖"), StrangerWallet->GetBalance(), StrangerBalance);
        TestEqual(TEXT("只转交和等待时主人没有收到送达提示"),OwnerAudio->PlayCount,0);
        TestEqual(TEXT("旁观玩家没有收到送达提示"),StrangerAudio->PlayCount,0);
        TestTrue(TEXT("同一实物仍然存在"), IsValid(Package));
    };

    const FDeliveryNpcDecisionRequest BeforeTake = Snapshot();
    TestEqual(TEXT("请求快照指向主人真实携带的实物"), BeforeTake.CompanionPackage.Get(), Package);
    TestEqual(TEXT("手持槽中的原包裹按库存报告位置"), BeforeTake.CompanionOwnerPackageLocation, FName(TEXT("Hand")));
    TestTrue(TEXT("主人能得到接件候选"), BeforeTake.AllowedActions.Contains(EDeliveryNpcAction::TakePackage));
    FDeliveryNpcDecisionRequest VisitorRequest;
    Companion->ExtendDecisionRequest(VisitorRequest, StrangerState);
    for (const auto Action : {EDeliveryNpcAction::FetchPackage, EDeliveryNpcAction::TakePackage,
        EDeliveryNpcAction::DeliverPackage, EDeliveryNpcAction::ReturnPackage, EDeliveryNpcAction::CancelCompanionTask})
        TestFalse(TEXT("访客请求没有跑腿和取消候选"), VisitorRequest.AllowedActions.Contains(Action));
    TestFalse(TEXT("访客不能开始替主人接件"), Apply(EDeliveryNpcAction::TakePackage, StrangerState, BeforeTake));
    TestEqual(TEXT("拒绝访客前未释放主人实物"), Owner->GetCarriedTaskPackage(), Package);
    TestEqual(TEXT("拒绝访客不改变安排版本"), Companion->GetCommandRevision(), BeforeTake.CompanionCommandRevision);
    if (!TestTrue(TEXT("当前主人快照可以开始接件"), Apply(EDeliveryNpcAction::TakePackage, OwnerState, BeforeTake))) return false;
    TestTrue(TEXT("接受接件后仍等待实际交接"), Companion->HasActiveErrand());
    TestEqual(TEXT("接受委托本身不转移实物"), Owner->GetCarriedTaskPackage(), Package);
    CheckNotDelivered();

    auto* OldTask = NewObject<UDeliveryTaskDefinition>(State);
    OldTask->TaskId = TEXT("PreviousCompanionTask");
    ADeliveryPackageItem* OldPackage = World->SpawnActor<ADeliveryTestPackage>(FVector(1000, 1000, 150), FRotator::ZeroRotator);
    OldPackage->GetDeliveryItemComponent()->OwningTask = OldTask;
    const FDeliveryNpcDecisionRequest RunningTake = Snapshot();
    TestEqual(TEXT("正在接件时委托记录自己的订单"),RunningTake.CompanionErrandTaskId,Task->TaskId.ToString());
    TestFalse(TEXT("旧版本不能替换正在执行的接件"), Apply(EDeliveryNpcAction::TakePackage, OwnerState, BeforeTake));
    auto WrongTask = RunningTake;
    WrongTask.CompanionTask = OldTask;
    TestFalse(TEXT("旧订单快照不能开始并取消当前委托"), Apply(EDeliveryNpcAction::TakePackage, OwnerState, WrongTask));
    auto WrongPackage = RunningTake;
    WrongPackage.CompanionPackage = OldPackage;
    TestFalse(TEXT("旧实物快照不能开始并取消当前委托"), Apply(EDeliveryNpcAction::TakePackage, OwnerState, WrongPackage));
    TestFalse(TEXT("访客不能取消当前接件"), Apply(EDeliveryNpcAction::CancelCompanionTask, StrangerState, RunningTake));
    TestFalse(TEXT("旧版本取消不能覆盖当前接件"), Apply(EDeliveryNpcAction::CancelCompanionTask, OwnerState, BeforeTake));
    TestTrue(TEXT("被拒请求留下正在执行的接件"), Companion->HasActiveErrand());
    TestEqual(TEXT("被拒请求未推进安排版本"), Companion->GetCommandRevision(), RunningTake.CompanionCommandRevision);
    TestEqual(TEXT("被拒请求未换掉执行订单"), Companion->GetErrand().Task.Get(), Task);
    TestEqual(TEXT("被拒请求未换掉执行实物"), Companion->GetErrand().Package.Get(), Package);

    // 双方已在交接范围内，调用真实委托更新；不创建导航或假定到达事件
    Companion->UpdateErrand();
    TestEqual(TEXT("Take 将同一份实物交给同伴"), Npc->GetCarriedTaskPackage(), Package);
    TestEqual(TEXT("实物 Owner 同步指向同伴"), Package->GetOwner(), static_cast<AActor*>(Npc));
    TestNull(TEXT("Take 成功后主人手持槽清空"), Owner->GetCarriedTaskPackage());
    TestFalse(TEXT("实际接件成功才结束 Take"), Companion->HasActiveErrand());
    TestEqual(TEXT("接件报告来自真实转移成功"), Companion->GetErrand().Reason, FName(TEXT("PackageReceived")));
    TestEqual(TEXT("Take 保留原 Follow 模式"), Companion->GetMode(), EDeliveryCompanionMode::Follow);
    CheckNotDelivered();

    const auto BeforeReturn = Snapshot();
    TestTrue(TEXT("近处归还提供无需另选出行的候选"), BeforeReturn.Candidates.ContainsByPredicate([](const auto& Candidate)
    { return Candidate.Action==EDeliveryNpcAction::ReturnPackage && Candidate.TravelMode==EDeliveryCompanionTravelMode::Unspecified; }));
    if (!TestTrue(TEXT("当前快照开始交还"), Apply(EDeliveryNpcAction::ReturnPackage, OwnerState, BeforeReturn))) return false;
    Companion->UpdateErrand();
    TestEqual(TEXT("Return 把同一个实物交还主人"), Owner->GetCarriedTaskPackage(), Package);
    TestEqual(TEXT("交还后的实物 Owner 是主人"), Package->GetOwner(), static_cast<AActor*>(Owner));
    TestNull(TEXT("Return 清掉同伴持件引用"), Npc->GetCarriedTaskPackage());
    TestEqual(TEXT("交还报告来自真实转移成功"), Companion->GetErrand().Reason, FName(TEXT("PackageReturned")));
    CheckNotDelivered();

    TestTrue(TEXT("主人改为原地等候"), Apply(EDeliveryNpcAction::Stay, OwnerState, Snapshot()));
    if (!TestTrue(TEXT("等候模式仍可接件"), Apply(EDeliveryNpcAction::TakePackage, OwnerState, Snapshot()))) return false;
    Companion->UpdateErrand();
    TestEqual(TEXT("再次 Take 仍携带原实物"), Npc->GetCarriedTaskPackage(), Package);
    TestEqual(TEXT("Take 保留原 Stay 模式"), Companion->GetMode(), EDeliveryCompanionMode::Stay);
    auto* Held = World->SpawnActor<ADeliveryHandheldItem>(FVector(0, 600, 150), FRotator::ZeroRotator);
    if (!TestTrue(TEXT("主人真实拿起普通物品占用手持槽"), Owner->GetInventoryComponent()->TryPickup(Held))) return false;
    auto* OwnerInventory=Owner->GetInventoryComponent();
    if (!TestTrue(TEXT("主人手忙时可以直接要求交还外卖"),Apply(EDeliveryNpcAction::ReturnPackage,OwnerState,Snapshot()))) return false;
    Companion->UpdateErrand();
    if (!TestTrue(TEXT("普通交还自动将同一外卖收进空背包槽，并保留手持工具"),
        Owner->IsTaskPackageInBackpack(Package) && OwnerInventory->GetHeldItem()==Held &&
        !Npc->GetCarriedTaskPackage() && Companion->GetErrand().Reason==TEXT("PackageStoredInOwnerBackpack"))) return false;
    if (!TestTrue(TEXT("下次委托仍能从实际背包取回同一原件"),Apply(EDeliveryNpcAction::TakePackage,OwnerState,Snapshot()))) return false;
    Companion->UpdateErrand();
    if (!TestTrue(TEXT("测试备用槽前同一个外卖已经再次交给二狗"),Npc->GetCarriedTaskPackage()==Package &&
        OwnerInventory->GetHeldItem()==Held && !Owner->IsTaskPackageInBackpack(Package))) return false;
    TArray<ADeliveryHandheldItem*> Fillers;
    for (int32 Slot=UDeliveryInventoryComponent::FirstBackpackSlot; Slot<UDeliveryInventoryComponent::SlotCount; ++Slot)
    {
        auto* Filler=World->SpawnActor<ADeliveryHandheldItem>(FVector(0,650,150),FRotator::ZeroRotator);
        if (!TestTrue(TEXT("主人用实物占满背包槽"),OwnerInventory->TryPickup(Filler))) return false;
        Fillers.Add(Filler);
    }
    const auto BeforeBusyReturn = Snapshot();
    if (!TestTrue(TEXT("手和背包都满时仍可安排稍后交还"), Apply(EDeliveryNpcAction::ReturnPackage, OwnerState, BeforeBusyReturn))) return false;
    Companion->UpdateErrand();
    TestTrue(TEXT("手忙暂停仍保留当前交还委托"), Companion->HasActiveErrand());
    TestEqual(TEXT("手忙报告不冒充交还成功"), Companion->GetSuspensionReason(), FName(TEXT("OwnerHandsBusy")));
    TestEqual(TEXT("暂停未丢弃同伴原实物"), Npc->GetCarriedTaskPackage(), Package);
    TestEqual(TEXT("暂停未替主人丢掉普通物品"), Owner->GetInventoryComponent()->GetHeldItem(), Held);
    const auto BusyReturn = Snapshot();
    TestEqual(TEXT("暂停仍向模型报告正在交还"), BusyReturn.CompanionErrandStatus, FString(TEXT("Running")));
    TestEqual(TEXT("暂停快照携带当前真实原因"), BusyReturn.CompanionErrandReason, FString(TEXT("OwnerHandsBusy")));
    TestFalse(TEXT("访客不能取消暂停中的委托"), Apply(EDeliveryNpcAction::CancelCompanionTask, StrangerState, BusyReturn));
    TestFalse(TEXT("开始前的旧版本不能取消暂停中的委托"), Apply(EDeliveryNpcAction::CancelCompanionTask, OwnerState, BeforeBusyReturn));
    if (!TestTrue(TEXT("当前主人可以取消暂停中的委托"), Apply(EDeliveryNpcAction::CancelCompanionTask, OwnerState, BusyReturn))) return false;
    Companion->UpdateErrand();
    TestFalse(TEXT("取消后更新不会继续交还"), Companion->HasActiveErrand());
    TestEqual(TEXT("取消明确保留 Cancelled 状态"), Companion->GetErrand().Status, EDeliveryNpcActionStatus::Cancelled);
    TestEqual(TEXT("取消回到原地等候"), Companion->GetMode(), EDeliveryCompanionMode::Stay);
    TestEqual(TEXT("取消保留同一份实物"), Npc->GetCarriedTaskPackage(), Package);
    TestEqual(TEXT("取消不改变实物所有者"), Package->GetOwner(), static_cast<AActor*>(Npc));
    TestEqual(TEXT("取消不清掉主人原手物"), Owner->GetInventoryComponent()->GetHeldItem(), Held);
    const auto AfterCancel=Snapshot();
    TestTrue(TEXT("已取消的旧暂停从新快照消失"), AfterCancel.CompanionSuspensionReason.IsEmpty());
    TestEqual(TEXT("取消之后报告真实结束状态"), AfterCancel.CompanionErrandStatus, FString(TEXT("Cancelled")));
    CheckNotDelivered();

    const auto BackpackRequest=Snapshot();
    TestTrue(TEXT("持有原件时主人可以指令收进实际背包"),BackpackRequest.AllowedActions.Contains(EDeliveryNpcAction::StorePackageInBackpack));
    TestFalse(TEXT("旁观者不能用主人的快照收包裹"),Apply(EDeliveryNpcAction::StorePackageInBackpack,StrangerState,BackpackRequest));
    if (!TestTrue(TEXT("背包交接可等待一个真实空槽"),Apply(EDeliveryNpcAction::StorePackageInBackpack,OwnerState,BackpackRequest))) return false;
    Companion->UpdateErrand();
    TestEqual(TEXT("背包满时保留原件并报告原因"),Companion->GetSuspensionReason(),FName(TEXT("OwnerBackpackUnavailable")));
    TestEqual(TEXT("背包满时原件仍由二狗保管"),Npc->GetCarriedTaskPackage(),Package);
    auto* ReleasedFiller=Fillers.Pop();
    OwnerInventory->RemoveItem(ReleasedFiller);
    ReleasedFiller->Destroy();
    Companion->UpdateErrand();
    constexpr int32 PackageSlot=UDeliveryInventoryComponent::SlotCount-1;
    TestEqual(TEXT("交接完成后同一个包裹确实在背包格"),OwnerInventory->GetItemInSlot(PackageSlot),static_cast<ADeliveryHandheldItem*>(Package));
    TestEqual(TEXT("主人手中原物保持不变"),OwnerInventory->GetHeldItem(),Held);
    TestTrue(TEXT("包裹的真实归属和背包显隐一致"),Package->GetOwner()==Owner && Package->IsHidden() &&
        !Package->GetActorEnableCollision() && !Npc->GetCarriedTaskPackage());
    TestEqual(TEXT("交接结果确认进入真实背包槽"),Companion->GetErrand().Reason,FName(TEXT("PackageStoredInOwnerBackpack")));
    TestEqual(TEXT("主人请求按真实槽报告背包位置"),Snapshot().CompanionOwnerPackageLocation,FName(TEXT("Backpack")));
    OwnerInventory->RequestSlotAction(PackageSlot);
    TestTrue(TEXT("数字键将原包裹取回手上并恢复显示"),OwnerInventory->GetHeldItem()==Package && !Package->IsHidden());
    OwnerInventory->RequestSlotAction(PackageSlot);
    TestTrue(TEXT("换回原手物会再次收好同一个包裹"),OwnerInventory->GetHeldItem()==Held && Owner->IsTaskPackageInBackpack(Package));
    if (!TestTrue(TEXT("主人可以再把背包里的同一件交给二狗"),Apply(EDeliveryNpcAction::TakePackage,OwnerState,Snapshot()))) return false;
    Companion->UpdateErrand();
    TestTrue(TEXT("取出原件清掉背包槽并保留主人手物"),Npc->GetCarriedTaskPackage()==Package &&
        OwnerInventory->GetItemInSlot(PackageSlot)==nullptr && OwnerInventory->GetHeldItem()==Held);
    for (auto* Filler:Fillers)
    {
        OwnerInventory->RemoveItem(Filler);
        Filler->Destroy();
    }
    CheckNotDelivered();

    Owner->GetInventoryComponent()->ConsumeHeldItem(Held);
    Held->Destroy();
    if (!TestTrue(TEXT("取消后可用新快照重新安排交还"), Apply(EDeliveryNpcAction::ReturnPackage, OwnerState, Snapshot()))) return false;
    Companion->UpdateErrand();
    TestEqual(TEXT("新委托实际交还取消时保留的原实物"), Owner->GetCarriedTaskPackage(), Package);
    TestNull(TEXT("最后交还后同伴不再持件"), Npc->GetCarriedTaskPackage());
    TestEqual(TEXT("新委托交还成功后旧暂停不会覆盖实际结果"), Snapshot().CompanionErrandReason, FString(TEXT("PackageReturned")));
    CheckNotDelivered();

    {
        const FVector Ground(14000, 7000, 0);
        auto* ProductionTuning=LoadObject<UDeliveryCompanionTuning>(nullptr,
            TEXT("/Game/NPC/Companion/DA_CompanionTuning.DA_CompanionTuning"));
        auto* ProductionMesh=LoadObject<USkeletalMesh>(nullptr,
            TEXT("/Game/NPC/Companion/Appearance/SK_ErgouBoy.SK_ErgouBoy"));
        auto* OwnerController=Cast<ADeliveryPlayerController>(Owner->GetController());
        auto* Ragdoll=Owner->GetActiveRagdoll();
        auto* BodyMode=FindFProperty<FEnumProperty>(UDeliveryActiveRagdollComponent::StaticClass(),TEXT("ReplicatedControlMode"));
        if (!TestTrue(TEXT("放件回归使用正式地面动画与主人交互入口"),
            ProductionTuning && ProductionTuning->GroundPickupAnimation && ProductionMesh && OwnerController && BodyMode)) return false;
        auto* Move=Npc->GetCharacterMovement();
        const auto SavedNpcTransform=Npc->GetActorTransform();
        const auto SavedOwnerTransform=Owner->GetActorTransform();
        auto* SavedMesh=Npc->GetMesh()->GetSkeletalMeshAsset();
        const auto SavedTuning=Companion->Tuning;
        const auto SavedMovementMode=Move->MovementMode;
        const auto SavedBodyMode=Ragdoll->GetControlMode();
        const bool bSavedWaitReturn=Companion->bReturningToWaitLocation;
        const auto ChangeBodyMode=[&](EDeliveryRagdollControlMode NewMode)
        {
            BodyMode->GetUnderlyingProperty()->SetIntPropertyValue(BodyMode->ContainerPtrToValuePtr<void>(Ragdoll),uint64(NewMode));
        };
        const auto SpawnBox=[&](const FVector& Location,const FVector& Extent)
        {
            auto* Actor=World->SpawnActor<AActor>();
            auto* Box=NewObject<UBoxComponent>(Actor);
            Actor->SetRootComponent(Box);
            Box->SetBoxExtent(Extent);
            Box->SetCollisionProfileName(TEXT("BlockAll"));
            Box->RegisterComponent();
            Actor->SetActorLocation(Location);
            return Actor;
        };
        const auto AdvanceTime=[&](float Seconds)
        {
            for (int32 Step=0;Step<FMath::CeilToInt(Seconds/.1f);++Step)
            {
                ++GFrameCounter;
                World->Tick(LEVELTICK_TimeOnly,.1f);
            }
        };
        AActor* Floor=nullptr;
        AActor* Wall=nullptr;
        ON_SCOPE_EXIT
        {
            if (IsValid(Wall)) Wall->Destroy();
            if (IsValid(Floor)) Floor->Destroy();
            Companion->Tuning=SavedTuning;
            Companion->bReturningToWaitLocation=bSavedWaitReturn;
            Npc->GetMesh()->SetSkeletalMeshAsset(SavedMesh);
            Move->SetMovementMode(SavedMovementMode);
            Npc->SetActorTransform(SavedNpcTransform,false,nullptr,ETeleportType::TeleportPhysics);
            Owner->SetActorTransform(SavedOwnerTransform,false,nullptr,ETeleportType::TeleportPhysics);
            ChangeBodyMode(SavedBodyMode);
        };
        Companion->Tuning=ProductionTuning;
        Npc->GetMesh()->SetSkeletalMeshAsset(ProductionMesh);
        Npc->SetActorLocationAndRotation(Ground+FVector(0,0,88),FRotator::ZeroRotator,false,nullptr,ETeleportType::TeleportPhysics);
        Owner->SetActorLocation(Ground+FVector(150,0,96),false,nullptr,ETeleportType::TeleportPhysics);
        Move->SetMovementMode(MOVE_Walking);
        // 沿原交互测试只配置身体可操作状态，用角色控制器校验真实长按，不启动身体物理
        ChangeBodyMode(EDeliveryRagdollControlMode::Active);
        if (!TestTrue(TEXT("放件前重新按真实委托接取同一原件"), Apply(EDeliveryNpcAction::TakePackage,OwnerState,Snapshot()))) return false;
        Companion->UpdateErrand();
        if (!TestEqual(TEXT("二狗已持有准备放下的原件"), Npc->GetCarriedTaskPackage(), Package)) return false;

        const auto BeforeDrop=Snapshot();
        TestEqual(TEXT("贴近放件使用当前能力版本"), BeforeDrop.RequestedCapabilityVersion,FDeliveryNpcDecisionRequest::CapabilityVersion);
        TestTrue(TEXT("主人有独立近处落地候选"), BeforeDrop.Candidates.ContainsByPredicate([](const auto& Candidate)
        { return Candidate.Action==EDeliveryNpcAction::DropPackage && Candidate.TravelMode==EDeliveryCompanionTravelMode::Unspecified; }));
        FDeliveryNpcDecisionRequest VisitorDrop;
        VisitorDrop.bPlayerRequest=true;
        Companion->ExtendDecisionRequest(VisitorDrop,StrangerState);
        TestFalse(TEXT("访客没有主人包裹的放地候选"), VisitorDrop.AllowedActions.Contains(EDeliveryNpcAction::DropPackage));
        TestFalse(TEXT("访客不能借主人的快照放地"), Apply(EDeliveryNpcAction::DropPackage,StrangerState,BeforeDrop));
        TestFalse(TEXT("主人不能接受已过期的放地快照"), Apply(EDeliveryNpcAction::DropPackage,OwnerState,BeforeTake));
        TestEqual(TEXT("未授权和旧命令保持修订"), Companion->GetCommandRevision(), BeforeDrop.CompanionCommandRevision);
        if (!TestTrue(TEXT("主人可安排把原件放地"), Apply(EDeliveryNpcAction::DropPackage,OwnerState,BeforeDrop))) return false;
        Companion->UpdateErrand();
        TestEqual(TEXT("没有支撑时不能放件"), Companion->GetErrand().Reason, FName(TEXT("NoSafePackageGround")));
        TestEqual(TEXT("无支撑时仍持同一件货"), Npc->GetCarriedTaskPackage(), Package);

        Floor=SpawnBox(Ground-FVector(0,0,25),FVector(350,350,25));
        Move->SetMovementMode(MOVE_Walking);
        Companion->UpdateErrand();
        if (!TestTrue(TEXT("有地面后先开始放件动画，仍未释放物品"),
            Companion->GetErrand().GroundPlacementServerTime>=0 && Npc->GetCarriedTaskPackage()==Package)) return false;
        Floor->Destroy();
        AdvanceTime(ProductionTuning->GroundPickupContactTime+.12f);
        Companion->UpdateErrand();
        TestTrue(TEXT("双方仍在近处但地面失效时清掉旧放件时间并保留原件"),
            Companion->GetErrand().Reason==TEXT("NoSafePackageGround") &&
            Companion->GetErrand().GroundPlacementServerTime<0 && Npc->GetCarriedTaskPackage()==Package);
        Floor=SpawnBox(Ground-FVector(0,0,25),FVector(350,350,25));
        Companion->UpdateErrand();
        if (!TestTrue(TEXT("支撑恢复后必须等待下一次动画接触"),
            Companion->GetErrand().GroundPlacementServerTime>=0 && Npc->GetCarriedTaskPackage()==Package)) return false;

        Wall=SpawnBox(Ground+FVector(74,0,110),FVector(5,340,110));
        AdvanceTime(ProductionTuning->GroundPickupContactTime+.12f);
        Companion->UpdateErrand();
        TestTrue(TEXT("墙挡住时取消已开始的放件，超过原接触时间也保留同一原件"),
            Companion->HasActiveErrand() && Companion->GetErrand().GroundPlacementServerTime<0 &&
            Npc->GetCarriedTaskPackage()==Package && Package->GetOwner()==Npc);
        Wall->Destroy();

        Companion->UpdateErrand();
        if (!TestTrue(TEXT("墙解除后重新开始放件，仍等待新的接触时间"),
            Companion->GetErrand().GroundPlacementServerTime>=0 && Npc->GetCarriedTaskPackage()==Package)) return false;
        AdvanceTime(ProductionTuning->GroundPickupContactTime+.12f);
        Companion->UpdateErrand();
        if (!TestTrue(TEXT("游戏确认同一原件落地并可再次拾取"),
            !Companion->HasActiveErrand() && Companion->GetErrand().Reason==TEXT("PackagePlacedOnGround") &&
            IsValid(Package) && !Package->GetOwner() && !Package->GetAttachParentActor() &&
            Package->GetActorEnableCollision() && !Package->IsHidden() && !Npc->GetCarriedTaskPackage() &&
            !Owner->GetCarriedTaskPackage() && Package->GetDeliveryItemComponent()->OwningTask==Task &&
            FVector::Dist2D(Package->GetActorLocation(),Owner->GetActorLocation())<=180.f)) return false;
        CheckNotDelivered();

        const auto SendPickup=[&](EDeliveryInteractionRequest Request)
        { OwnerController->ServerRequestInteraction_Implementation(Owner,Request,Package); };
        if (!TestTrue(TEXT("主人的原 E 拾取条件可识别地上原件"), Owner->GetInteractionComponent()->CanUse(Package))) return false;
        SendPickup(EDeliveryInteractionRequest::CompleteHold);
        TestNull(TEXT("跳过长按不能捡回"), Owner->GetCarriedTaskPackage());
        SendPickup(EDeliveryInteractionRequest::BeginHold);
        AdvanceTime(.6f);
        SendPickup(EDeliveryInteractionRequest::CompleteHold);
        if (!TestTrue(TEXT("按原长按入口捡回同一包裹"),
            Owner->GetCarriedTaskPackage()==Package && Package->GetOwner()==Owner)) return false;
        CheckNotDelivered();

        TGuardValue<FName> FirstOrderId(Task->TaskId,FName(TEXT("Task_001")));
        auto* Inventory=Owner->GetInventoryComponent();
        auto* PackageMesh=Package->GetItemMesh();
        const auto PutDownOriginal=[&]()
        {
            Inventory->RequestSlotAction(0);
            if (Inventory->GetDroppedFirstOrderPackage()!=Package) return false;
            // 此世界不推进刚体，沿地面落稳后的真实物体位置建立可重复的接触条件
            const FVector Settled=Ground+FVector(76,0,PackageMesh->Bounds.BoxExtent.Z+1.);
            Package->SetActorLocation(Settled,false,nullptr,ETeleportType::ResetPhysics);
            PackageMesh->SetPhysicsLinearVelocity(FVector::ZeroVector);
            return Inventory->GetDroppedFirstOrderPackage()==Package && !Package->GetOwner();
        };
        if (!TestTrue(TEXT("主人从真实手持槽主动放下首单并保留原件记录"),PutDownOriginal())) return false;
        const auto GroundTake=Snapshot();
        TestTrue(TEXT("当前主人可从地面取回原件或取来交还"),GroundTake.CompanionPackage.Get()==Package &&
            !GroundTake.bCompanionOwnerHasPackage && GroundTake.AllowedActions.Contains(EDeliveryNpcAction::TakePackage) &&
            GroundTake.AllowedActions.Contains(EDeliveryNpcAction::FetchPackage));
        TestFalse(TEXT("访客不能用主人放下的包裹快照拾地上货物"),Apply(EDeliveryNpcAction::TakePackage,StrangerState,GroundTake));
        if (!TestTrue(TEXT("主人可以指示从地面拿起原件"),Apply(EDeliveryNpcAction::TakePackage,OwnerState,GroundTake))) return false;
        Companion->UpdateErrand();
        if (!TestTrue(TEXT("先播放地面拾取动作，接触前原件仍在地面"),Companion->Errand.GroundPickupServerTime>=0 &&
            Inventory->GetDroppedFirstOrderPackage()==Package && !Npc->GetCarriedTaskPackage())) return false;
        Wall=SpawnBox(Ground+FVector(51,0,90),FVector(3,160,90));
        AdvanceTime(ProductionTuning->GroundPickupContactTime+.12f);
        Companion->UpdateErrand();
        TestTrue(TEXT("接触前出现遮挡会撤销拾取，地面保留同一原件"),Companion->HasActiveErrand() &&
            Companion->Errand.GroundPickupServerTime<0 && Inventory->GetDroppedFirstOrderPackage()==Package && !Npc->GetCarriedTaskPackage());
        Wall->Destroy();
        Companion->UpdateErrand();
        if (!TestTrue(TEXT("移开遮挡后重新等待地面接触"),Companion->Errand.GroundPickupServerTime>=0 && !Npc->GetCarriedTaskPackage())) return false;
        AdvanceTime(ProductionTuning->GroundPickupAnimation->GetPlayLength()+.1f);
        Companion->UpdateErrand();
        TestTrue(TEXT("接触时只拿起主人放下的同一包裹，旧落地提示清除"),Npc->GetCarriedTaskPackage()==Package &&
            Package->GetOwner()==Npc && !Inventory->GetDroppedFirstOrderPackage());
        Companion->UpdateErrand();
        TestTrue(TEXT("只取件结束后仍由二狗持有原件"),!Companion->HasActiveErrand() &&
            Companion->Errand.Reason==TEXT("PackagePickedUpFromGround") && Npc->GetCarriedTaskPackage()==Package);
        CheckNotDelivered();

        if (!TestTrue(TEXT("主人能另行要求交还这件原件"),Apply(EDeliveryNpcAction::ReturnPackage,OwnerState,Snapshot()))) return false;
        Companion->UpdateErrand();
        if (!TestTrue(TEXT("交还后主人可再次真实放下同一原件"),Owner->GetCarriedTaskPackage()==Package && PutDownOriginal())) return false;
        const auto HasImmediateFetch=[](const FDeliveryNpcDecisionRequest& Request)
        {
            return Request.Candidates.ContainsByPredicate([](const auto& Candidate)
            { return Candidate.Action==EDeliveryNpcAction::FetchPackage && Candidate.TravelMode==EDeliveryCompanionTravelMode::Unspecified; });
        };
        const FVector NearbyOwner=Owner->GetActorLocation();
        Owner->SetActorLocation(NearbyOwner+FVector(0,400,0),false,nullptr,ETeleportType::TeleportPhysics);
        TestFalse(TEXT("原件近而主人远时取来交还仍需要出行方式"),HasImmediateFetch(Snapshot()));
        Owner->SetActorLocation(NearbyOwner,false,nullptr,ETeleportType::TeleportPhysics);
        const auto GroundFetch=Snapshot();
        if (!TestTrue(TEXT("主人和地上原件都在近处时可直接安排拾取并交还"),HasImmediateFetch(GroundFetch) &&
            Apply(EDeliveryNpcAction::FetchPackage,OwnerState,GroundFetch))) return false;
        Companion->UpdateErrand();
        AdvanceTime(ProductionTuning->GroundPickupAnimation->GetPlayLength()+.1f);
        Companion->UpdateErrand();
        Companion->UpdateErrand();
        if (!TestTrue(TEXT("地面 Fetch 拾到后进入返还原主人的实物阶段"),Companion->GetErrand().Phase==EDeliveryCompanionErrandPhase::ReturnToOwner &&
            Npc->GetCarriedTaskPackage()==Package)) return false;
        Companion->UpdateErrand();
        if (!TestTrue(TEXT("Fetch 把地面同一原件带回主人手里"),!Companion->HasActiveErrand() &&
            Companion->Errand.Reason==TEXT("PackageReturned") && Owner->GetCarriedTaskPackage()==Package && Package->GetOwner()==Owner)) return false;
        CheckNotDelivered();

        if (!TestTrue(TEXT("主人先放下原件再安排拾取"),PutDownOriginal() && Apply(EDeliveryNpcAction::TakePackage,OwnerState,Snapshot()))) return false;
        Companion->UpdateErrand();
        if (!TestTrue(TEXT("接触前主人实际重新拾起原件"),Companion->Errand.GroundPickupServerTime>=0 && Owner->TryReceiveTaskPackage(Package))) return false;
        Companion->UpdateErrand();
        TestTrue(TEXT("原件在接触前离开地面时结束旧安排并保留实际持有人"),!Companion->HasActiveErrand() &&
            Companion->Errand.Reason==TEXT("GroundPackageUnavailable") && Owner->GetCarriedTaskPackage()==Package &&
            Package->GetOwner()==Owner && !Npc->GetCarriedTaskPackage());
        CheckNotDelivered();
    }

    // 地点通过实际组件注册，包裹丢失后仍有合法的取件与帮送候选
    const auto MakeLocation = [&](FName Id, const FVector& Position)
    {
        auto* Actor = World->SpawnActor<AActor>();
        if (!Actor) return static_cast<AActor*>(nullptr);
        auto* Location = NewObject<UDeliveryLocationComponent>(Actor);
        Location->LocationId = Id;
        Actor->SetRootComponent(Location);
        Actor->AddInstanceComponent(Location);
        Location->RegisterComponent();
        Actor->SetActorLocation(Position);
        return Actor;
    };
    auto* PickupActor = MakeLocation(Task->PickupLocationId, FVector(1000, 0, 88));
    auto* AddressActor = MakeLocation(Task->DeliveryLocationId, FVector(2000, 0, 88));
    auto* DeliveryActor = MakeLocation(Task->ReceiverNpcId, FVector(2300, 0, 88));
    if (!TestNotNull(TEXT("登记真实取件地点"), PickupActor) || !TestNotNull(TEXT("登记真实收件地点"), DeliveryActor)) return false;
    if (!TestNotNull(TEXT("登记单独的门外地址"), AddressActor)) return false;
    TestFalse(TEXT("只有门外地址时没有合法的送件候选"), Companion->CanArrangeErrand(EDeliveryNpcAction::DeliverPackage, OwnerState));
    {
        using EDoorState=EDeliveryCompanionTaskDoorState;
        // 按正式房屋的门高与导航端点布置，订单和包裹沿用上文的真实状态
        const FVector Porch(9000,10000,2000);
        FActorSpawnParameters Spawn;
        Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* Door=World->SpawnActor<ADeliveryBreakableDoor>(Porch+FVector(0,108,324),FRotator(0,90,0),Spawn);
        if (!TestTrue(TEXT("测试门使用正式门网格"),IsValid(Door) && Door->GetIntactMesh()->GetStaticMesh())) return false;
        const FTransform GrandpaTransform(Porch+FVector(100,340,58));
        auto* Grandpa=World->SpawnActorDeferred<ADeliveryGrandpa>(ADeliveryGrandpa::StaticClass(),GrandpaTransform,
            nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if (!TestNotNull(TEXT("创建第一单实际收件角色"),Grandpa)) return false;
        Grandpa->AIControllerClass=nullptr;
        Grandpa->FrontDoor=Door;
        Grandpa->DeliveryTarget->ExpectedTask=Task;
        auto* ReceiverLocation=NewObject<UDeliveryLocationComponent>(Grandpa);
        ReceiverLocation->LocationId=Task->ReceiverNpcId;
        ReceiverLocation->SetupAttachment(Grandpa->GetRootComponent());
        Grandpa->AddInstanceComponent(ReceiverLocation);
        ReceiverLocation->RegisterComponent();
        Grandpa->FinishSpawning(GrandpaTransform);
        const FTransform HomeTransform(Porch);
        auto* Home=World->SpawnActorDeferred<ADeliveryHome>(ADeliveryHome::StaticClass(),HomeTransform,
            nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if (!TestNotNull(TEXT("登记爷爷所在的同一房屋"),Home)) return false;
        Home->FormerResident=Grandpa;
        Grandpa->SharedHome=Home;
        Home->FinishSpawning(HomeTransform);
        const auto SavedOwner=Owner->GetActorTransform();
        const auto SavedNpc=Npc->GetActorTransform();
        ON_SCOPE_EXIT
        {
            Owner->SetActorTransform(SavedOwner,false,nullptr,ETeleportType::TeleportPhysics);
            Npc->SetActorTransform(SavedNpc,false,nullptr,ETeleportType::TeleportPhysics);
            Home->Destroy(); Grandpa->Destroy();
            if (IsValid(Door)) Door->Destroy();
        };
        TestEqual(TEXT("远处不提供实时门况"),Snapshot().CompanionTaskDoorState,EDoorState::Unknown);
        const auto* Link=Home->DoorwayLink.Get();
        if (!TestTrue(TEXT("房屋登记了原步行门洞连接"),IsValid(Link) && Link->IsRegistered())) return false;
        const FVector LinkStart=Link->GetStartPoint();
        const FVector LinkEnd=Link->GetEndPoint();
        const FVector DoorAxis=(LinkEnd-LinkStart).GetSafeNormal2D();
        const FVector DoorSide=FVector::CrossProduct(FVector::UpVector,DoorAxis);
        const FBoxSphereBounds DoorBounds=Door->GetIntactMesh()->Bounds;
        const double DoorWidthOffset=FVector::DotProduct(DoorBounds.Origin-LinkStart,DoorSide);
        const FVector StreetFeet=LinkStart+DoorSide*DoorWidthOffset;
        const FVector RoomFeet=LinkEnd+DoorSide*DoorWidthOffset;
        const FVector Bypass=DoorSide*(FVector::DotProduct(DoorBounds.BoxExtent,DoorSide.GetAbs())+160.);
        TestFalse(TEXT("完整门保持原步行连接关闭"),Link->IsEnabled());
        TestTrue(TEXT("完整门也标记为专属车不能穿越的步行通道"),Home->CrossesPedestrianDoorway(StreetFeet,RoomFeet));
        const FVector Outside=Porch-FVector(0,60,0);
        Owner->SetActorLocation(Outside+FVector(-70,0,Owner->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()),false,nullptr,ETeleportType::TeleportPhysics);
        Npc->SetActorLocation(Outside+FVector(70,0,Npc->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()),false,nullptr,ETeleportType::TeleportPhysics);
        const FVector DoorWaitAnchor=Home->ReturnAnchor->GetComponentLocation();
        const double DoorClearance=Companion->GetTuning().DestinationClearance;
        const FVector SafeDoorWait=DoorWaitAnchor-DoorAxis*(DoorClearance+55.)+DoorSide*80.;
        TestFalse(TEXT("代送等候不能站在敲门与挥斧的地址中心"),Companion->IsSafeDeliveryDoorWait(*Home,DoorWaitAnchor));
        TestTrue(TEXT("门外同层且让出入口净空的位置可以等候"),Companion->IsSafeDeliveryDoorWait(*Home,SafeDoorWait));
        TestFalse(TEXT("离地址足够远的屋内位置仍不是街侧等候点"),
            Companion->IsSafeDeliveryDoorWait(*Home,DoorWaitAnchor+DoorAxis*(DoorClearance+100.)));
        TestFalse(TEXT("与街侧等候点 XY 相同的另一层不能等候"),
            Companion->IsSafeDeliveryDoorWait(*Home,SafeDoorWait+FVector(0,0,240)));
        const FVector NearDoorClearance=DoorWaitAnchor-DoorAxis*(DoorClearance+10.);
        TestTrue(TEXT("刚超出入口净空的实际脚点可用"),Companion->IsSafeDeliveryDoorWait(*Home,NearDoorClearance));
        TestFalse(TEXT("寻路接受容差不能让预计等候点停进净空范围"),
            Companion->IsSafeDeliveryDoorWait(*Home,NearDoorClearance,20.f));
        const FVector OwnerAtDoor=Owner->GetActorLocation();
        const double OwnerGap=Companion->GetTuning().YieldDistance-12.;
        const FVector OwnerNearWait=SafeDoorWait+DoorSide*OwnerGap+
            FVector(0,0,Owner->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
        Owner->SetActorLocation(OwnerNearWait,false,nullptr,ETeleportType::TeleportPhysics);
        TestFalse(TEXT("主人站在个人间距内时不选此等候点"),Companion->IsSafeDeliveryDoorWait(*Home,SafeDoorWait));
        Owner->SetActorLocation(OwnerAtDoor,false,nullptr,ETeleportType::TeleportPhysics);
        auto Intact=Snapshot();
        TestEqual(TEXT("主人问当前订单时看到真正的完整门叶"),Intact.CompanionTaskDoorState,EDoorState::Intact);
        TestTrue(TEXT("v8 请求发送真实门况"),DeliveryNpc::EncodeRequest(Intact).Contains(TEXT("\"task_door_state\"")));
        Intact.RequestedCapabilityVersion=7;
        TestFalse(TEXT("旧版请求省略新字段"),DeliveryNpc::EncodeRequest(Intact).Contains(TEXT("\"task_door_state\"")));
        FDeliveryNpcDecisionRequest StrangerRequest;
        StrangerRequest.bPlayerRequest=true;
        Companion->ExtendDecisionRequest(StrangerRequest,StrangerState);
        TestEqual(TEXT("访客不获得主人订单门况"),StrangerRequest.CompanionTaskDoorState,EDoorState::Unknown);

        Grandpa->DeliveryTarget->ExpectedTask=NextTask;
        TestEqual(TEXT("别的订单收件人不能提供本单门况"),Snapshot().CompanionTaskDoorState,EDoorState::Unknown);
        Grandpa->DeliveryTarget->ExpectedTask=Task;
        auto* Wall=World->SpawnActor<AActor>();
        auto* WallCollision=NewObject<UBoxComponent>(Wall);
        Wall->SetRootComponent(WallCollision);
        WallCollision->SetBoxExtent(FVector(60,10,95));
        WallCollision->SetCollisionProfileName(TEXT("BlockAll"));
        WallCollision->RegisterComponent();
        FVector Eye; FRotator EyeRotation;
        Npc->GetActorEyesViewPoint(Eye,EyeRotation);
        Wall->SetActorLocation((Eye+Door->GetIntactMesh()->Bounds.Origin)*.5);
        TestEqual(TEXT("另一物体遮住门叶后省略观察"),Snapshot().CompanionTaskDoorState,EDoorState::Unknown);
        WallCollision->SetCollisionObjectType(ECC_WorldDynamic);
        WallCollision->SetBoxExtent(FVector(38,38,60));
        Wall->SetActorLocation(SafeDoorWait+FVector(0,0,Npc->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()));
        TestFalse(TEXT("移动到等候点的实体会阻挡二狗胶囊"),Companion->IsSafeDeliveryDoorWait(*Home,SafeDoorWait));
        Wall->Destroy();
        TestTrue(TEXT("实体移开后可重新选择原街侧等候点"),Companion->IsSafeDeliveryDoorWait(*Home,SafeDoorWait));

        if (!TestTrue(TEXT("服务器确实将门切换为破碎"),Door->BreakFromAxe())) return false;
        TestTrue(TEXT("破门只开放原步行连接的专用区域"),Link->IsEnabled() &&
            Link->GetLinkAreaClass()==UDeliveryPedestrianLinkArea::StaticClass());
        TestTrue(TEXT("破门后仍识别外向内跨越步行门洞"),Home->CrossesPedestrianDoorway(StreetFeet,RoomFeet));
        TestTrue(TEXT("破门后仍识别内向外跨越步行门洞"),Home->CrossesPedestrianDoorway(RoomFeet,StreetFeet));
        TestFalse(TEXT("同一层离开门叶横向范围的绕行不会误判"),Home->CrossesPedestrianDoorway(StreetFeet+Bypass,RoomFeet+Bypass));
        TestFalse(TEXT("沿门外同一侧朝街上移动不会误判"),Home->CrossesPedestrianDoorway(StreetFeet,StreetFeet-DoorAxis*100.));
        TestFalse(TEXT("在门外平行经过不会误判"),Home->CrossesPedestrianDoorway(StreetFeet-Bypass,StreetFeet+Bypass));
        const FVector Above(0,0,240);
        TestFalse(TEXT("同一 XY 的高层行程不会误判"),Home->CrossesPedestrianDoorway(StreetFeet+Above,RoomFeet+Above));
        TestEqual(TEXT("下一次询问读取实物已经破开"),Snapshot().CompanionTaskDoorState,EDoorState::Broken);
        TestEqual(TEXT("破门不能推进行为或结算旧委托"),Tasks->GetTaskStatus(Task),EDeliveryTaskStatus::InProgress);
        Owner->SetActorLocation(Owner->GetActorLocation()+FVector(0,0,220),false,nullptr,ETeleportType::TeleportPhysics);
        TestEqual(TEXT("另一楼层的主人不获得现门况"),Snapshot().CompanionTaskDoorState,EDoorState::Unknown);
    }
    auto* Pickup = NewObject<UDeliveryPickupPointComponent>(PickupActor);
    PickupActor->AddInstanceComponent(Pickup);
    Pickup->RegisterComponent();
    auto* Delivery = NewObject<UDeliveryTargetComponent>(DeliveryActor);
    Delivery->ExpectedTask = Task;
    DeliveryActor->AddInstanceComponent(Delivery);
    Delivery->RegisterComponent();
    auto* Tracker = UDeliveryTaskTrackerComponent::FindTracker(OwnerState);
    if (!TestNotNull(TEXT("主人拥有真实任务追踪组件"), Tracker)) return false;
    Tracker->RequestTrackTask(Task);
    TestEqual(TEXT("持件丢失后仍追踪同一订单"), Tracker->GetTrackedTask(), Task);

    auto* Agent = Npc->Agent.Get();
    const auto MakeModelReply=[&](const FDeliveryNpcDecisionRequest& Request,const FDeliveryNpcCandidate& Candidate)
    {
        // 只替代模型传输，实际候选、采纳与执行都走正常入口
        Agent->ActiveRequest=Request;
        Agent->bDecisionPending=true;
        FDeliveryNpcDecisionProposal Proposal;
        Proposal.CapabilityVersion=Request.RequestedCapabilityVersion;
        Proposal.CandidateId=Candidate.CandidateId;
        Proposal.WorldEpoch=Request.WorldEpoch;
        Proposal.NpcId=Request.NpcId;
        Proposal.RequestId=Request.RequestId;
        Proposal.ContextRevision=Request.ContextRevision;
        Proposal.PlanRevision=Request.PlanRevision;
        Proposal.Action=Candidate.Action;
        Proposal.TargetId=Candidate.TargetId;
        Proposal.EvidenceIds=Candidate.EvidenceIds;
        Proposal.Duration=DeliveryNpc::CandidateDuration(Candidate.Action);
        return Proposal;
    };
    const auto PrepareDeliveryReply = [&](FDeliveryNpcDecisionRequest& Request, FDeliveryNpcDecisionProposal& Proposal)
    {
        FDeliveryNpcObservation Speech;
        Speech.EventId = FGuid::NewGuid();
        Speech.Kind = TEXT("PlayerSpeech");
        Speech.Subject = Owner;
        Speech.Addressee = Npc;
        Speech.Source = EDeliveryNpcEvidenceSource::Heard;
        Speech.Description = TEXT("帮我把这单送给收件人");
        Speech.bNeedsReply = true;
        Agent->BeginListening(Owner, Speech.EventId);
        if (!Agent->ObserveEvent(Speech) || !Agent->BuildDecisionRequest(FGuid::NewGuid(), Request)) return false;
        const auto* Candidate = Request.Candidates.FindByPredicate([](const auto& Item)
        { return Item.Action == EDeliveryNpcAction::DeliverPackage && !Item.TargetId.IsValid() && Item.TravelMode == EDeliveryCompanionTravelMode::Walk; });
        if (!Candidate) return false;
        Proposal=MakeModelReply(Request,*Candidate);
        return true;
    };

    FDeliveryNpcDecisionRequest BeforeDestroy;
    FDeliveryNpcDecisionProposal OldDeliveryReply;
    if (!TestTrue(TEXT("实际持件时生成主人 Deliver 请求与合法候选"), PrepareDeliveryReply(BeforeDestroy, OldDeliveryReply))) return false;
    const auto* IndexedCandidate=BeforeDestroy.FindCandidate(OldDeliveryReply.CandidateId);
    TestTrue(TEXT("正式请求按编号索引到同一送件候选"),IndexedCandidate &&
        IndexedCandidate->Action==OldDeliveryReply.Action && IndexedCandidate->EvidenceIds==OldDeliveryReply.EvidenceIds);
    TestNull(TEXT("其他候选编号不能命中当前请求"),BeforeDestroy.FindCandidate(FGuid::NewGuid()));
    TestEqual(TEXT("模型请求确实捕获包裹 A"), BeforeDestroy.CompanionPackage.Get(), Package);
    const int32 BeforeDestroyedReplyRevision = Companion->GetCommandRevision();
    AddExpectedMessagePlain(FString::Printf(TEXT("[Task] %s 的快递丢失"), *Task->TaskId.ToString()), ELogVerbosity::Warning);
    if (!TestTrue(TEXT("直接销毁主人手中的包裹 A，不预先清槽或释放"), Package->Destroy())) return false;
    TestFalse(TEXT("旧请求的包裹已经失效"), BeforeDestroy.CompanionPackage.IsValid());
    TestNull(TEXT("主人携带查询过滤已销毁的包裹"), Owner->GetCarriedTaskPackage());
    TestFalse(TEXT("已销毁包裹不会继续占用主人手持槽"), Owner->GetInventoryComponent()->HasHeldItem());
    TestEqual(TEXT("真实丢件生命周期退回待取件"), Tasks->GetTaskStatus(Task), EDeliveryTaskStatus::AwaitingPickup);
    TestEqual(TEXT("丢件不取消接听后的原订单计时"), Tasks->GetTimedTask(), Task);
    TestEqual(TEXT("取件点现在可以重新发出同一订单"), Pickup->ResolvePendingTask(), Task);
    const auto AfterDestroy = Snapshot();
    TestNull(TEXT("新的上下文不保留主人槽内的失效实物"), AfterDestroy.CompanionPackage.Get());
    TestTrue(TEXT("丢件后从取件点帮送仍是合法动作"), Companion->CanArrangeErrand(EDeliveryNpcAction::DeliverPackage, OwnerState));
    // 验证 ReceiveDecision 实际产生的拒绝原因，不能只对弱指针运算符作断言
    AddExpectedMessagePlain(FString::Printf(TEXT("request=%s accepted=0 reason=CompanionTaskOrPackageChanged"),
        *BeforeDestroy.RequestId.ToString()), ELogVerbosity::Display);
    Agent->ReceiveDecision(BeforeDestroy.RequestId, &OldDeliveryReply, NAME_None);
    TestFalse(TEXT("旧 Deliver 回复没有被 Agent 接受"), Agent->HasActiveAction());
    TestFalse(TEXT("旧回复不能把已销毁包裹的安排改成重新取件"), Companion->HasActiveErrand());
    TestFalse(TEXT("拒绝后结束本次等待"), Agent->IsDecisionPending());
    TestEqual(TEXT("拒绝旧实物回复不改主人安排版本"), Companion->GetCommandRevision(), BeforeDestroyedReplyRevision);
    TestEqual(TEXT("拒绝旧回复不自动重新取件"), Tasks->GetTaskStatus(Task), EDeliveryTaskStatus::AwaitingPickup);
    TestNull(TEXT("拒绝旧回复不制造同伴持件"), Npc->GetCarriedTaskPackage());
    TestEqual(TEXT("拒绝旧回复不发奖"), OwnerWallet->GetBalance(), OriginalBalance);

    FDeliveryNpcDecisionRequest EmptyPackageRequest;
    FDeliveryNpcDecisionProposal FreshDeliveryReply;
    if (!TestTrue(TEXT("包裹已空时重新生成真实 Deliver 请求"), PrepareDeliveryReply(EmptyPackageRequest, FreshDeliveryReply))) return false;
    TestNull(TEXT("新请求的包裹确实为空"), EmptyPackageRequest.CompanionPackage.Get());
    AddExpectedMessagePlain(FString::Printf(TEXT("request=%s accepted=1 reason=None"),
        *EmptyPackageRequest.RequestId.ToString()), ELogVerbosity::Display);
    Agent->ReceiveDecision(EmptyPackageRequest.RequestId, &FreshDeliveryReply, NAME_None);
    TestEqual(TEXT("空快照保持为空时正常采纳新安排"), Agent->GetPublicState().Status, EDeliveryNpcActionStatus::Accepted);
    TestEqual(TEXT("采纳的是新请求中的 Deliver"), Agent->GetPublicState().Action, EDeliveryNpcAction::DeliverPackage);
    TestNull(TEXT("新安排不会恢复旧实物指针"), Agent->GetPublicState().CompanionPackage.Get());
    TestEqual(TEXT("新安排仍属于原主人"), Agent->GetPublicState().Requester.Get(), static_cast<APlayerState*>(OwnerState));
    TestFalse(TEXT("采纳尚未执行，不冒充已经取件"), Companion->HasActiveErrand());
    Agent->CancelPlan(TEXT("SnapshotRegressionFinished"));

    // 不手动清槽，用新实物的真实接收验证失效物品不会阻塞后续交接
    Package = World->SpawnActor<ADeliveryTestPackage>(FVector(0, 0, 150), FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("创建丢件后的新实物"), Package)) return false;
    Package->GetDeliveryItemComponent()->OwningTask = Task;
    if (!TestTrue(TEXT("新实物经过真实接收入口恢复配送"), Owner->TryReceiveTaskPackage(Package))) return false;
    CheckNotDelivered();

    // 真实订单与地点组件构成院子机会，不伪造主人发言或已取得道具
    Npc->EncounterTaskId=Task->TaskId;
    Companion->UpdateCaterpillarOpportunity();
    Npc->SetActorLocation(FVector(1800,0,88),false,nullptr,ETeleportType::TeleportPhysics);
    Owner->SetActorLocation(FVector(1700,0,88),false,nullptr,ETeleportType::TeleportPhysics);
    // 原生夹具没有 RightHand 骨骼，临时把原挂点摆在主人手边，防止回退到脚下的网格原点
    auto* OwnerGrip=Owner->GetHeldItemAnchor();
    const FTransform SavedWormFixtureGrip=OwnerGrip->GetRelativeTransform();
    OwnerGrip->SetWorldLocation(Owner->GetActorLocation()+FVector(22,0,16));
    Owner->ApplyTaskPackagePresentation(Package);
    Companion->UpdateCaterpillarOpportunity();
    TestFalse(TEXT("院子没有真实小绿时无提议"),Companion->CanOfferCaterpillar(OwnerState));
    UClass* WormClass=LoadClass<ADeliveryCaterpillarItem>(nullptr,TEXT("/Game/Blueprint/Item/BP_Caterpillar.BP_Caterpillar_C"));
    if (!TestNotNull(TEXT("测试使用正式小绿资源"),WormClass)) return false;
    const auto* WormStock=WormClass->GetDefaultObject<ADeliveryCaterpillarItem>();
    UStaticMesh* WormAsset=WormStock && WormStock->GetItemMesh()?WormStock->GetItemMesh()->GetStaticMesh().Get():nullptr;
    if (!TestTrue(TEXT("正式小绿蓝图有原静态网格"),IsValid(WormAsset))) return false;
    // 独立规则世界没有正常地图加载阶段，等正式网格完成异步编译后才创建物理实例
    UStaticMesh* WormAssets[]={WormAsset};
    FStaticMeshCompilingManager::Get().FinishCompilation(WormAssets);
    if (!TestFalse(TEXT("创建小绿实体前原网格已编译完成"),WormAsset->IsCompiling())) return false;
    auto* WormBodySetup=WormAsset->GetBodySetup();
    if (!TestTrue(TEXT("正式小绿原网格有可用简单碰撞"),WormBodySetup && WormBodySetup->AggGeom.GetElementCount()>0 &&
        WormBodySetup->GetCollisionTraceFlag()!=CTF_UseComplexAsSimple)) return false;
    WormBodySetup->CreatePhysicsMeshes();
    FActorSpawnParameters WormSpawn;
    WormSpawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    const FVector YardSpot=Owner->GetActorLocation()+FVector(0,75,-35);
    const bool bDeferredPhysics=World->GetAllowDeferredPhysicsStateCreation();
    World->SetAllowDeferredPhysicsStateCreation(false);
    auto* YardWorm=World->SpawnActor<ADeliveryCaterpillarItem>(WormClass,YardSpot,FRotator::ZeroRotator,WormSpawn);
    World->SetAllowDeferredPhysicsStateCreation(bDeferredPhysics);
    if (!TestNotNull(TEXT("院子登记真实地面小绿"),YardWorm)) return false;
    // 此独立世界不推进刚体，使用实物落地入口建立与正式关卡落稳后相同的交互状态
    YardWorm->DropFromInventory(YardSpot,FVector::ZeroVector);
    auto* WormMesh=YardWorm->GetItemMesh();
    if (!TestTrue(TEXT("生成的正式小绿保留根网格组件"),WormMesh && WormMesh==YardWorm->GetRootComponent())) return false;
    const auto* WormBody=WormMesh->GetBodyInstance(NAME_None,false);
    if (!WormBody || !WormBody->IsValidBodyInstance())
    {
        WormMesh->RecreatePhysicsState();
        WormBody=WormMesh->GetBodyInstance(NAME_None,false);
    }
    AddInfo(FString::Printf(TEXT("小绿 CDO/实际网格=%s/%s 可取标记=%d/%d 碰撞=%d 活动性=%d 要求模拟=%d 有效刚体=%d"),
        *GetNameSafe(WormStock->GetItemMesh()->GetStaticMesh()),*GetNameSafe(WormMesh->GetStaticMesh()),
        WormStock->GetItemComponent()->bCompanionCanFetch,YardWorm->GetItemComponent()->bCompanionCanFetch,
        int32(WormMesh->GetCollisionEnabled()),int32(WormMesh->Mobility),
        WormBody && WormBody->ShouldInstanceSimulatingPhysics(),WormBody && WormBody->IsValidBodyInstance()));
    const bool bWormFetchable=TestTrue(TEXT("生成的正式小绿保留同伴可取标记"),YardWorm->GetItemComponent()->bCompanionCanFetch);
    const bool bWormMesh=TestTrue(TEXT("生成的正式小绿保留网格资产"),IsValid(WormMesh->GetStaticMesh()));
    const bool bWormUnowned=TestNull(TEXT("实物落地后没有持有者"),YardWorm->GetOwner());
    if (!bWormFetchable || !bWormMesh || !bWormUnowned) return false;
    WormMesh->SetSimulatePhysics(false);
    if (!TestTrue(TEXT("静置小绿仍有可查询实物碰撞和实际拾取入口"),!WormMesh->IsSimulatingPhysics() &&
        DeliveryHandheldTransfer::FindWorldPickupInteraction(YardWorm,World))) return false;
    if (!TestEqual(TEXT("主人近处实际可达登记小绿"),Companion->FindReachableYardCaterpillar(Owner,180.f),YardWorm)) return false;
    if (!TestTrue(TEXT("真实原包裹与近处小绿满足提议条件"),Companion->CanOfferCaterpillar(OwnerState))) return false;
    Companion->UpdateCaterpillarOpportunity();
    if (!TestTrue(TEXT("服务器在当前现场登记小绿提议机会"),Companion->HasCaterpillarOpportunity())) return false;
    FDeliveryNpcDecisionRequest OpportunityRequest;
    if (!TestTrue(TEXT("院子机会经正常 Agent 构建主动请求"),Agent->BuildDecisionRequest(FGuid::NewGuid(),OpportunityRequest))) return false;
    TestFalse(TEXT("主动机会不伪造主人发言身份"),OpportunityRequest.bRequesterIsCompanionOwner);
    TestTrue(TEXT("主动机会只开放小绿提议"),OpportunityRequest.AllowedActions.Contains(EDeliveryNpcAction::OfferCaterpillar));
    TestFalse(TEXT("主动提议不能自行拿小绿"),OpportunityRequest.AllowedActions.Contains(EDeliveryNpcAction::CollectCaterpillar));
    TestFalse(TEXT("主动提议不能自行投放"),OpportunityRequest.AllowedActions.Contains(EDeliveryNpcAction::InsertCaterpillar));
    TestFalse(TEXT("主动机会不能改跟随安排"),OpportunityRequest.AllowedActions.Contains(EDeliveryNpcAction::Follow));
    const auto* Offer=OpportunityRequest.Candidates.FindByPredicate([](const auto& Candidate)
        { return Candidate.Action==EDeliveryNpcAction::OfferCaterpillar && !Candidate.TargetId.IsValid(); });
    if (!TestNotNull(TEXT("小绿候选实际存在"),Offer)) return false;
    FDeliveryNpcDecisionProposal OfferReply;
    OfferReply.CapabilityVersion=OpportunityRequest.RequestedCapabilityVersion;
    OfferReply.CandidateId=Offer->CandidateId; OfferReply.TargetId=Offer->TargetId;
    OfferReply.WorldEpoch=OpportunityRequest.WorldEpoch; OfferReply.NpcId=OpportunityRequest.NpcId;
    OfferReply.RequestId=OpportunityRequest.RequestId; OfferReply.ContextRevision=OpportunityRequest.ContextRevision;
    OfferReply.PlanRevision=OpportunityRequest.PlanRevision; OfferReply.Action=Offer->Action;
    OfferReply.EvidenceIds=Offer->EvidenceIds; OfferReply.Duration=0;
    Agent->ActiveRequest=OpportunityRequest; Agent->bDecisionPending=true;
    Agent->ReceiveDecision(OpportunityRequest.RequestId,&OfferReply,NAME_None);
    FDeliveryNpcPublicState OfferState;
    if (!TestTrue(TEXT("模型提议经真实采纳与开始复核"),Agent->StartAction(OfferState)) ||
        !TestTrue(TEXT("模型动作只开放接物交互"),Npc->ExecuteAction(OfferState))) return false;
    Agent->TryFinishAction(OfferState.ActionId,EDeliveryNpcActionStatus::Succeeded,NAME_None);
    Npc->Tick(0);
    auto* EAction=UDeliveryInteractableComponent::FindOn(Npc,EDeliveryInteractionKey::PickupE);
    if (!TestEqual(TEXT("结识后仍使用同一个 E 入口"),EAction,Npc->HelpInteractable.Get())) return false;
    TestEqual(TEXT("接小绿不沿用扶起的长按计时"),EAction->HoldDuration,0.f);
    TestFalse(TEXT("模型提议没有把虫放进外卖"),Package->ContainsCaterpillar());
    TestFalse(TEXT("访客不能从主人同伴处接走小绿"),EAction->CanUse.Execute(Stranger,true));
    if (!TestTrue(TEXT("主人实际交互条件允许接小绿"),EAction->CanUse.Execute(Owner,true)) ||
        !TestTrue(TEXT("E 使用回调沿真实库存接收道具"),EAction->Use.Execute(Owner))) return false;
    int32 Caterpillars=0;
    for (int32 Index=0;Index<UDeliveryInventoryComponent::SlotCount;++Index)
        if (Cast<ADeliveryCaterpillarItem>(Owner->GetInventoryComponent()->GetItemInSlot(Index))) ++Caterpillars;
    TestEqual(TEXT("主人库存实际只有一条小绿"),Caterpillars,1);
    TestTrue(TEXT("同一只院子小绿进背包"),YardWorm->GetOwner()==Owner && Owner->OwnsHandheldItem(YardWorm));
    TestFalse(TEXT("重复E不会再给一条"),EAction->Use.Execute(Owner));
    TestFalse(TEXT("接过小绿仍没有自动投放外卖"),Package->ContainsCaterpillar());
    CheckNotDelivered();

    // 回到同一个地面实物，覆盖主人当轮同意、模型替身应答、候选采纳和实际动作启动
    FDeliverySpecialEventRule Prank;
    Prank.EventTag=TAG_Event_Delivery_Caterpillar;
    Prank.Multiplier=1.f;
    Task->SpecialEventRules.Add(Prank);
    if (!TestTrue(TEXT("主人可从原背包释放同一小绿"),Owner->ReleaseHandheldItem(YardWorm))) return false;
    YardWorm->DropFromInventory(YardSpot,FVector::ZeroVector);
    WormMesh->SetSimulatePhysics(false);
    const auto BuildPrankRequest=[&](const TCHAR* Line,FDeliveryNpcDecisionRequest& Request)
    {
        FDeliveryNpcObservation Speech;
        Speech.EventId=FGuid::NewGuid(); Speech.Kind=TEXT("PlayerSpeech"); Speech.Subject=Owner; Speech.Addressee=Npc;
        Speech.Source=EDeliveryNpcEvidenceSource::Heard; Speech.Description=Line; Speech.bNeedsReply=true;
        return Agent->BeginListening(Owner,Speech.EventId) && Agent->ObserveEvent(Speech) &&
            Agent->BuildDecisionRequest(FGuid::NewGuid(),Request) && Request.bPendingPlayerMessage &&
            Request.bRequesterIsCompanionOwner && Request.PlayerMessageId==Speech.EventId;
    };
    const auto FindPrankOption=[YardWorm](const FDeliveryNpcDecisionRequest& Request,EDeliveryNpcAction Action,
        const ADeliveryCaterpillarItem* Target=nullptr)
    {
        if (!Target) Target=YardWorm;
        return Request.Candidates.FindByPredicate([Target,Action](const auto& Candidate)
        {
            return Candidate.Action==Action && Candidate.ItemChoice.IsValid() && Candidate.ItemChoice->Item.Get()==Target &&
                Candidate.TravelMode==EDeliveryCompanionTravelMode::Unspecified;
        });
    };
    const auto StartPrank=[&](const FDeliveryNpcDecisionRequest& Request,const FDeliveryNpcCandidate& Candidate)
    {
        auto Reply=MakeModelReply(Request,Candidate);
        Reply.Speech=TEXT("好，我来。");
        Agent->ActiveRequest=Request;
        Agent->bDecisionPending=true;
        Agent->ReceiveDecision(Request.RequestId,&Reply,NAME_None);
        FDeliveryNpcPublicState Started;
        if (!Candidate.ItemChoice || !Agent->StartAction(Started) || Started.Action!=Candidate.Action ||
            Started.Target!=Candidate.ItemChoice->Item.Get()) return false;
        // 夹具替身经正式采纳，但不发送语音合成请求
        Started.Speech.Reset();
        return Npc->ExecuteAction(Started) &&
            Agent->TryFinishAction(Started.ActionId,EDeliveryNpcActionStatus::Succeeded,TEXT("CompanionErrandAccepted"));
    };

    FDeliveryNpcDecisionRequest CollectRequest;
    if (!TestTrue(TEXT("实际主人转入找小绿的待回复请求"),BuildPrankRequest(TEXT("好，先把地上的小绿拿起来"),CollectRequest))) return false;
    const auto* Collect=FindPrankOption(CollectRequest,EDeliveryNpcAction::CollectCaterpillar);
    const auto* DirectInsert=FindPrankOption(CollectRequest,EDeliveryNpcAction::InsertCaterpillar);
    if (!TestTrue(TEXT("地上同一原虫有分别拿起和投放的候选"),Collect && DirectInsert &&
        Collect->ItemChoice->Source==TEXT("World") && !Collect->ItemChoice->UsePackage.IsValid() &&
        DirectInsert->ItemChoice->UsePackage.Get()==Package && DirectInsert->TargetId==Collect->TargetId)) return false;
    if (!TestTrue(TEXT("回复采纳后启动真实找物并保留地上原虫"),StartPrank(CollectRequest,*Collect) &&
        Companion->HasNearbyItemSearch() && Companion->GetErrand().Item.Get()==YardWorm && !YardWorm->GetOwner())) return false;
    Companion->FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("SwitchToHeldPrankFixture"));

    // 把 E 取到的同一只小绿放回主人实际背包，由一句话接虫并投放
    if (!TestTrue(TEXT("同一只小绿在主人背包，原件仍在主人手中"),Owner->TryReceiveHandheldItem(YardWorm) &&
        Owner->OwnsHandheldItem(YardWorm) && Owner->GetInventoryComponent()->GetHeldItem()==Package)) return false;
    FDeliveryNpcDecisionRequest InventoryPrankRequest;
    if (!TestTrue(TEXT("真实主人可从背包安排同单投放"),BuildPrankRequest(TEXT("好，把我背包里的小绿放进这单"),InventoryPrankRequest))) return false;
    const auto* InventoryPrank=FindPrankOption(InventoryPrankRequest,EDeliveryNpcAction::InsertCaterpillar);
    if (!InventoryPrank)
    {
        AddInfo(FString::Printf(TEXT("背包投放：证据=%d 实体=%d 候选=%d 判为可取=%d 同包裹=%d 位置=%s"),
            InventoryPrankRequest.Evidence.Num(),InventoryPrankRequest.Subjects.Num(),InventoryPrankRequest.Candidates.Num(),
            Companion->CanTakeOwnerItem(YardWorm),Companion->GetCommandPackage()==Package,
            *InventoryPrankRequest.CompanionOwnerPackageLocation.ToString()));
    }
    if (!TestTrue(TEXT("投放候选封存背包来源、主人和同一包裹"),InventoryPrank &&
        InventoryPrank->ItemChoice->Source==TEXT("OwnerBackpack") && InventoryPrank->ItemChoice->SourceCarrier.Get()==Owner &&
        InventoryPrank->ItemChoice->UsePackage.Get()==Package && InventoryPrankRequest.CompanionOwnerPackageLocation==TEXT("Hand"))) return false;
    auto TamperedPrank=*InventoryPrank->ItemChoice;
    TamperedPrank.UsePackage.Reset();
    TestFalse(TEXT("旧包裹或空包裹不能令同伴先拿走小绿"),Companion->CanApplyItemChoice(TamperedPrank,OwnerState,InventoryPrankRequest.CompanionCommandRevision));
    TamperedPrank=*InventoryPrank->ItemChoice;
    TamperedPrank.Source=TEXT("OwnerHand");
    TestFalse(TEXT("不能改签本轮小绿来源"),Companion->CanApplyItemChoice(TamperedPrank,OwnerState,InventoryPrankRequest.CompanionCommandRevision));
    if (!TestTrue(TEXT("模型从背包投放的指令进入真实交接阶段"),StartPrank(InventoryPrankRequest,*InventoryPrank) &&
        Companion->GetErrand().Phase==EDeliveryCompanionErrandPhase::GoToPickup && Owner->OwnsHandheldItem(YardWorm))) return false;
    Companion->UpdateErrand();
    if (!TestTrue(TEXT("依同一安排接过原虫后保留原包裹投放阶段"),Npc->OwnsHandheldItem(YardWorm) &&
        !Owner->OwnsHandheldItem(YardWorm) && Companion->GetErrand().Phase==EDeliveryCompanionErrandPhase::GoToDelivery &&
        Companion->GetErrand().ItemUsePackage.Get()==Package && !Package->ContainsCaterpillar())) return false;
    Companion->NextItemObservationAt=0;
    Companion->RefreshItemObservations();
    const auto Current=[] { return true; };
    const auto Cancelled=[] { return false; };
    TestFalse(TEXT("访客不能投放同一小绿"),Package->TryInsertCompanionCaterpillar(Npc,YardWorm,StrangerState,Current));
    TestFalse(TEXT("旧安排不能投放同一小绿"),Package->TryInsertCompanionCaterpillar(Npc,YardWorm,OwnerState,Cancelled));
    TestTrue(TEXT("拒绝后原虫仍在二狗手中且原包裹未改"),Npc->OwnsHandheldItem(YardWorm) && !Package->ContainsCaterpillar());
    Npc->SetActorLocation(Owner->GetActorLocation()+FVector(0,400,0),false,nullptr,ETeleportType::TeleportPhysics);
    TestFalse(TEXT("隔远时不能投放主人手中原件"),Package->CanAcceptCompanionCaterpillar(Npc,OwnerState,true));
    Npc->SetActorLocation(Owner->GetActorLocation()+FVector(0,85,0),false,nullptr,ETeleportType::TeleportPhysics);
    const auto* HeldPackageInteraction=UDeliveryInteractableComponent::FindOn(Package,EDeliveryInteractionKey::PickupE);
    if (!TestTrue(TEXT("手持真实原件关闭了地面拾取"),HeldPackageInteraction && !HeldPackageInteraction->CanInteract(Npc))) return false;
    const FVector PrankFrom=Npc->GetActorLocation()+FVector(0,0,25);
    const FVector PrankTarget=Package->GetItemMesh()->Bounds.Origin;
    const double PrankReach=HeldPackageInteraction->InteractRadius+Npc->GetSimpleCollisionRadius();
    if (!TestTrue(FString::Printf(TEXT("测试包裹网格位于接触距离内（实际 %.1f 厘米、上限 %.1f 厘米）"),
        FVector::Dist(PrankFrom,PrankTarget),PrankReach),FVector::DistSquared(PrankFrom,PrankTarget)<=FMath::Square(PrankReach))) return false;
    if (!TestTrue(TEXT("手持原件在实际近处满足投放校验"),Package->CanAcceptCompanionCaterpillar(Npc,OwnerState,true))) return false;
    FDeliveryNpcDecisionRequest InsertRequest;
    if (!TestTrue(TEXT("第二轮主人同意把手里的小绿放进原快递"),BuildPrankRequest(TEXT("现在把你手里的小绿放进我拿着的这件快递"),InsertRequest))) return false;
    const auto* Insert=FindPrankOption(InsertRequest,EDeliveryNpcAction::InsertCaterpillar);
    if (!TestNotNull(TEXT("二狗手中实物仍有投放候选"),Insert)) return false;
    TestTrue(TEXT("投放候选锁定二狗手中原虫与主人手中原包裹"),Insert->ItemChoice->Source==TEXT("CompanionHand") &&
        Insert->ItemChoice->UsePackage.Get()==Package && InsertRequest.CompanionHeldItemId==Insert->TargetId);
    TestTrue(TEXT("持物摘要与候选主体使用相同名称"),InsertRequest.Subjects.ContainsByPredicate([&](const auto& Subject)
        { return Subject.SubjectId==Insert->TargetId && Subject.Name==InsertRequest.CompanionHeldItemName; }));
    if (!TestTrue(TEXT("投放复用同一只小绿的真实持物证据"),
        Insert->ItemChoice->EvidenceId==Companion->HeldItemEventId &&
        InsertRequest.Evidence.ContainsByPredicate([&](const auto& Fact)
            { return Fact.EventId==Insert->ItemChoice->EvidenceId && Fact.SubjectId==Insert->TargetId &&
                Fact.Kind==TEXT("CompanionHoldingItem") && Fact.Source==EDeliveryNpcEvidenceSource::ActionResult && !Fact.bNeedsReply; })))
        return false;
    auto FullEvidenceRequest=InsertRequest;
    FullEvidenceRequest.Candidates.RemoveAll([](const auto& Candidate) { return Candidate.Action==EDeliveryNpcAction::InsertCaterpillar; });
    while (FullEvidenceRequest.Evidence.Num()<32)
    {
        auto& Fact=FullEvidenceRequest.Evidence.AddDefaulted_GetRef();
        Fact.EventId=FGuid::NewGuid();
        Fact.Kind=TEXT("CapacityFixture");
    }
    Companion->AddCaterpillarItemCandidates(FullEvidenceRequest,OwnerState);
    const auto* FullEvidenceOption=FindPrankOption(FullEvidenceRequest,EDeliveryNpcAction::InsertCaterpillar);
    TestTrue(TEXT("证据已满仍可使用同一持物事实，保留原话和容量上限"),FullEvidenceRequest.Evidence.Num()==32 &&
        FullEvidenceOption && FullEvidenceOption->ItemChoice->EvidenceId==Insert->ItemChoice->EvidenceId);
    if (!TestTrue(TEXT("模型选择投放后只启动绑定同一原件的委托"),StartPrank(InsertRequest,*Insert) &&
        Companion->GetErrand().ItemUsePackage.Get()==Package && Npc->OwnsHandheldItem(YardWorm) && !Package->ContainsCaterpillar())) return false;
    Companion->UpdateErrand();
    TestEqual(TEXT("委托回执确认已投放小绿"),Companion->GetErrand().Reason,FName(TEXT("CaterpillarInserted")));
    TestTrue(TEXT("只消耗同一只小绿并标记同一原件"),YardWorm->IsActorBeingDestroyed() &&
        !Npc->GetCarriedHandheldItem() && Package->ContainsCaterpillar() && Owner->GetCarriedTaskPackage()==Package);
    CheckNotDelivered();

    // 独立实物覆盖二狗手持包裹的分支，保留上面的主人背包交接回归
    {
        auto* PickupTuning=LoadObject<UDeliveryCompanionTuning>(nullptr,TEXT("/Game/NPC/Companion/DA_CompanionTuning.DA_CompanionTuning"));
        auto* PickupMesh=LoadObject<USkeletalMesh>(nullptr,TEXT("/Game/NPC/Companion/Appearance/SK_ErgouBoy.SK_ErgouBoy"));
        if (!TestTrue(TEXT("手持包裹投放使用正式拾取动画和骨架"),PickupTuning && PickupTuning->GroundPickupAnimation && PickupMesh)) return false;
        auto* Move=Npc->GetCharacterMovement();
        const auto SavedMovement=Move->MovementMode;
        const auto SavedFloor=Move->CurrentFloor;
        const auto SavedTransform=Npc->GetActorTransform();
        auto* SavedMesh=Npc->GetMesh()->GetSkeletalMeshAsset();
        TGuardValue<TObjectPtr<UDeliveryCompanionTuning>> UsePickupTuning(Companion->Tuning,PickupTuning);
        TGuardValue<bool> AllowPrankOffer(Companion->bCaterpillarGiven,false);
        auto* Floor=World->SpawnActor<AActor>();
        auto* Box=NewObject<UBoxComponent>(Floor);
        Floor->SetRootComponent(Box);
        Box->SetBoxExtent(FVector(280,280,25));
        Box->SetCollisionProfileName(TEXT("BlockAll"));
        Box->RegisterComponent();
        Floor->SetActorLocation(FVector(1800,0,-25));
        ON_SCOPE_EXIT
        {
            Floor->Destroy();
            Npc->GetMesh()->SetSkeletalMeshAsset(SavedMesh);
            Npc->SetActorTransform(SavedTransform,false,nullptr,ETeleportType::TeleportPhysics);
            Move->SetMovementMode(SavedMovement);
            Move->CurrentFloor=SavedFloor;
        };
        Npc->SetActorLocationAndRotation(FVector(1800,0,90),FRotator::ZeroRotator,false,nullptr,ETeleportType::TeleportPhysics);
        Npc->GetMesh()->SetSkeletalMeshAsset(PickupMesh);
        Move->StopMovementImmediately();
        Move->SetMovementMode(MOVE_Walking);
        Move->FindFloor(Npc->GetActorLocation(),Move->CurrentFloor,false);
        if (!TestTrue(TEXT("俯身夹具有可行走地面"),Move->CurrentFloor.IsWalkableFloor())) return false;
        auto* HeldParcel=World->SpawnActor<ADeliveryTestPackage>(FVector(1800,0,140),FRotator::ZeroRotator,WormSpawn);
        if (!TestNotNull(TEXT("创建用于二狗手持投放的任务原件"),HeldParcel)) return false;
        HeldParcel->GetDeliveryItemComponent()->OwningTask=Task;
        HeldParcel->GetDeliveryItemComponent()->bReportLostOnDestroy=false;
        if (!TestTrue(TEXT("二狗真实持有该件货"),Npc->TryReceiveTaskPackage(HeldParcel))) return false;
        ON_SCOPE_EXIT { Npc->ReleaseTaskPackage(HeldParcel); HeldParcel->Destroy(); };
        const FVector Palm=Npc->GetMesh()->GetComponentTransform().TransformPosition(PickupTuning->GroundPickupPalmPosition);
        World->SetAllowDeferredPhysicsStateCreation(false);
        auto* GroundWorm=World->SpawnActor<ADeliveryCaterpillarItem>(WormClass,Palm,FRotator::ZeroRotator,WormSpawn);
        World->SetAllowDeferredPhysicsStateCreation(bDeferredPhysics);
        if (!TestNotNull(TEXT("地面候选对应一只真实小绿"),GroundWorm)) return false;
        const FVector GripOffset=GroundWorm->GetItemMesh()->GetComponentTransform().TransformVector(GroundWorm->NpcGroundPickupGrip);
        GroundWorm->DropFromInventory(Palm-GripOffset,FVector::ZeroVector);
        GroundWorm->GetItemMesh()->SetSimulatePhysics(false);
        if (!TestTrue(TEXT("持着原件仍可提议当场投放"),Companion->CanOfferCaterpillar(OwnerState))) return false;
        FVector CurrentSight;
        if (!TestTrue(TEXT("二狗在本轮真实看见地面原虫"),Companion->CanSeeWorldItem(GroundWorm,CurrentSight))) return false;
        // 新进入视野的实物仍在通用观察限频内，不能因此漏掉主人的当轮指令
        TGuardValue<double> ItemScanInCooldown(Companion->NextItemObservationAt,World->GetTimeSeconds()+1.);
        TestFalse(TEXT("通用扫描尚未记录刚出现的小绿"),Agent->FindKnownSubjectId(GroundWorm).IsValid());
        FDeliveryNpcDecisionRequest HandPackagePrank;
        if (!TestTrue(TEXT("主人同意对二狗手里的原件投放"),BuildPrankRequest(TEXT("好，放进你手里的包裹"),HandPackagePrank))) return false;
        const auto GroundSubject=Agent->FindKnownSubjectId(GroundWorm);
        if (!TestTrue(TEXT("快照前记录当前实见，保留同一只地上小绿及容量上限"),GroundSubject.IsValid() &&
            HandPackagePrank.Evidence.Num()<=32 && HandPackagePrank.Evidence.ContainsByPredicate([GroundSubject](const auto& Fact)
            {
                return Fact.SubjectId==GroundSubject && Fact.Kind==TEXT("WorldItemSeen") &&
                    Fact.Source==EDeliveryNpcEvidenceSource::Seen && !Fact.bNeedsReply;
            }))) return false;
        const auto* FirstGroundOption=FindPrankOption(HandPackagePrank,EDeliveryNpcAction::InsertCaterpillar,GroundWorm);
        if (!TestNotNull(TEXT("首轮候选引用已经封存的实见"),FirstGroundOption)) return false;
        const FGuid GroundObservationId=FirstGroundOption->ItemChoice->EvidenceId;
        const double GroundObservedAt=FirstGroundOption->ItemChoice->ObservedAt;
        ++GFrameCounter;
        World->Tick(LEVELTICK_TimeOnly,.2f);
        if (!TestTrue(TEXT("后续主人请求复用已有实见时仍能生成投放候选"),
            BuildPrankRequest(TEXT("把刚才那只小绿放进你的外卖"),HandPackagePrank))) return false;
        const auto* HandPackageOption=FindPrankOption(HandPackagePrank,EDeliveryNpcAction::InsertCaterpillar,GroundWorm);
        if (!HandPackageOption || HandPackagePrank.CompanionCarriedItemLocation!=TEXT("Hand"))
        {
            FVector SeenPoint;
            AddInfo(FString::Printf(TEXT("持件投放：候选=%d 证据=%d 实见小绿=%d 包裹快照位置=%s 附件=%s"),
                HandPackagePrank.Candidates.Num(),HandPackagePrank.Evidence.Num(),
                Companion->CanSeeWorldItem(GroundWorm,SeenPoint),*HandPackagePrank.CompanionCarriedItemLocation.ToString(),
                *GetNameSafe(HeldParcel->GetRootComponent()->GetAttachParent())));
        }
        if (!TestNotNull(TEXT("二狗持包裹时本轮提供同一地面小绿的投放候选"),HandPackageOption)) return false;
        TestTrue(TEXT("复用的真实观察时间不能改成当前请求时刻"),HandPackageOption->ItemChoice->EvidenceId==GroundObservationId &&
            HandPackageOption->ItemChoice->ObservedAt==GroundObservedAt && GroundObservedAt<World->GetTimeSeconds());
        TestEqual(TEXT("实物包裹位置快照仍在二狗手里"),HandPackagePrank.CompanionCarriedItemLocation,FName(TEXT("Hand")));
        TestNull(TEXT("已有包裹时不开放第二个手持物"),FindPrankOption(HandPackagePrank,EDeliveryNpcAction::CollectCaterpillar,GroundWorm));
        if (!TestTrue(TEXT("投放候选封存同一地面小绿及手持原包裹"),
            HandPackageOption->ItemChoice->bInsertIntoHeldPackage && HandPackageOption->ItemChoice->UsePackage.Get()==HeldParcel &&
            HandPackageOption->ItemChoice->Source==TEXT("World") &&
            HandPackagePrank.Evidence.ContainsByPredicate([&](const auto& Fact)
            {
                return Fact.EventId==HandPackageOption->ItemChoice->EvidenceId && Fact.SubjectId==GroundSubject &&
                    Fact.SubjectId==HandPackageOption->TargetId && Fact.Kind==TEXT("WorldItemSeen");
            }))) return false;
        TestFalse(TEXT("访客不能借用这次原件投放"),Companion->CanApplyItemChoice(*HandPackageOption->ItemChoice,StrangerState,
            HandPackagePrank.CompanionCommandRevision));
        if (!TestTrue(TEXT("主人选择后正常开始寻找和俯身"),StartPrank(HandPackagePrank,*HandPackageOption))) return false;
        Companion->UpdateErrand();
        Companion->UpdateErrand();
        if (!TestTrue(TEXT("接触前先播放俯身，原虫仍留地上"),Companion->Errand.GroundPickupServerTime>=0 &&
            !HeldParcel->ContainsCaterpillar() && !GroundWorm->GetOwner())) return false;
        FName EarlyReason;
        TestFalse(TEXT("提前触发技能不会投放"),Companion->RunItemTransfer(false,EarlyReason));
        for (int32 Step=0;Step<FMath::CeilToInt((PickupTuning->GroundPickupAnimation->GetPlayLength()+.15f)/.1f);++Step)
        {
            ++GFrameCounter;
            World->Tick(LEVELTICK_TimeOnly,.1f);
        }
        Companion->UpdateErrand();
        Companion->UpdateErrand();
        TestTrue(TEXT("接触后只消耗地面原虫，包裹仍是二狗所持同一件"),GroundWorm->IsActorBeingDestroyed() &&
            HeldParcel->ContainsCaterpillar() && Npc->GetCarriedTaskPackage()==HeldParcel &&
            Companion->GetErrand().Reason==TEXT("CaterpillarInserted"));
        CheckNotDelivered();
    }

    OwnerGrip->SetRelativeTransform(SavedWormFixtureGrip);
    Owner->ApplyTaskPackagePresentation(Package);

    // 用同一委托转移、交付及结算；测试直接布置站位，路径和画面由试玩检查
    const int32 DeliveryReward = Tasks->PreviewReward(Task).FinalReward;
    TestTrue(TEXT("正对照奖励配置非零"), DeliveryReward > 0);
    if (!TestTrue(TEXT("送件前由主人确认原地等候"), Apply(EDeliveryNpcAction::Stay, OwnerState, Snapshot()))) return false;
    const FVector OriginalWaitLocation=Npc->GetNavAgentLocation();
    TestEqual(TEXT("等候命令记录实际步行锚点"),Companion->WaitLocation,OriginalWaitLocation);
    // 先复现开箱被打断，再验证主人升级为代送会重新申请同一个包裹
    auto* Bike=World->SpawnActor<ADeliveryMotorbike>(FVector(2600,0,88),FRotator::ZeroRotator);
    auto* Summon=Owner->FindComponentByClass<UDeliveryVehicleSummonComponent>();
    if (!TestNotNull(TEXT("生成真实主人车辆"),Bike) || !TestNotNull(TEXT("主人车辆登记组件存在"),Summon)) return false;
    if (!TestTrue(TEXT("把原包裹放进登记车辆"),Owner->ReleaseTaskPackage(Package) && Bike->AttachCargoForPlayer(Package,OwnerState))) return false;
    Summon->RegisterVehicle(Bike);
    auto* RecentBike=World->SpawnActor<ADeliveryMotorbike>(FVector(6000,0,88),FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("主人稍后登记另一辆空车"),RecentBike)) return false;
    Summon->RegisterVehicle(RecentBike);
    TestEqual(TEXT("改骑空车仍更新最近使用车辆"),Summon->GetRegisteredVehicle(),RecentBike);
    TestEqual(TEXT("装原件的车独立于最近使用车辆"),Summon->GetRegisteredCargoVehicle(),Bike);
    FDeliveryNpcDecisionRequest RegisteredCargoRequest;
    RegisteredCargoRequest.bPlayerRequest=true;
    Npc->ExtendDecisionRequest(RegisteredCargoRequest,OwnerState);
    TestEqual(TEXT("主人实际请求仍能读到原包裹在登记货车"),
        RegisteredCargoRequest.CompanionOwnerPackageLocation,FName(TEXT("RegisteredBikeTrunk")));

    {
        auto* PC=OwnerState->GetPlayerController();
        const auto PriorMode=Companion->Mode;
        const auto PriorTravel=Companion->TravelMode;
        const FTransform PriorOwnerTransform=Owner->GetActorTransform();
        Companion->Mode=EDeliveryCompanionMode::Follow;
        ON_SCOPE_EXIT
        {
            if (Bike->GetDriver()==Owner) Bike->ExitVehicle();
            Companion->FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("OwnerRendezvousChecked"));
            Companion->Mode=PriorMode;
            Companion->TravelMode=PriorTravel;
            Owner->SetActorTransform(PriorOwnerTransform,false,nullptr,ETeleportType::TeleportPhysics);
            Summon->RegisterVehicle(RecentBike);
        };
        const auto BeforeRendezvous=Snapshot();
        if (!TestTrue(TEXT("委托原车箱包裹后主人可实际骑同一辆车"),PC && Companion->ApplyCommand(EDeliveryNpcAction::DeliverPackage,
            OwnerState,BeforeRendezvous.CompanionCommandRevision,BeforeRendezvous.CompanionTask.Get(),Package,
            EDeliveryCompanionTravelMode::Walk) && Bike->TryEnter(Owner) && PC->GetPawn()==Bike)) return false;
        const int32 RendezvousRevision=Companion->GetCommandRevision();
        Companion->UpdateErrand();
        TestTrue(TEXT("骑乘期间委托保留同一原件并接续跟随，禁止提前开箱交接"),
            Companion->OwnerHandoffFollowRevision==RendezvousRevision && Companion->HasActiveErrand() &&
            Companion->GetErrand().Phase==EDeliveryCompanionErrandPhase::ReceiveFromOwner &&
            Bike->GetCargo()==Package && !Companion->ErrandCargoHandoffId.IsValid() && !Npc->GetCarriedTaskPackage());
        // 交接规则夹具使用无网格的原生玩家，下车仍会尝试恢复主动布娃娃
        AddExpectedError(FString::Printf(TEXT("%s: Skeletal mesh has no Physics Asset."),*GetNameSafe(Owner)),
            EAutomationExpectedErrorFlags::Contains,1);
        Bike->ExitVehicle();
        if (!TestTrue(TEXT("主人从实际骑乘恢复步行"),PC->GetPawn()==Owner)) return false;
        Companion->UpdateErrand();
        TestTrue(TEXT("恢复交接前仅清掉等车的跟随路径和目标"),Companion->OwnerHandoffFollowRevision==INDEX_NONE &&
            !Companion->bHasFollowDestination && Companion->GetCommandRevision()==RendezvousRevision &&
            Companion->GetErrand().Phase==EDeliveryCompanionErrandPhase::ReceiveFromOwner && Bike->GetCargo()==Package &&
            !Companion->ErrandCargoHandoffId.IsValid());
        CheckNotDelivered();
    }

    {
        // 清理上一段结识对话的待回复事件，独立验证行为树换动作
        Agent->ResetContext(TEXT("BehaviorTreeReplacementFixture"));
        auto* AI=Cast<AAIController>(Npc->GetController());
        auto* Tree=LoadObject<UBehaviorTree>(nullptr,TEXT("/Game/NPC/Companion/AI/BT_Companion.BT_Companion"));
        if (!TestTrue(TEXT("启动二狗正式行为树验证同帧换动作"),AI && Tree && AI->RunBehaviorTree(Tree))) return false;
        auto* Brain=Cast<UBehaviorTreeComponent>(AI->GetBrainComponent());
        if (!TestTrue(TEXT("正式行为树具有执行组件和黑板"),Brain && AI->GetBlackboardComponent())) return false;
        const auto PriorTravel=Companion->TravelMode;
        ON_SCOPE_EXIT
        {
            Brain->StopTree(EBTStopMode::Forced);
            Companion->FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("BehaviorTreeRegressionFinished"));
            Agent->CancelPlan(TEXT("BehaviorTreeRegressionFinished"));
            Companion->TravelMode=PriorTravel;
        };
        const auto TickTree=[&]
        {
            // 只驱动正式黑板与行为树，保留原夹具的时钟和货物位置
            Npc->Tick(0.f);
            Brain->TickComponent(.02f,LEVELTICK_All,nullptr);
        };
        FDeliveryNpcDecisionRequest WaitRequest;
        if (!TestTrue(TEXT("生成真实 Wait 候选请求"),Agent->BuildDecisionRequest(FGuid::NewGuid(),WaitRequest))) return false;
        const auto* Wait=WaitRequest.Candidates.FindByPredicate([](const auto& Candidate)
        { return Candidate.Action==EDeliveryNpcAction::Wait; });
        if (!TestNotNull(TEXT("当前状态可选短时 Wait"),Wait)) return false;
        const auto WaitReply=MakeModelReply(WaitRequest,*Wait);
        Agent->ReceiveDecision(WaitRequest.RequestId,&WaitReply,NAME_None);
        const auto ApprovedWait=Agent->GetPublicState();
        if (!TestTrue(TEXT("行为树执行前 Agent 已采纳短时 Wait"),ApprovedWait.Action==EDeliveryNpcAction::Wait &&
            ApprovedWait.Status==EDeliveryNpcActionStatus::Accepted)) return false;
        for (int32 Frame=0;Frame<8 && Agent->GetPublicState().Status==EDeliveryNpcActionStatus::Accepted;++Frame) TickTree();
        const auto Running=Agent->GetPublicState();
        const auto* Blackboard=AI->GetBlackboardComponent();
        if (!TestTrue(TEXT("短时 Wait 已被正式节点领取且黑板为真"),Running.Action==EDeliveryNpcAction::Wait &&
            Running.Status==EDeliveryNpcActionStatus::Running && Blackboard->GetValueAsBool(TEXT("HasApprovedAction")))) return false;

        FDeliveryNpcDecisionRequest DeliveryRequest;
        FDeliveryNpcDecisionProposal DeliveryReply;
        if (!TestTrue(TEXT("正在 Wait 时主人可以提议代送同一车箱货物"),PrepareDeliveryReply(DeliveryRequest,DeliveryReply) &&
            DeliveryRequest.RunningActionId==Running.ActionId && DeliveryRequest.CompanionPackage.Get()==Package)) return false;
        const int32 PriorRevision=Companion->GetCommandRevision();
        Agent->ReceiveDecision(DeliveryRequest.RequestId,&DeliveryReply,NAME_None);
        const FGuid Replacement=Agent->GetPublicState().ActionId;
        if (!TestTrue(TEXT("同一帧采纳新动作为 Accepted 黑板一直为真"),Replacement.IsValid() && Replacement!=Running.ActionId &&
            Agent->GetPublicState().Status==EDeliveryNpcActionStatus::Accepted && Blackboard->GetValueAsBool(TEXT("HasApprovedAction")))) return false;
        for (int32 Frame=0;Frame<8 && Agent->GetPublicState().Status==EDeliveryNpcActionStatus::Accepted;++Frame) TickTree();
        const auto Accepted=Agent->GetPublicState();
        if (!TestTrue(TEXT("正式行为树领取并确认刚替换的代送命令"),Accepted.ActionId==Replacement &&
            Accepted.Status==EDeliveryNpcActionStatus::Succeeded && Accepted.Reason==TEXT("CompanionErrandAccepted"))) return false;
        TestTrue(TEXT("旧 Wait 清理后持续执行原来的真实车箱货物"),Companion->GetCommandRevision()==PriorRevision+1 &&
            Companion->HasActiveErrand() && Companion->GetErrand().Phase==EDeliveryCompanionErrandPhase::ReceiveFromOwner &&
            Companion->GetErrand().Package.Get()==Package && Bike->GetCargo()==Package);
        CheckNotDelivered();
    }

    {
        auto* Inventory=Owner->GetInventoryComponent();
        auto* OwnerController=OwnerState->GetPlayerController();
        auto* Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!TestTrue(TEXT("普通道具回归使用原主人的控制器和真实背包"),OwnerController && OwnerController->GetPawn()==Owner && Inventory && Cube)) return false;
        const FVector OwnerPosition=Owner->GetActorLocation(),NpcPosition=Npc->GetActorLocation();
        const auto PriorTravel=Companion->TravelMode;
        const FVector PriorWaitLocation=Companion->WaitLocation;
        const bool bPriorReturningToWait=Companion->bReturningToWaitLocation;
        const bool bPriorCaptureWaitOnFoot=Companion->bCaptureWaitLocationOnFoot;
        Owner->SetActorLocation(FVector(15600,5000,88),false,nullptr,ETeleportType::TeleportPhysics);
        Npc->SetActorLocation(FVector(15750,5000,88),false,nullptr,ETeleportType::TeleportPhysics);
        ON_SCOPE_EXIT
        {
            Agent->CancelPlan(TEXT("OwnerInventoryRegressionFinished"));
            Companion->FinishErrand(EDeliveryNpcActionStatus::Cancelled,TEXT("OwnerInventoryRegressionFinished"));
            Companion->TravelMode=PriorTravel;
            if (OwnerController->GetPawn()!=Owner) OwnerController->Possess(Owner);
            Owner->SetActorLocation(OwnerPosition,false,nullptr,ETeleportType::TeleportPhysics);
            Npc->SetActorLocation(NpcPosition,false,nullptr,ETeleportType::TeleportPhysics);
            // 取物取消会在临时场景重新等候，离开时恢复送件测试的原安排
            Companion->WaitLocation=PriorWaitLocation;
            Companion->bReturningToWaitLocation=bPriorReturningToWait;
            Companion->bCaptureWaitLocationOnFoot=bPriorCaptureWaitOnFoot;
        };
        const auto SpawnTool=[&](const TCHAR* Name)
        {
            auto* Tool=World->SpawnActor<ADeliveryHandheldItem>(Owner->GetActorLocation()+FVector(0,0,100),FRotator::ZeroRotator);
            if (!Tool) return Tool;
            Tool->GetItemMesh()->SetStaticMesh(Cube);
            Tool->GetItemComponent()->ItemType=EDeliveryInventoryItemType::BackpackItem;
            Tool->GetItemComponent()->DisplayName=FText::FromString(Name);
            return Tool;
        };
        auto* HandGuard=SpawnTool(TEXT("手中扳手"));
        auto* Requested=SpawnTool(TEXT("指定背包锤子"));
        auto* OtherTool=SpawnTool(TEXT("另一件普通道具"));
        ON_SCOPE_EXIT
        {
            for (auto* Tool:{HandGuard,Requested,OtherTool}) if (IsValid(Tool))
            {
                if (Owner->OwnsHandheldItem(Tool)) Owner->ReleaseHandheldItem(Tool);
                if (Stranger->OwnsHandheldItem(Tool)) Stranger->ReleaseHandheldItem(Tool);
                if (Npc->OwnsHandheldItem(Tool)) Npc->ReleaseHandheldItem(Tool);
                Tool->Destroy();
            }
        };
        if (!TestTrue(TEXT("把目标真实放进背包并留下另一件道具"),HandGuard && Requested && OtherTool &&
            Owner->TryReceiveHandheldItem(HandGuard) && Owner->TryReceiveHandheldItem(Requested) &&
            Owner->TryReceiveHandheldItem(OtherTool) && Inventory->GetHeldItem()==HandGuard)) return false;
        TArray<ADeliveryHandheldItem*> OriginalSlots;
        for (int32 Slot=0;Slot<UDeliveryInventoryComponent::SlotCount;++Slot) OriginalSlots.Add(Inventory->GetItemInSlot(Slot));
        const int32 RequestedSlot=OriginalSlots.IndexOfByKey(Requested);
        if (!TestTrue(TEXT("目标确实在非手持槽"),RequestedSlot>0)) return false;
        const auto OtherSlotsUnchanged=[&]
        {
            for (int32 Slot=0;Slot<OriginalSlots.Num();++Slot)
                if (Slot!=RequestedSlot && Inventory->GetItemInSlot(Slot)!=OriginalSlots[Slot]) return false;
            return true;
        };
        const auto BuildItemRequest=[&](APawn* Speaker,FDeliveryNpcDecisionRequest& Request)
        {
            FDeliveryNpcObservation Speech;
            Speech.EventId=FGuid::NewGuid(); Speech.Kind=TEXT("PlayerSpeech"); Speech.Subject=Speaker; Speech.Addressee=Npc;
            Speech.Source=EDeliveryNpcEvidenceSource::Heard; Speech.bNeedsReply=true; Speech.Description=TEXT("把指定锤子拿一下");
            return Agent->BeginListening(Speaker,Speech.EventId) && Agent->ObserveEvent(Speech) && Agent->BuildDecisionRequest(FGuid::NewGuid(),Request);
        };
        const auto FindTool=[&](const FDeliveryNpcDecisionRequest& Request,EDeliveryNpcAction Action)
        {
            return Request.Candidates.FindByPredicate([&](const FDeliveryNpcCandidate& Candidate)
            {
                return Candidate.Action==Action && Candidate.TravelMode==EDeliveryCompanionTravelMode::Walk &&
                    Candidate.ItemChoice.IsValid() && Candidate.ItemChoice->Item.Get()==Requested;
            });
        };
        const auto StartItem=[&](const FDeliveryNpcDecisionRequest& Request,const FDeliveryNpcCandidate& Candidate)
        {
            const auto Reply=MakeModelReply(Request,Candidate);
            Agent->ReceiveDecision(Request.RequestId,&Reply,NAME_None);
            FDeliveryNpcPublicState Started;
            if (!Agent->StartAction(Started) || !Npc->ExecuteAction(Started)) return false;
            return Agent->TryFinishAction(Started.ActionId,EDeliveryNpcActionStatus::Succeeded,TEXT("CompanionErrandAccepted"));
        };
        const auto StartTake=[&](FDeliveryCompanionItemChoice* OutChoice=nullptr)
        {
            FDeliveryNpcDecisionRequest Request;
            if (!BuildItemRequest(Owner,Request)) return false;
            const auto* Candidate=FindTool(Request,EDeliveryNpcAction::TakeItem);
            if (!Candidate || !StartItem(Request,*Candidate)) return false;
            if (OutChoice) *OutChoice=*Candidate->ItemChoice;
            return true;
        };

        FDeliveryNpcDecisionRequest OwnerRequest;
        if (!TestTrue(TEXT("只有主人本轮请求枚举实际背包"),BuildItemRequest(Owner,OwnerRequest))) return false;
        const auto* Take=FindTool(OwnerRequest,EDeliveryNpcAction::TakeItem);
        if (!TestTrue(TEXT("背包候选绑定本轮、原主人身体及所指实物"),Take && Take->ItemChoice->Source==TEXT("OwnerBackpack") &&
            Take->ItemChoice->SourceCarrier.Get()==Owner && Take->ItemChoice->RequestId==OwnerRequest.RequestId &&
            Take->ItemChoice->Requester.Get()==OwnerState)) return false;
        const auto Choice=*Take->ItemChoice;
        TestTrue(TEXT("私有物件证据只存在当前请求"),OwnerRequest.Evidence.ContainsByPredicate([&](const auto& Fact)
            { return Fact.EventId==Choice.EvidenceId && Fact.Kind==TEXT("CompanionOwnerItemAvailable"); }) && !Agent->HasObservationEvent(Choice.EvidenceId));

        FDeliveryNpcDecisionRequest FullInventoryRequest;
        FullInventoryRequest.RequesterPlayerState=OwnerRequest.RequesterPlayerState;
        FullInventoryRequest.PlayerMessageId=OwnerRequest.PlayerMessageId;
        FullInventoryRequest.bPlayerRequest=true;
        FullInventoryRequest.bPendingPlayerMessage=true;
        const auto* CurrentSpeech=OwnerRequest.Evidence.FindByPredicate([&](const auto& Fact)
            { return Fact.EventId==FullInventoryRequest.PlayerMessageId; });
        if (!TestNotNull(TEXT("容量夹具使用同一条经过确认的主人发言"),CurrentSpeech)) return false;
        FullInventoryRequest.Evidence.Add(*CurrentSpeech);
        FGuid LatestFinished;
        while (FullInventoryRequest.Evidence.Num()<32)
        {
            auto& Fact=FullInventoryRequest.Evidence.AddDefaulted_GetRef();
            Fact.EventId=LatestFinished=FGuid::NewGuid();
            Fact.Kind=TEXT("ActionFinished");
            Fact.Source=EDeliveryNpcEvidenceSource::ActionResult;
            Fact.bNeedsReply=false;
        }
        Companion->AddOwnerInventoryCandidates(FullInventoryRequest,OwnerState);
        const auto ContainsExactSource=[&](const ADeliveryHandheldItem* Item,FName Source)
        {
            return FullInventoryRequest.Candidates.ContainsByPredicate([&](const auto& Candidate)
            {
                return Candidate.Action==EDeliveryNpcAction::TakeItem && Candidate.ItemChoice.IsValid() &&
                    Candidate.ItemChoice->Item.Get()==Item && Candidate.ItemChoice->Source==Source &&
                    FullInventoryRequest.Evidence.ContainsByPredicate([&](const auto& Fact)
                        { return Fact.EventId==Candidate.ItemChoice->EvidenceId && Fact.SubjectId==Candidate.TargetId; });
            });
        };
        TestEqual(TEXT("追加主人道具后仍遵守 32 条证据上限"),FullInventoryRequest.Evidence.Num(),32);
        TestTrue(TEXT("证据满额时仍提供主人手里真实物件的候选"),ContainsExactSource(HandGuard,TEXT("OwnerHand")));
        TestTrue(TEXT("证据满额时仍提供主人背包中真实物件的候选"),ContainsExactSource(Requested,TEXT("OwnerBackpack")));
        TestTrue(TEXT("让出旧证据不挤掉当前原话和最近的动作结果"),LatestFinished.IsValid() &&
            FullInventoryRequest.Evidence.ContainsByPredicate([&](const auto& Fact) { return Fact.EventId==LatestFinished; }) &&
            FullInventoryRequest.Evidence.ContainsByPredicate([&](const auto& Fact) { return Fact.EventId==OwnerRequest.PlayerMessageId; }));
        if (!TestTrue(TEXT("主人确认后进入真实背包取物委托"),StartItem(OwnerRequest,*Take))) return false;

        bool bCancelledDuringRelease=false;
        auto* ReleaseProbe=NewObject<UDeliveryPackageTransferProbe>(Inventory);
        ReleaseProbe->InventoryChangeCallback=[&]
        {
            if (!Requested->GetOwner())
                bCancelledDuringRelease=Apply(EDeliveryNpcAction::CancelCompanionTask,OwnerState,Snapshot());
        };
        Inventory->OnInventoryChanged.AddDynamic(ReleaseProbe,&UDeliveryPackageTransferProbe::RunInventoryChangeCallback);
        Companion->UpdateErrand();
        Inventory->OnInventoryChanged.RemoveDynamic(ReleaseProbe,&UDeliveryPackageTransferProbe::RunInventoryChangeCallback);
        ReleaseProbe->InventoryChangeCallback=nullptr;
        if (!TestTrue(TEXT("GAS 释放物品期间同步取消，原物回滚给原主，其余槽位未改"),bCancelledDuringRelease &&
            Companion->ItemAbilityHandle.IsValid() && !Companion->HasActiveErrand() && !Companion->GetCarriedItem() &&
            Owner->OwnsHandheldItem(Requested) && OtherSlotsUnchanged())) return false;
        TestEqual(TEXT("取消取物会在当前脚点等候"),Companion->WaitLocation,Npc->GetNavAgentLocation());
        TestFalse(TEXT("已取消修订的旧候选不能重开委托"),Companion->ApplyItemChoice(Choice,OwnerState,OwnerRequest.CompanionCommandRevision,Take->TravelMode));
        FDeliveryNpcDecisionRequest NextPrivateRequest;
        if (!TestTrue(TEXT("取消后可以重新构造新的主人背包候选"),BuildItemRequest(Owner,NextPrivateRequest))) return false;
        const auto* FreshTake=FindTool(NextPrivateRequest,EDeliveryNpcAction::TakeItem);
        if (!TestNotNull(TEXT("新请求有新签发的同件候选"),FreshTake)) return false;
        auto Replayed=MakeModelReply(NextPrivateRequest,*FreshTake);
        Replayed.CandidateId=Choice.CandidateId; Replayed.TargetId=Choice.SubjectId; Replayed.EvidenceIds={Choice.EvidenceId};
        Agent->ReceiveDecision(NextPrivateRequest.RequestId,&Replayed,NAME_None);
        TestFalse(TEXT("在新请求中复用旧候选与私有目标编号会被拒绝"),Agent->HasActiveAction() || Companion->HasActiveErrand());

        if (!TestTrue(TEXT("重新接单后可复现物品已被移给另一人"),StartTake() && Owner->ReleaseHandheldItem(Requested) &&
            Stranger->TryReceiveHandheldItem(Requested))) return false;
        Companion->UpdateErrand();
        TestTrue(TEXT("目标已离开主人背包时旧委托不再夺取"),!Companion->HasActiveErrand() &&
            Companion->GetErrand().Reason==TEXT("OwnerItemChanged") && Requested->GetOwner()==Stranger && !Companion->GetCarriedItem() && OtherSlotsUnchanged());
        if (!TestTrue(TEXT("原主真实取回同一锤子后可重新发起"),Stranger->ReleaseHandheldItem(Requested) &&
            Owner->TryReceiveHandheldItem(Requested) && StartTake())) return false;
        auto* OtherPawn=World->SpawnActor<APawn>();
        if (!TestNotNull(TEXT("创建同一玩家临时控制对象"),OtherPawn)) return false;
        OwnerController->Possess(OtherPawn);
        Companion->UpdateErrand();
        TestTrue(TEXT("主人换控制对象后不再从原来的身体取走实物"),!Companion->HasActiveErrand() &&
            Requested->GetOwner()==Owner && !Companion->GetCarriedItem() && OtherSlotsUnchanged());
        OwnerController->Possess(Owner);
        OtherPawn->Destroy();

        FDeliveryCompanionItemChoice SuccessfulChoice;
        if (!TestTrue(TEXT("主人回到原身体可重新确认背包取物"),StartTake(&SuccessfulChoice))) return false;
        Companion->UpdateErrand();
        if (!TestTrue(TEXT("实际 GAS 将同一件道具从背包接到二狗手中"),Companion->GetErrand().Reason==TEXT("ItemTakenIntoHand") &&
            Companion->GetCarriedItem()==Requested && Requested->GetOwner()==Npc && !Inventory->GetItemInSlot(RequestedSlot) && OtherSlotsUnchanged())) return false;
        Agent->CancelPlan(TEXT("OwnerInventoryPrivacyCheck"));
        FDeliveryNpcDecisionRequest VisitorInventoryRequest;
        if (!TestTrue(TEXT("随后可以为访客构造另一轮请求"),BuildItemRequest(Stranger,VisitorInventoryRequest))) return false;
        TestFalse(TEXT("访客请求不带主人物件候选或背包事实"),VisitorInventoryRequest.AllowedActions.Contains(EDeliveryNpcAction::TakeItem) ||
            VisitorInventoryRequest.Evidence.ContainsByPredicate([](const auto& Fact) { return Fact.Kind==TEXT("CompanionOwnerItemAvailable"); }));
        TestFalse(TEXT("主人私有证据与目标编号不会进访客快照或历史"),VisitorInventoryRequest.LastDecisionEventIds.Contains(SuccessfulChoice.EvidenceId) ||
            VisitorInventoryRequest.Subjects.ContainsByPredicate([&](const auto& Subject) { return Subject.SubjectId==SuccessfulChoice.SubjectId; }));
        Agent->CancelPlan(TEXT("OwnerInventoryPrivacyChecked"));

        Companion->NextItemObservationAt=0;
        FDeliveryNpcDecisionRequest ReturnRequest;
        if (!TestTrue(TEXT("主人可单独安排把原道具还回来"),BuildItemRequest(Owner,ReturnRequest))) return false;
        const auto* Return=FindTool(ReturnRequest,EDeliveryNpcAction::ReturnItem);
        if (!TestTrue(TEXT("真实交还候选开始执行"),Return && StartItem(ReturnRequest,*Return))) return false;
        Companion->UpdateErrand();
        TestTrue(TEXT("交还原件保留手中道具和其他背包槽"),Companion->GetErrand().Reason==TEXT("ItemReturnedToOwner") &&
            Owner->OwnsHandheldItem(Requested) && !Companion->GetCarriedItem() && OtherSlotsUnchanged());
        CheckNotDelivered();
    }

    Bike->RegisterPlayerUse(StrangerState);
    TestNull(TEXT("实际登记转给访客后不能向原主人提供货车"),Summon->GetRegisteredCargoVehicle());
    TestEqual(TEXT("访客登记不会修改原包裹实际装货身份"),Bike->GetCargoPlayerState(),static_cast<APlayerState*>(OwnerState));
    Summon->RegisterVehicle(Bike);
    Summon->RegisterVehicle(RecentBike);
    Npc->SetActorLocation(Bike->GetTaskPackageTransferPoint()-FVector(150,0,0),false,nullptr,ETeleportType::TeleportPhysics);
    if (!TestTrue(TEXT("主人先要求接件"),Apply(EDeliveryNpcAction::TakePackage,OwnerState,Snapshot()))) return false;
    Companion->UpdateErrand();
    const FGuid OriginalHandoff=Companion->ErrandCargoHandoffId;
    if (!TestTrue(TEXT("原接件已开始打开真实车箱"),OriginalHandoff.IsValid() && Bike->IsTrunkBusy())) return false;
    const int32 OriginalTakeRevision=Companion->GetCommandRevision();
    Companion->ErrandActiveSeconds=4.f;
    if (!TestTrue(TEXT("主人用最新快照重复同一取件安排也可接受"),Apply(EDeliveryNpcAction::TakePackage,OwnerState,Snapshot()))) return false;
    TestTrue(TEXT("重复接件保留原开箱编号、执行时钟与接件阶段"),
        Companion->ErrandCargoHandoffId==OriginalHandoff && Companion->GetCommandRevision()==OriginalTakeRevision &&
        Companion->Errand.Phase==EDeliveryCompanionErrandPhase::ReceiveFromOwner && Companion->ErrandActiveSeconds==4.f &&
        Bike->IsTrunkBusy() && Bike->GetCargo()==Package && !Npc->GetCarriedTaskPackage());

    const auto TakeWithNewTravel=Snapshot();
    if (!TestTrue(TEXT("同一接件改为明确步行时实际改派"),Companion->TravelMode==EDeliveryCompanionTravelMode::Unspecified &&
        Companion->ApplyCommand(EDeliveryNpcAction::TakePackage,OwnerState,TakeWithNewTravel.CompanionCommandRevision,
            TakeWithNewTravel.CompanionTask.Get(),TakeWithNewTravel.CompanionPackage.Get(),EDeliveryCompanionTravelMode::Walk))) return false;
    TestTrue(TEXT("新出行方式开始新版本并关闭旧开箱"),Companion->GetCommandRevision()>OriginalTakeRevision &&
        Companion->TravelMode==EDeliveryCompanionTravelMode::Walk && !Companion->ErrandCargoHandoffId.IsValid() &&
        Companion->ErrandActiveSeconds==0.f && Bike->GetCargo()==Package);
    Companion->UpdateErrand();
    const FGuid Interrupted=Companion->ErrandCargoHandoffId;
    if (!TestTrue(TEXT("改派后重新开箱并绑定原包裹"),Interrupted.IsValid() && Interrupted!=OriginalHandoff)) return false;
    const FVector NearCargo=Npc->GetActorLocation();
    Npc->SetActorLocation(NearCargo-FVector(500,0,0),false,nullptr,ETeleportType::TeleportPhysics);
    Bike->UpdateTrunkAnimation(.1f);
    Companion->UpdateErrand();
    TestTrue(TEXT("开箱被打断保留原委托"),Companion->HasActiveErrand());
    TestEqual(TEXT("模型看到开箱暂时中断"),Snapshot().CompanionErrandReason,FString(TEXT("CargoHandoffInterrupted")));
    TestEqual(TEXT("取消开箱动画保留车中原包裹"),Bike->GetCargo(),Package);
    const auto BeforeDeliver = Snapshot();
    Npc->SetActorLocation(NearCargo,false,nullptr,ETeleportType::TeleportPhysics);
    if (!TestTrue(TEXT("指定步行后开始帮送同一件实物"), Companion->ApplyCommand(EDeliveryNpcAction::DeliverPackage,
        OwnerState, BeforeDeliver.CompanionCommandRevision, BeforeDeliver.CompanionTask.Get(),
        BeforeDeliver.CompanionPackage.Get(), EDeliveryCompanionTravelMode::Walk))) return false;
    Companion->UpdateErrand();
    const FGuid Replacement=Companion->ErrandCargoHandoffId;
    if (!TestTrue(TEXT("新委托为原包裹开箱并分配新编号"),Replacement.IsValid() && Replacement!=Interrupted)) return false;
    FDeliveryNpcObservation CargoQuestion;
    CargoQuestion.EventId=FGuid::NewGuid();
    CargoQuestion.Kind=TEXT("PlayerSpeech");
    CargoQuestion.Subject=Owner;
    CargoQuestion.Addressee=Npc;
    CargoQuestion.Source=EDeliveryNpcEvidenceSource::Heard;
    CargoQuestion.Description=TEXT("已经从我车上拿到货了吗？");
    CargoQuestion.bNeedsReply=true;
    FDeliveryNpcDecisionRequest BeforeCargoCommit;
    if (!TestTrue(TEXT("车箱交接前记录主人仍在等待的原问题"),
        Agent->BeginListening(Owner,CargoQuestion.EventId) && Agent->ObserveEvent(CargoQuestion) &&
        Agent->BuildDecisionRequest(FGuid::NewGuid(),BeforeCargoCommit))) return false;
    if (!TestTrue(TEXT("交接前快照仍从原车接同一个实物"),
        BeforeCargoCommit.bRunningErrand && BeforeCargoCommit.bPendingPlayerMessage &&
        BeforeCargoCommit.CompanionErrandPhase==TEXT("ReceiveFromOwner") && !BeforeCargoCommit.bCompanionHasPackage &&
        BeforeCargoCommit.CompanionPackage.Get()==Package)) return false;
    const auto* ContinueCargo=BeforeCargoCommit.Candidates.FindByPredicate([](const auto& Candidate)
        { return Candidate.Action==EDeliveryNpcAction::ContinueCurrent &&
            Candidate.SpeechPreference==EDeliveryCompanionSpeechPreference::Unspecified; });
    if (!TestNotNull(TEXT("等待开箱时仍可选择继续原委托"),ContinueCargo)) return false;
    FDeliveryNpcDecisionProposal StaleCargoReply;
    StaleCargoReply.CapabilityVersion=BeforeCargoCommit.RequestedCapabilityVersion;
    StaleCargoReply.CandidateId=ContinueCargo->CandidateId;
    StaleCargoReply.WorldEpoch=BeforeCargoCommit.WorldEpoch;
    StaleCargoReply.NpcId=BeforeCargoCommit.NpcId;
    StaleCargoReply.RequestId=BeforeCargoCommit.RequestId;
    StaleCargoReply.ContextRevision=BeforeCargoCommit.ContextRevision;
    StaleCargoReply.PlanRevision=BeforeCargoCommit.PlanRevision;
    StaleCargoReply.Action=ContinueCargo->Action;
    StaleCargoReply.TargetId=ContinueCargo->TargetId;
    StaleCargoReply.EvidenceIds=ContinueCargo->EvidenceIds;
    StaleCargoReply.Speech=TEXT("车尾箱还在开，货还没拿到。");
    // 只代替异步模型传输，保留正常收包和回执验证
    Agent->ActiveRequest=BeforeCargoCommit;
    Agent->bDecisionPending=true;
    Bike->CancelCargoHandoff(Interrupted);
    Bike->UpdateTrunkAnimation(10.f);
    bool bHandoffComplete=false, bHandoffSucceeded=false;
    FName HandoffReason;
    const int32 HandoffRevision=Companion->GetCommandRevision();
    if (!TestTrue(TEXT("行为树读回前车箱已实际提交原包裹"),
        Bike->GetCargoHandoffResult(Replacement,bHandoffComplete,bHandoffSucceeded,HandoffReason) &&
        bHandoffComplete && bHandoffSucceeded && Npc->GetCarriedTaskPackage()==Package && !Bike->GetCargo() &&
        Companion->ErrandCargoHandoffId==Replacement && Companion->Errand.Phase==EDeliveryCompanionErrandPhase::ReceiveFromOwner)) return false;
    TestEqual(TEXT("移交已确认但仍未切阶段时也识别旧进度台词"),
        Agent->GetRequestedCompanionErrandRefreshReason(EDeliveryNpcAction::ContinueCurrent),FName(TEXT("CompanionErrandAdvanced")));
    {
        // 夹具只设置已开始的阻断动作，实物与交接结果仍来自真实后备箱
        const auto PreviousAction=Agent->PublicState;
        ON_SCOPE_EXIT { Agent->PublicState=PreviousAction; };
        Agent->PublicState.ActionId=FGuid::NewGuid();
        Agent->PublicState.Action=EDeliveryNpcAction::Flee;
        Agent->PublicState.Origin=EDeliveryNpcActionOrigin::Model;
        Agent->PublicState.Status=EDeliveryNpcActionStatus::Running;
        Agent->PublicState.Target=Stranger;
        if (!TestTrue(TEXT("已开始的逃离动作确实会打断跑腿移动"),Agent->HasActiveAction() &&
            !Agent->CanContinueCompanionMovement())) return false;
        Companion->UpdateErrand();
        if (!TestTrue(TEXT("临时暂停保留已提交的交接结果和原件"),
            Companion->HasActiveErrand() && Companion->GetCommandRevision()==HandoffRevision &&
            Companion->Errand.Phase==EDeliveryCompanionErrandPhase::ReceiveFromOwner && Companion->ErrandCargoHandoffId==Replacement &&
            Npc->GetCarriedTaskPackage()==Package && Package->GetOwner()==Npc)) return false;
        TestEqual(TEXT("已经接到原件的委托仍可临时暂停移动"),Companion->GetSuspensionReason(),FName(TEXT("OtherBehavior")));
        CheckNotDelivered();
    }
    Companion->UpdateErrand();
    if (!TestTrue(TEXT("恢复后原版本读取先前提交结果并继续代送"),
        Companion->HasActiveErrand() && Companion->GetCommandRevision()==HandoffRevision &&
        Companion->Errand.Phase==EDeliveryCompanionErrandPhase::GoToDelivery && !Companion->ErrandCargoHandoffId.IsValid())) return false;
    CheckNotDelivered();
    TestEqual(TEXT("帮送先从主人车上接到同一个包裹"), Npc->GetCarriedTaskPackage(), Package);
    TestEqual(TEXT("拿到实物不阻挡主人明确取消"),Agent->GetRequestedCompanionErrandRefreshReason(EDeliveryNpcAction::CancelCompanionTask),NAME_None);
    AddExpectedMessagePlain(FString::Printf(TEXT("request=%s accepted=0 reason=CompanionErrandAdvanced"),
        *BeforeCargoCommit.RequestId.ToString()),ELogVerbosity::Display);
    TestFalse(TEXT("车箱交接成功后拒绝过期的流式开箱回答"),
        Agent->ReceiveStreamDecision(BeforeCargoCommit.RequestId,StaleCargoReply));
    TestTrue(TEXT("原问题进入现有重试，实物和代送进度保留"),
        Npc->IsDecisionRetryScheduled() && Agent->IsWaitingToReplyToPlayer() &&
        Agent->GetReplyMessage(OwnerState->GetPlayerController())==CargoQuestion.EventId && Npc->GetCarriedTaskPackage()==Package &&
        Companion->GetCommandRevision()==HandoffRevision && Companion->HasActiveErrand());
    FDeliveryNpcDecisionRequest AfterCargoCommit;
    if (!TestTrue(TEXT("原问题可用交接后事实重建"),Agent->BuildDecisionRequest(BeforeCargoCommit.WorldEpoch,AfterCargoCommit))) return false;
    TestTrue(TEXT("重建保留主人原句并读取已持同一件货、正在代送"),
        AfterCargoCommit.PlayerMessageId==CargoQuestion.EventId && AfterCargoCommit.bCompanionHasPackage &&
        AfterCargoCommit.CompanionPackage.Get()==Package && AfterCargoCommit.CompanionErrandPhase==TEXT("GoToDelivery"));
    Agent->CancelPlan(TEXT("ProgressAfterCargoChecked"));
    TestNull(TEXT("实际转移后后备箱为空"),Bike->GetCargo());
    TestNull(TEXT("原件转移后不会把空车继续报告为货车"),Summon->GetRegisteredCargoVehicle());
    {
        const FVector ParkingCenter(18000,12000,55);
        auto* DeliveryBike=World->SpawnActor<ADeliveryCompanionMotorbike>(ParkingCenter,FRotator::ZeroRotator);
        auto* Mesh=LoadObject<USkeletalMesh>(nullptr,TEXT("/Game/NPC/Companion/Appearance/SK_ErgouBoy.SK_ErgouBoy"));
        auto* Ride=LoadObject<UAnimSequence>(nullptr,TEXT("/Game/NPC/Companion/Animation/A_ErgouRide.A_ErgouRide"));
        if (!TestNotNull(TEXT("代送使用自己的实体车"),DeliveryBike) || !TestNotNull(TEXT("加载正式二狗网格"),Mesh) ||
            !TestNotNull(TEXT("加载正式二狗坐姿"),Ride)) return false;
        auto* Floor=World->SpawnActor<AActor>();
        auto* FloorBox=NewObject<UBoxComponent>(Floor);
        Floor->SetRootComponent(FloorBox);
        FloorBox->SetBoxExtent(FVector(400,340,25));
        FloorBox->SetCollisionProfileName(TEXT("BlockAll"));
        FloorBox->RegisterComponent();
        Floor->SetActorLocation(FVector(ParkingCenter.X,ParkingCenter.Y,-25));
        const auto SavedNpcTransform=Npc->GetActorTransform();
        auto* SavedMesh=Npc->GetMesh()->GetSkeletalMeshAsset();
        const auto SavedVehicle=Companion->ErrandVehicle;
        const auto SavedTravel=Companion->TravelMode;
        const FName SavedSuspension=Companion->GetSuspensionReason();
        const FName SavedErrandPause=Companion->LastErrandPause;
        const double SavedErrandUpdate=Companion->LastErrandUpdateAt;
        const float SavedErrandActiveSeconds=Companion->ErrandActiveSeconds;
        const double SavedErrandNavigationAt=Companion->NextErrandNavigationAt;
        ON_SCOPE_EXIT
        {
            Companion->ReleaseErrandVehicle();
            Companion->ErrandVehicle=SavedVehicle;
            Companion->TravelMode=SavedTravel;
            Companion->LastErrandPause=SavedErrandPause;
            Companion->LastErrandUpdateAt=SavedErrandUpdate;
            Companion->ErrandActiveSeconds=SavedErrandActiveSeconds;
            Companion->NextErrandNavigationAt=SavedErrandNavigationAt;
            Companion->SetSuspension(SavedSuspension);
            DeliveryBike->Destroy();
            Floor->Destroy();
            Npc->GetMesh()->SetSkeletalMeshAsset(SavedMesh);
            Npc->SetActorTransform(SavedNpcTransform,false,nullptr,ETeleportType::TeleportPhysics);
        };
        Companion->ErrandVehicle=DeliveryBike;
        Companion->TravelMode=EDeliveryCompanionTravelMode::OwnVehicle;
        Npc->GetMesh()->SetSkeletalMeshAsset(Mesh);
        DeliveryBike->RideAnimation=Ride;
        if (!TestTrue(TEXT("把专属车登记给二狗"),DeliveryBike->InitializeForCompanion(Npc))) return false;
        auto Boarding=DeliveryBike->GetBoardingLocation();
        Boarding.Z=Npc->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()+3.;
        Npc->SetActorLocation(Boarding,false,nullptr,ETeleportType::TeleportPhysics);
        Npc->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
        if (!TestTrue(TEXT("持原包裹的二狗通过真实入口上了自己的车"),DeliveryBike->TryMount(Npc) &&
            Npc->IsMountedOnCompanionBike() && Npc->GetCarriedTaskPackage()==Package)) return false;
        FDeliveryNpcDecisionRequest RidingRequest;
        RidingRequest.bPlayerRequest=true;
        Npc->ExtendDecisionRequest(RidingRequest,OwnerState);
        TestEqual(TEXT("主人所问的包裹位置来自实际专属车货架"),RidingRequest.CompanionCarriedItemLocation,FName(TEXT("OwnBikeRack")));

        // 独立世界有真实平地，但没有导航，分别检查尚未派车和已到停车点
        const int32 DeliveryRevision=Companion->GetCommandRevision();
        Companion->VehicleTravelPurpose=EDeliveryCompanionTravelPurpose::Errand;
        Companion->VehicleLegRevision=DeliveryRevision;
        Companion->VehicleLegPhase=Companion->Errand.Phase;
        const FVector RequiredWalkGoal(ParkingCenter.X+300,ParkingCenter.Y,0);
        Companion->VehicleLegGoal=RequiredWalkGoal;
        Companion->bVehicleLegComplete=false;
        TestFalse(TEXT("未找到出发路线的载件车不能冒充已经到了收件点"),Companion->UpdateVehicleTravel(
            RequiredWalkGoal,150.f,EDeliveryCompanionTravelPurpose::Errand,&RequiredWalkGoal));
        TestEqual(TEXT("刚上车未能规划时报告导航不可用"),Companion->GetSuspensionReason(),FName(TEXT("CompanionVehicleNavigationUnavailable")));
        TestTrue(TEXT("规划失败保留真实座位、原货和原阶段"),Npc->IsMountedOnCompanionBike() &&
            !Companion->bVehicleHasDriveGoal && !Companion->bVehicleLegComplete &&
            Npc->GetCarriedTaskPackage()==Package && Companion->Errand.Phase==EDeliveryCompanionErrandPhase::GoToDelivery);
        Companion->bVehicleLegComplete=true;
        TestFalse(TEXT("到站后没有可确认步行路线就不承认已下车准备送件"),Companion->UpdateVehicleTravel(
            RequiredWalkGoal,150.f,EDeliveryCompanionTravelPurpose::Errand,&RequiredWalkGoal));
        TestTrue(TEXT("不能确认出口时既保留真实车座也不放下原货"),Npc->IsMountedOnCompanionBike() &&
            Npc->GetCarriedTaskPackage()==Package && Package->GetOwner()==Npc);
        TestTrue(TEXT("出口受限时保留原代送版本与收件阶段"),Companion->HasActiveErrand() &&
            Companion->GetCommandRevision()==DeliveryRevision && Companion->Errand.Phase==EDeliveryCompanionErrandPhase::GoToDelivery);

        const double RetryBeforeHit=World->GetTimeSeconds()+15.;
        Companion->NextErrandNavigationAt=RetryBeforeHit;
        Companion->NextVehicleNavigationAt=RetryBeforeHit;
        Companion->LastErrandUpdateAt=World->GetTimeSeconds();
        Companion->bVehicleHasDriveGoal=true;
        Npc->Combat->ReceiveFightHit(Stranger,FGuid::NewGuid(),true);
        if (!TestTrue(TEXT("真实轻击在货架载件骑行时造成短后仰"),Npc->Combat->IsReactingToHit() && !Npc->IsKnockedDown())) return false;
        Companion->VehicleLegRevision=DeliveryRevision-1;
        TestFalse(TEXT("已失效的旧骑程不能沿用保座"),Companion->CanKeepVehicleSeatWhilePaused());
        Companion->VehicleLegRevision=DeliveryRevision;
        DeliveryBike->Tick(.016f);
        if (!TestTrue(TEXT("轻击后的车辆更新保留原座及同一个货架包裹"),Npc->IsMountedOnCompanionBike() &&
            Npc->GetCarriedTaskPackage()==Package && Package->GetOwner()==Npc &&
            Package->GetRootComponent()->GetAttachParent()==DeliveryBike->CompanionCargoAnchor.Get())) return false;
        Companion->UpdateErrand();
        TestEqual(TEXT("轻击后的委托只暂停"),Companion->GetSuspensionReason(),FName(TEXT("HitReaction")));
        TestTrue(TEXT("暂停停用原车路线且不推进委托计时"),!Companion->bVehicleHasDriveGoal &&
            Companion->LastErrandUpdateAt==0. && Companion->ErrandActiveSeconds==SavedErrandActiveSeconds);
        const auto BeforeHitRecovery=Tasks->GetTimeSnapshot(Task);
        for (int32 Step=0;Step<8 && Npc->Combat->IsReactingToHit();++Step)
        {
            ++GFrameCounter;
            World->Tick(LEVELTICK_TimeOnly,.1f);
        }
        if (!TestFalse(TEXT("短后仰经过实际时长后结束"),Npc->Combat->IsReactingToHit())) return false;
        Companion->UpdateErrand();
        TestTrue(TEXT("恢复时释放旧导航退避并继续原委托"),Companion->GetSuspensionReason()!=FName(TEXT("HitReaction")) &&
            Companion->NextErrandNavigationAt<RetryBeforeHit && Companion->NextVehicleNavigationAt<RetryBeforeHit &&
            Companion->HasActiveErrand() && Companion->GetCommandRevision()==DeliveryRevision &&
            Companion->Errand.Phase==EDeliveryCompanionErrandPhase::GoToDelivery && Companion->CanCommand(OwnerState));
        TestTrue(TEXT("恢复时不补扣暂停时间，包裹仍在同一骑乘身上"),
            Companion->ErrandActiveSeconds==SavedErrandActiveSeconds && Npc->IsMountedOnCompanionBike() &&
            Npc->GetCarriedTaskPackage()==Package && Package->GetOwner()==Npc);
        TestTrue(TEXT("二狗暂停期间订单自身计时仍随服务器时钟增长"),
            Tasks->GetTimeSnapshot(Task).ElapsedSeconds>BeforeHitRecovery.ElapsedSeconds);
        CheckNotDelivered();
        TestTrue(TEXT("同一地点确实有安全的物理出口以构成对照"),DeliveryBike->TryDismount() && !Npc->IsMountedOnCompanionBike());
        FDeliveryNpcDecisionRequest WalkingRequest;
        WalkingRequest.bPlayerRequest=true;
        Npc->ExtendDecisionRequest(WalkingRequest,OwnerState);
        TestEqual(TEXT("下车后实际包裹位置恢复到手上"),WalkingRequest.CompanionCarriedItemLocation,FName(TEXT("Hand")));
        TestTrue(TEXT("下车继续携带同一个包裹"),Npc->GetCarriedTaskPackage()==Package && Package->GetOwner()==Npc);
    }
    Bike->SetActorLocation(FVector(4500,0,88),false,nullptr,ETeleportType::TeleportPhysics);
    auto* OwnerController=OwnerState->GetPlayerController();
    if (!TestTrue(TEXT("交货前主人已实际换成驾驶车辆"),OwnerController && Bike->TryEnter(Owner) && OwnerController->GetPawn()==Bike)) return false;
    CheckNotDelivered();
    Npc->SetActorLocation(FVector(1980, 0, 88), false, nullptr, ETeleportType::TeleportPhysics);
    Companion->UpdateErrand();
    TestTrue(TEXT("二狗到门外地址仍保留委托"), Companion->HasActiveErrand());
    CheckNotDelivered();
    // 此测试世界没有导航网格，只向真实导航委托注入完成结果来验证所属编号与站位记录
    auto* ErrandController=Cast<AAIController>(Npc->GetController());
    auto* ErrandPath=ErrandController?ErrandController->GetPathFollowingComponent():nullptr;
    if (!TestNotNull(TEXT("收件路线监听使用当前 AI 路径组件"),ErrandPath)) return false;
    const FVector BlockedStand=DeliveryActor->GetActorLocation()+FVector(-110,20,-88);
    const FVector OtherStand=BlockedStand+FVector(30,120,0);
    const FAIRequestID ReplacedRoute(731), ActiveRoute(732), CancelledRoute(733);
    const auto AttachDeliveryRoute=[&](FAIRequestID Id,const FVector& Stand,UDeliveryTargetComponent& Receiver)
    {
        Companion->ErrandMoveId=Id;
        Companion->bErrandOwnsNavigation=true;
        Companion->bHasErrandDestination=true;
        Companion->ErrandGoal=Receiver.GetDeliveryPoint(Npc->GetActorLocation());
        Companion->ErrandDestination=Stand;
        Companion->WatchDeliveryApproach(*ErrandPath,Id,*Receiver.GetOwner());
    };
    const FPathFollowingResult BlockedPath(EPathFollowingResult::Blocked,FPathFollowingResultFlags::None);
    const FPathFollowingResult AbortedPath(EPathFollowingResult::Aborted,FPathFollowingResultFlags::UserAbort);
    AttachDeliveryRoute(ReplacedRoute,OtherStand,*Delivery);
    AttachDeliveryRoute(ActiveRoute,BlockedStand,*Delivery);
    const auto DuringDeliveryRoute=Snapshot();
    {
        FDeliveryNpcPublicState Repeat;
        Repeat.Status=EDeliveryNpcActionStatus::Accepted;
        Repeat.Action=EDeliveryNpcAction::DeliverPackage;
        Repeat.Requester=OwnerState;
        Repeat.CompanionCommandRevision=DuringDeliveryRoute.CompanionCommandRevision;
        Repeat.CompanionTask=DuringDeliveryRoute.CompanionTask.Get();
        Repeat.CompanionPackage=DuringDeliveryRoute.CompanionPackage.Get();
        Repeat.CompanionTravelMode=EDeliveryCompanionTravelMode::Walk;
        TGuardValue<FDeliveryNpcPublicState> PauseCheck(Npc->Agent->PublicState,Repeat);
        TestTrue(TEXT("行为树抢占前识别出重复代送并保留导航"),Npc->Agent->CanContinueCompanionMovement());
        Npc->Agent->PublicState.Requester=StrangerState;
        TestFalse(TEXT("访客的相同指令没有权力保留或改派委托"),Npc->Agent->CanContinueCompanionMovement());
        Npc->Agent->PublicState.Requester=OwnerState;
        Npc->Agent->PublicState.CompanionTravelMode=EDeliveryCompanionTravelMode::OwnVehicle;
        TestFalse(TEXT("确实改了出行方式就不能沿用原步行导航"),Npc->Agent->CanContinueCompanionMovement());
    }
    Companion->ErrandActiveSeconds=11.5f;
    if (!TestTrue(TEXT("主人可以重申同一件正在步行送的包裹"),Companion->ApplyCommand(EDeliveryNpcAction::DeliverPackage,
        OwnerState,DuringDeliveryRoute.CompanionCommandRevision,DuringDeliveryRoute.CompanionTask.Get(),
        DuringDeliveryRoute.CompanionPackage.Get(),EDeliveryCompanionTravelMode::Walk))) return false;
    TestTrue(TEXT("重复代送保留同一路线、计划站位、配送阶段和时钟"),
        Companion->GetCommandRevision()==DuringDeliveryRoute.CompanionCommandRevision &&
        Companion->Errand.Phase==EDeliveryCompanionErrandPhase::GoToDelivery && Companion->ErrandActiveSeconds==11.5f &&
        Companion->ErrandMoveId==ActiveRoute && Companion->bHasErrandDestination &&
        Companion->ErrandDestination.Equals(BlockedStand) && Npc->GetCarriedTaskPackage()==Package);
    ErrandPath->OnRequestFinished.Broadcast(ReplacedRoute,BlockedPath);
    TestTrue(TEXT("迟到的旧路线不改变当前站位或记录失败"),Companion->RejectedDeliveryStands.IsEmpty() &&
        Companion->ErrandMoveId==ActiveRoute && Companion->bHasErrandDestination);
    ErrandPath->OnRequestFinished.Broadcast(ActiveRoute,BlockedPath);
    TestTrue(TEXT("当前收件路线受阻后避开对应的计划站位"),Companion->RejectedDeliveryStands.Num()==1 &&
        Companion->RejectedDeliveryStands[0].Equals(BlockedStand));
    TestFalse(TEXT("当前路线受阻后待下次行为树选新站位"),Companion->bHasErrandDestination);
    TestEqual(TEXT("模型快照报告步行受阻而非结束委托"),Snapshot().CompanionErrandReason,FString(TEXT("ErrandPathBlocked")));
    TestEqual(TEXT("路线受阻不改变正在配送的实际包裹"),Npc->GetCarriedTaskPackage(),Package);
    TestTrue(TEXT("路线受阻仍保留原来的配送委托"),Companion->HasActiveErrand());
    CheckNotDelivered();
    ErrandPath->OnRequestFinished.Broadcast(ActiveRoute,BlockedPath);
    AttachDeliveryRoute(CancelledRoute,OtherStand,*Delivery);
    ErrandPath->OnRequestFinished.Broadcast(CancelledRoute,AbortedPath);
    ErrandPath->OnRequestFinished.Broadcast(CancelledRoute,BlockedPath);
    TestEqual(TEXT("重复完成或主动中止不添加失效站位"),Companion->RejectedDeliveryStands.Num(),1);
    Companion->StopErrandMovement();
    {
        const FTransform ReceiverTransform(DeliveryActor->GetActorLocation());
        auto* CapsuleReceiver=World->SpawnActorDeferred<ADeliveryGrandpa>(ADeliveryGrandpa::StaticClass(),ReceiverTransform,
            nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if (!TestNotNull(TEXT("生成带真实胶囊的收件人"),CapsuleReceiver)) return false;
        const FTransform SavedNpcTransform=Npc->GetActorTransform();
        const FVector SavedGoal=Companion->ErrandGoal;
        const FVector SavedDestination=Companion->ErrandDestination;
        const bool bSavedDestination=Companion->bHasErrandDestination;
        const double SavedNavigationAt=Companion->NextErrandNavigationAt;
        const FName SavedSuspension=Companion->GetSuspensionReason();
        const auto SavedRejectedStands=Companion->RejectedDeliveryStands;
        ON_SCOPE_EXIT
        {
            Companion->StopErrandMovement();
            if (IsValid(CapsuleReceiver)) CapsuleReceiver->Destroy();
            Delivery->ExpectedTask=Task;
            Companion->RejectDeliveryStand(*Delivery,BlockedStand);
            Companion->RejectedDeliveryStands=SavedRejectedStands;
            Companion->ErrandGoal=SavedGoal;
            Companion->ErrandDestination=SavedDestination;
            Companion->bHasErrandDestination=bSavedDestination;
            Companion->NextErrandNavigationAt=SavedNavigationAt;
            Companion->SetSuspension(SavedSuspension);
            Npc->SetActorTransform(SavedNpcTransform,false,nullptr,ETeleportType::TeleportPhysics);
        };
        Delivery->ExpectedTask=nullptr;
        CapsuleReceiver->AIControllerClass=nullptr;
        auto* CapsuleDelivery=CapsuleReceiver->DeliveryTarget.Get();
        CapsuleDelivery->ExpectedTask=Task;
        auto* ReceiverRegistration=NewObject<UDeliveryLocationComponent>(CapsuleReceiver);
        ReceiverRegistration->LocationId=Task->ReceiverNpcId;
        ReceiverRegistration->SetupAttachment(CapsuleReceiver->GetRootComponent());
        CapsuleReceiver->AddInstanceComponent(ReceiverRegistration);
        ReceiverRegistration->RegisterComponent();
        CapsuleReceiver->FinishSpawning(ReceiverTransform);
        if (!TestEqual(TEXT("订单此时解析为同编号的胶囊收件人"),Companion->ResolveErrandLocation(false),
            static_cast<AActor*>(CapsuleReceiver))) return false;
        auto* CapsuleInteraction=UDeliveryInteractableComponent::FindOn(CapsuleReceiver,EDeliveryInteractionKey::PickupE);
        if (!TestNotNull(TEXT("胶囊收件人沿用爷爷的真实交互组件"),CapsuleInteraction)) return false;
        // 模拟未配置锚点的收件人，实际交互位置取与二狗最近的胶囊表面
        CapsuleInteraction->InteractAnchor=nullptr;
        const FVector Center=CapsuleReceiver->GetActorLocation();
        const FVector Side(120,0,0);
        Npc->SetActorLocation(Center-Side,false,nullptr,ETeleportType::TeleportPhysics);
        const FVector FirstContact=CapsuleDelivery->GetDeliveryPoint(Npc->GetActorLocation());
        Companion->RejectDeliveryStand(*CapsuleDelivery,BlockedStand);
        Npc->SetActorLocation(Center+Side,false,nullptr,ETeleportType::TeleportPhysics);
        const FVector OppositeContact=CapsuleDelivery->GetDeliveryPoint(Npc->GetActorLocation());
        if (!TestTrue(TEXT("从相反两侧测得的真实接触位置相差超过旧阈值"),
            FVector::Dist(FirstContact,OppositeContact)>35.f)) return false;
        Companion->RefreshDeliveryStandRejections(*CapsuleDelivery);
        TestTrue(TEXT("二狗绕到同一胶囊收件人另一侧仍记得原站位"),Companion->RejectedDeliveryStands.Num()==1 &&
            Companion->RejectedDeliveryStands[0].Equals(BlockedStand));

        const FAIRequestID BeforeMove(734);
        AttachDeliveryRoute(BeforeMove,OtherStand,*CapsuleDelivery);
        CapsuleReceiver->SetActorLocation(Center+FVector(0,80,0),false,nullptr,ETeleportType::TeleportPhysics);
        Companion->RefreshDeliveryStandRejections(*CapsuleDelivery);
        TestTrue(TEXT("真实收件人移动后清除旧站位"),Companion->RejectedDeliveryStands.IsEmpty());
        ErrandPath->OnRequestFinished.Broadcast(BeforeMove,BlockedPath);
        TestTrue(TEXT("收件人移动前发出的路径结果不能恢复旧站位"),Companion->RejectedDeliveryStands.IsEmpty());

        CapsuleReceiver->SetActorLocation(Center,false,nullptr,ETeleportType::TeleportPhysics);
        Companion->RejectDeliveryStand(*CapsuleDelivery,BlockedStand);
        const FAIRequestID BeforeReplacement(735);
        AttachDeliveryRoute(BeforeReplacement,BlockedStand,*CapsuleDelivery);
        TestTrue(TEXT("两个收件对象在同一原点"),CapsuleReceiver->GetActorLocation().Equals(DeliveryActor->GetActorLocation()));
        CapsuleReceiver->Destroy();
        Delivery->ExpectedTask=Task;
        if (!TestEqual(TEXT("旧收件人销毁后订单解析到同位置的新对象"),Companion->ResolveErrandLocation(false),DeliveryActor)) return false;
        Companion->RefreshDeliveryStandRejections(*Delivery);
        TestTrue(TEXT("同位置的新收件人不继承原对象站位"),Companion->RejectedDeliveryStands.IsEmpty());
        Companion->RejectDeliveryStand(*Delivery,OtherStand);
        ErrandPath->OnRequestFinished.Broadcast(BeforeReplacement,BlockedPath);
        TestTrue(TEXT("已销毁收件人的旧路径结果不污染新收件人的记录"),Companion->RejectedDeliveryStands.Num()==1 &&
            Companion->RejectedDeliveryStands[0].Equals(OtherStand));
        TestEqual(TEXT("收件人变化仍保留原实物"),Npc->GetCarriedTaskPackage(),Package);
        TestTrue(TEXT("收件人变化仍在原代送委托"),Companion->HasActiveErrand());
        CheckNotDelivered();
    }
    const auto RejectRelease=[&]()
    {
        TestFalse(TEXT("解除携带时的重复结算被拒绝"),Tasks->TryCompleteDelivery(Task,OwnerState));
        TestFalse(TEXT("解除携带时另一名玩家也不能抢占同一单的结算"),Tasks->TryCompleteDelivery(Task,StrangerState));
        return false;
    };
    TestFalse(TEXT("实物释放失败时不结算"),Tasks->CompleteDelivery(Task,OwnerState,Npc,FGameplayTag(),RejectRelease));
    CheckNotDelivered();

    const auto ReleaseWhileDelivererLeaves=[&]()
    {
        const bool bReleased=Npc->ReleaseTaskPackage(Package);
        TestTrue(TEXT("原 PlayerState 失活回归经过真实持件释放"),
            bReleased && !Npc->GetCarriedTaskPackage() && !Package->GetOwner());
        OwnerState->SetIsInactive(true);
        return bReleased;
    };
    TestFalse(TEXT("原收款人在释放期间失活则不能签收"),
        Tasks->CompleteDelivery(Task,OwnerState,Npc,FGameplayTag(),ReleaseWhileDelivererLeaves));
    OwnerState->SetIsInactive(false);
    CheckNotDelivered();
    TestNull(TEXT("失活拒绝后清掉本次结算占用"),Tasks->GetCompletingPackageCarrier(Task));
    if (!TestTrue(TEXT("原主人恢复后同伴能取回同一件原包裹继续原委托"),
        IsValid(Package) && Npc->TryReceiveTaskPackage(Package) && Npc->GetCarriedTaskPackage()==Package &&
        Package->GetOwner()==Npc && Companion->HasActiveErrand())) return false;

    // 保留正在驾驶的真实主人和服务器候选，用模型替身延迟这一句进度回复
    FDeliveryNpcObservation ProgressQuestion;
    ProgressQuestion.EventId=FGuid::NewGuid();
    ProgressQuestion.Kind=TEXT("PlayerSpeech");
    ProgressQuestion.Subject=Bike;
    ProgressQuestion.Addressee=Npc;
    ProgressQuestion.Source=EDeliveryNpcEvidenceSource::Heard;
    ProgressQuestion.Description=TEXT("这单已经送到了吗？");
    ProgressQuestion.bNeedsReply=true;
    FDeliveryNpcDecisionRequest BeforeSignature;
    if (!TestTrue(TEXT("真实签收前同一主人正在等待订单进度"),Agent->BeginListening(Bike,ProgressQuestion.EventId) &&
        Agent->ObserveEvent(ProgressQuestion) && Agent->BuildDecisionRequest(FGuid::NewGuid(),BeforeSignature))) return false;
    TestTrue(TEXT("问题封存时仍在配送原件"),BeforeSignature.bRunningErrand && BeforeSignature.CompanionPackage.Get()==Package &&
        BeforeSignature.RequesterPC.Get()==OwnerController && BeforeSignature.RequesterPlayerState.Get()==OwnerState &&
        BeforeSignature.PlayerMessageId==ProgressQuestion.EventId);
    const auto* Continue=BeforeSignature.Candidates.FindByPredicate([](const auto& Candidate)
    {
        return Candidate.Action==EDeliveryNpcAction::ContinueCurrent &&
            Candidate.SpeechPreference==EDeliveryCompanionSpeechPreference::Unspecified;
    });
    if (!TestNotNull(TEXT("从实际请求取得继续原配送的候选"),Continue)) return false;
    FDeliveryNpcDecisionProposal StaleProgress;
    StaleProgress.CapabilityVersion=BeforeSignature.RequestedCapabilityVersion;
    StaleProgress.CandidateId=Continue->CandidateId;
    StaleProgress.WorldEpoch=BeforeSignature.WorldEpoch;
    StaleProgress.NpcId=BeforeSignature.NpcId;
    StaleProgress.RequestId=BeforeSignature.RequestId;
    StaleProgress.ContextRevision=BeforeSignature.ContextRevision;
    StaleProgress.PlanRevision=BeforeSignature.PlanRevision;
    StaleProgress.Action=Continue->Action;
    StaleProgress.TargetId=Continue->TargetId;
    StaleProgress.EvidenceIds=Continue->EvidenceIds;
    StaleProgress.Duration=0.f;
    StaleProgress.Speech=TEXT("我还在送这单。");
    Agent->ActiveRequest=BeforeSignature;
    Agent->bDecisionPending=true;
    int32 Confirmations=0;
    const FDelegateHandle Confirmation=Delivery->OnDeliveryConfirmed.AddLambda([&](UDeliveryTaskDefinition* ReceivedTask,AActor* Received,APlayerState* Player)
    {
        ++Confirmations;
        TestTrue(TEXT("收件通知仍能看到同一件原包裹"),ReceivedTask==Task && Received==Package && Player==OwnerState);
        TestNull(TEXT("收件通知前已清掉同伴持件"),Npc->GetCarriedTaskPackage());
        TestNull(TEXT("收件通知中实物已解除原归属"),Received->GetOwner());
    });
    Npc->SetActorLocation(FVector(2240, 0, 88), false, nullptr, ETeleportType::TeleportPhysics);
    Companion->UpdateErrand();
    Delivery->OnDeliveryConfirmed.Remove(Confirmation);
    TestEqual(TEXT("实物收件只通知一次"),Confirmations,1);
    TestEqual(TEXT("主人在远处时仍准确记住是二狗代送"),Companion->GetDeliveryTogetherCount(),1);
    TestNull(TEXT("同步通知结束后清除实际送件者的临时引用"),Tasks->GetCompletingPackageCarrier(Task));
    TestEqual(TEXT("真实收件人收下包裹后二狗才报告送达"), Companion->GetErrand().Reason, FName(TEXT("Delivered")));
    TestFalse(TEXT("实际交付后原物品已销毁"), IsValid(Package));
    TestEqual(TEXT("只有真实收件才完成订单"), Tasks->GetTaskStatus(Task), EDeliveryTaskStatus::Completed);
    TestEqual(TEXT("真实完成后主人钱包才入账"), OwnerWallet->GetBalance(), OriginalBalance + DeliveryReward);
    TestEqual(TEXT("真实完成仍不奖励旁观玩家"), StrangerWallet->GetBalance(), StrangerBalance);
    const auto* Wait=BeforeSignature.Candidates.FindByPredicate([](const auto& Candidate)
        { return Candidate.Action==EDeliveryNpcAction::Wait; });
    if (!TestNotNull(TEXT("原快照也提供普通短暂等待候选"),Wait)) return false;
    auto StaleWait=StaleProgress;
    StaleWait.Action=Wait->Action; StaleWait.CandidateId=Wait->CandidateId;
    StaleWait.TargetId=Wait->TargetId; StaleWait.EvidenceIds=Wait->EvidenceIds;
    StaleWait.Duration=DeliveryNpc::CandidateDuration(Wait->Action);
    FName StaleReason;
    TestFalse(TEXT("旧等待动作也不能携带过期的进度台词"),Agent->TryAcceptProposal(StaleWait,StaleReason));
    TestEqual(TEXT("过期等待和继续原委托都需要刷新"),StaleReason,FName(TEXT("CompanionErrandEnded")));
    AddExpectedMessagePlain(FString::Printf(TEXT("request=%s accepted=0 reason=CompanionErrandEnded"),
        *BeforeSignature.RequestId.ToString()),ELogVerbosity::Display);
    TestFalse(TEXT("签收后拒绝播放旧流式进度回复"),Agent->ReceiveStreamDecision(BeforeSignature.RequestId,StaleProgress));
    TestFalse(TEXT("旧请求已结束且未批准旧台词"),Agent->IsDecisionPending() ||
        Agent->GetPublicState().ReplyToEventId==ProgressQuestion.EventId);
    TestTrue(TEXT("真实委托结束进入已有的有界重试"),Npc->IsDecisionRetryScheduled());
    TestTrue(TEXT("原问题仍在等待同一主人回复"),Agent->IsWaitingToReplyToPlayer() &&
        Agent->GetReplyMessage(OwnerController)==ProgressQuestion.EventId);
    FDeliveryNpcDecisionRequest AfterSignature;
    if (!TestTrue(TEXT("原问题可重建最新签收上下文"),Agent->BuildDecisionRequest(BeforeSignature.WorldEpoch,AfterSignature))) return false;
    TestTrue(TEXT("请求重建保留同一问题与主人而不恢复旧配送"),AfterSignature.PlayerMessageId==ProgressQuestion.EventId &&
        AfterSignature.RequesterPC.Get()==OwnerController && AfterSignature.bPendingPlayerMessage && !AfterSignature.bRunningErrand);
    TestEqual(TEXT("新回复读取原订单的实物签收"),AfterSignature.CompanionErrandTaskId,Task->TaskId.ToString());
    TestEqual(TEXT("新回复读取已送达状态"),AfterSignature.CompanionErrandReason,FString(TEXT("Delivered")));
    TestFalse(TEXT("新回复没有已销毁的包裹"),AfterSignature.bCompanionHasPackage);
    Agent->CancelPlan(TEXT("ProgressAfterSignatureChecked"));
    TestFalse(TEXT("重复完成不能再次结算"), Tasks->TryCompleteDelivery(Task, OwnerState));
    TestEqual(TEXT("送达后仍执行原来的等候安排"),Companion->GetMode(),EDeliveryCompanionMode::Stay);
    TestEqual(TEXT("实际送达保留之前选定的等候锚点"),Companion->WaitLocation,OriginalWaitLocation);
    TestTrue(TEXT("步行代送完成后独立保留回到原站位的安排"),Companion->bReturningToWaitLocation);
    const FVector FeetToOrigin=Npc->GetActorLocation()-Npc->GetNavAgentLocation();
    Npc->SetActorLocation(OriginalWaitLocation+FeetToOrigin+FVector(160,0,0),false,nullptr,ETeleportType::TeleportPhysics);
    Companion->StopFollowMovement();
    TestEqual(TEXT("返回锚点途中不提供原地踱步范围"),Companion->GetWaitAreaRemaining(),0.f);
    Companion->UpdateFollow();
    TestTrue(TEXT("进等候范围后即使受阻仍需重试走回锚点"),Companion->bReturningToWaitLocation);
    TestEqual(TEXT("没有完整导航时报告返程受阻"),Companion->GetSuspensionReason(),FName(TEXT("WaitReturnBlocked")));
    Npc->SetActorLocation(OriginalWaitLocation+FeetToOrigin+FVector(0,0,220),false,nullptr,ETeleportType::TeleportPhysics);
    Companion->NextNavigationAt=0;
    Companion->UpdateFollow();
    TestTrue(TEXT("同平面坐标但在另一层时保持返程"),Companion->bReturningToWaitLocation);
    TestEqual(TEXT("失败重试不能改写原来等候点"),Companion->WaitLocation,OriginalWaitLocation);
    if (!TestTrue(TEXT("主人可在返程受阻时召回"),Apply(EDeliveryNpcAction::Follow,OwnerState,Snapshot()))) return false;
    TestFalse(TEXT("明确改为跟随才取消旧等候返程"),Companion->bReturningToWaitLocation);
    TestEqual(TEXT("受阻和召回不重新修改已完成订单"),Tasks->GetTaskStatus(Task),EDeliveryTaskStatus::Completed);
    TestEqual(TEXT("重复完成不多发奖励"), OwnerWallet->GetBalance(), OriginalBalance + DeliveryReward);
    TestEqual(TEXT("正在骑车的主人只收到一次真实交付事件"),OwnerAudio->PlayCount,1);
    TestEqual(TEXT("旁观者不收到原交付音效事件"),StrangerAudio->PlayCount,0);
    if (!TestTrue(TEXT("真实首单完成后可开始接听下一单"),Tasks->StartTaskTimerFromAcceptedCall(NextTask))) return false;
    Tracker->RequestTrackTask(NextTask);
    const auto NextOrder=Snapshot();
    TestEqual(TEXT("新指令查看新的追踪订单"),NextOrder.CompanionTaskId,NextTask->TaskId.ToString());
    TestEqual(TEXT("旧委托已送达仍归原订单"),NextOrder.CompanionErrandTaskId,Task->TaskId.ToString());
    TestEqual(TEXT("不把旧委托送达写成新订单进度"),NextOrder.CompanionTaskPhase,FString(TEXT("AwaitingPickup")));
    FDeliveryNpcDecisionRequest VisitorAfterDelivery;
    VisitorAfterDelivery.bPlayerRequest=true;
    Companion->ExtendDecisionRequest(VisitorAfterDelivery,StrangerState);
    TestTrue(TEXT("访客没有主人原委托的订单编号"),VisitorAfterDelivery.CompanionErrandTaskId.IsEmpty());

    // 第二单在真实签收通知中同步接受新安排，保留上一单的常规返程对照
    const FVector NextReceiverPosition(3160,350,88);
    auto* NextReceiver=MakeLocation(NextTask->ReceiverNpcId,NextReceiverPosition);
    if (!TestNotNull(TEXT("第二单使用独立的实际收件点"),NextReceiver)) return false;
    auto* NextDelivery=NewObject<UDeliveryTargetComponent>(NextReceiver);
    NextDelivery->ExpectedTask=NextTask;
    NextReceiver->AddInstanceComponent(NextDelivery);
    NextDelivery->RegisterComponent();
    Npc->SetActorLocation(NextReceiverPosition-FVector(60,0,0),false,nullptr,ETeleportType::TeleportPhysics);
    auto* NextPackage=World->SpawnActor<ADeliveryTestPackage>(NextReceiverPosition+FVector(-150,0,62),FRotator::ZeroRotator);
    if (!TestNotNull(TEXT("第二单使用独立的实物包裹"),NextPackage)) return false;
    NextPackage->GetDeliveryItemComponent()->OwningTask=NextTask;
    if (!TestTrue(TEXT("二狗通过真实接收推进第二单配送"),Npc->TryReceiveTaskPackage(NextPackage))) return false;
    const int32 BeforeSecondBalance=OwnerWallet->GetBalance();
    const int32 SecondReward=Tasks->PreviewReward(NextTask).FinalReward;
    TestTrue(TEXT("第二单配置可观察的真实奖励"),SecondReward>0);
    const auto BeforeSecondDelivery=Snapshot();
    if (!TestTrue(TEXT("主人在跟随模式安排第二单代送"),Companion->GetMode()==EDeliveryCompanionMode::Follow &&
        Companion->ApplyCommand(EDeliveryNpcAction::DeliverPackage,OwnerState,BeforeSecondDelivery.CompanionCommandRevision,
            NextTask,NextPackage,EDeliveryCompanionTravelMode::Walk))) return false;

    int32 SecondConfirmations=0;
    int32 StayRevision=INDEX_NONE;
    const FVector SecondWaitAnchor=Npc->GetNavAgentLocation();
    const FDelegateHandle SecondConfirmation=NextDelivery->OnDeliveryConfirmed.AddLambda([&](UDeliveryTaskDefinition* ReceivedTask,AActor* Received,APlayerState* Player)
    {
        ++SecondConfirmations;
        TestTrue(TEXT("第二次签收回调对应真实订单、包裹和主人"),ReceivedTask==NextTask && Received==NextPackage && Player==OwnerState);
        TestEqual(TEXT("签收回调内订单已确认完成"),Tasks->GetTaskStatus(NextTask),EDeliveryTaskStatus::Completed);
        if (TestTrue(TEXT("签收回调中同步接受主人的新等候指令"),Apply(EDeliveryNpcAction::Stay,OwnerState,Snapshot())))
            StayRevision=Companion->GetCommandRevision();
    });
    Companion->UpdateErrand();
    NextDelivery->OnDeliveryConfirmed.Remove(SecondConfirmation);
    TestEqual(TEXT("第二单只签收一次"),SecondConfirmations,1);
    TestFalse(TEXT("第二单真实包裹在签收后销毁"),IsValid(NextPackage));
    TestEqual(TEXT("改派发生后第二单仍已真实完成"),Tasks->GetTaskStatus(NextTask),EDeliveryTaskStatus::Completed);
    TestTrue(TEXT("旧配送返回后保留同步接收的新等候安排"),StayRevision!=INDEX_NONE &&
        Companion->GetMode()==EDeliveryCompanionMode::Stay && Companion->GetCommandRevision()==StayRevision &&
        Companion->WaitLocation.Equals(SecondWaitAnchor) && !Companion->HasActiveErrand());
    TestFalse(TEXT("同帧改派也不能重复完成第二单"),Tasks->TryCompleteDelivery(NextTask,OwnerState));
    TestEqual(TEXT("第二单只累加一次奖励"),OwnerWallet->GetBalance(),BeforeSecondBalance+SecondReward);
    TestEqual(TEXT("主人第二单只收到一次交付音效"),OwnerAudio->PlayCount,2);
    TestEqual(TEXT("旁观者仍未收到交付音效或奖励"),StrangerAudio->PlayCount,0);
    TestEqual(TEXT("旁观者余额未变化"),StrangerWallet->GetBalance(),StrangerBalance);

    const auto BuildReceiptRequest=[&](APawn* Speaker,FDeliveryNpcDecisionRequest& Request)
    {
        FDeliveryNpcObservation Question;
        Question.EventId=FGuid::NewGuid();
        Question.Kind=TEXT("PlayerSpeech");
        Question.Subject=Speaker;
        Question.Addressee=Npc;
        Question.Source=EDeliveryNpcEvidenceSource::Heard;
        Question.Description=TEXT("刚才的包裹送到了吗？");
        Question.bNeedsReply=true;
        return Agent->BeginListening(Speaker,Question.EventId) && Agent->ObserveEvent(Question) &&
            Agent->BuildDecisionRequest(BeforeSignature.WorldEpoch,Request);
    };
    const auto IsSecondReceipt=[NextTask](const FDeliveryNpcEvidence& Fact)
    { return Fact.Kind==TEXT("CompanionTaskDelivered") && Fact.Description.Contains(NextTask->TaskId.ToString()); };
    FDeliveryNpcDecisionRequest OwnerReceipt;
    if (!TestTrue(TEXT("主人可用正常请求追问同步改派前的真实签收"),BuildReceiptRequest(Bike,OwnerReceipt))) return false;
    const auto Receipts=OwnerReceipt.Evidence.FilterByPredicate(IsSecondReceipt);
    TestEqual(TEXT("同一已签收订单只写入一条被动回执"),Receipts.Num(),1);
    TestTrue(TEXT("回执来源是二狗自己的真实执行且不会要求自动作答"),Receipts.Num()==1 &&
        Receipts[0].Source==EDeliveryNpcEvidenceSource::ActionResult && Receipts[0].SubjectId==OwnerReceipt.SelfSubjectId &&
        !Receipts[0].bNeedsReply);
    Agent->CancelPlan(TEXT("OwnerReceiptChecked"));
    FDeliveryNpcDecisionRequest VisitorReceipt;
    if (!TestTrue(TEXT("访客可以单独向二狗提问"),BuildReceiptRequest(Stranger,VisitorReceipt))) return false;
    TestFalse(TEXT("访客快照不暴露主人的被动签收回执"),VisitorReceipt.Evidence.ContainsByPredicate(IsSecondReceipt));
    Agent->CancelPlan(TEXT("VisitorReceiptChecked"));
    return true;
}
#endif
