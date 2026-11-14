#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "AI/Companion/DeliveryCompanionComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Character/DeliveryCharacter.h"
#include "Economy/DeliveryWalletComponent.h"
#include "Game/DeliveryGameState.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Inventory/DeliveryPackageItem.h"
#include "Inventory/DeliveryCaterpillarItem.h"
#include "Interaction/DeliveryInteractableComponent.h"
#include "Player/DeliverPlayerState.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Task/Data/DeliveryTaskDefinition.h"
#include "Task/DeliveryTaskManagerComponent.h"
#include "Task/Tracking/DeliveryTaskTrackerComponent.h"
#include "Task/World/DeliveryItemComponent.h"
#include "Task/World/DeliveryPickupPointComponent.h"
#include "Task/World/DeliveryTargetComponent.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Vehicle/DeliveryVehicleSummonComponent.h"
#include "World/DeliveryLocationComponent.h"
#include "UObject/UnrealType.h"
#include "Engine/Engine.h"
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
    Definitions->ContainerPtrToValuePtr<TArray<TObjectPtr<UDeliveryTaskDefinition>>>(Tasks)->Add(Task);
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    TestEqual(TEXT("登记任务只到未接听状态"), Tasks->GetTaskStatus(Task), EDeliveryTaskStatus::Offered);
    if (!TestTrue(TEXT("真实接听入口开始订单"), Tasks->StartTaskTimerFromAcceptedCall(Task))) return false;
    TestEqual(TEXT("接听后等待实际取件"), Tasks->GetTaskStatus(Task), EDeliveryTaskStatus::AwaitingPickup);

    const auto MakePlayer = [&](const FVector& Location)
    {
        auto* PC = World->SpawnActor<APlayerController>();
        auto* PlayerState = World->SpawnActor<ADeliverPlayerState>();
        PlayerState->SetOwner(PC);
        PC->PlayerState = PlayerState;
        FActorSpawnParameters Spawn;
        Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* Character = World->SpawnActor<ADeliveryCharacter>(Location, FRotator::ZeroRotator, Spawn);
        PC->Possess(Character);
        Character->GetActiveRagdoll()->StopRagdoll();
        Character->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
        return Character;
    };
    auto* Owner = MakePlayer(FVector(0, 0, 88));
    auto* Stranger = MakePlayer(FVector(0, 500, 88));
    auto* OwnerState = Owner->GetPlayerState<ADeliverPlayerState>();
    auto* StrangerState = Stranger->GetPlayerState<ADeliverPlayerState>();
    if (!TestNotNull(TEXT("主人角色持有真实玩家状态"), OwnerState) ||
        !TestNotNull(TEXT("访客角色持有独立玩家状态"), StrangerState)) return false;
    auto* OwnerWallet = OwnerState->GetWallet();
    auto* StrangerWallet = StrangerState->GetWallet();
    const int32 OriginalBalance = OwnerWallet->GetBalance();
    const int32 StrangerBalance = StrangerWallet->GetBalance();

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

    const auto Snapshot = [&]()
    {
        FDeliveryNpcDecisionRequest Request;
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
        TestTrue(TEXT("原订单计时持续有效"), Tasks->GetTimeSnapshot(Task).bRunning);
        TestEqual(TEXT("没有推进世界时间时换手不改计时"), Tasks->GetTimeSnapshot(Task).ElapsedSeconds, AcceptedTime.ElapsedSeconds);
        TestEqual(TEXT("接受和结束跑腿不向主人发奖"), OwnerWallet->GetBalance(), OriginalBalance);
        TestEqual(TEXT("接受和结束跑腿不向访客发奖"), StrangerWallet->GetBalance(), StrangerBalance);
        TestTrue(TEXT("同一实物仍然存在"), IsValid(Package));
    };

    const FDeliveryNpcDecisionRequest BeforeTake = Snapshot();
    TestEqual(TEXT("请求快照指向主人真实携带的实物"), BeforeTake.CompanionPackage.Get(), Package);
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
    const auto BeforeBusyReturn = Snapshot();
    if (!TestTrue(TEXT("主人可安排交还，等待手空"), Apply(EDeliveryNpcAction::ReturnPackage, OwnerState, BeforeBusyReturn))) return false;
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

    Owner->GetInventoryComponent()->ConsumeHeldItem(Held);
    Held->Destroy();
    if (!TestTrue(TEXT("取消后可用新快照重新安排交还"), Apply(EDeliveryNpcAction::ReturnPackage, OwnerState, Snapshot()))) return false;
    Companion->UpdateErrand();
    TestEqual(TEXT("新委托实际交还取消时保留的原实物"), Owner->GetCarriedTaskPackage(), Package);
    TestNull(TEXT("最后交还后同伴不再持件"), Npc->GetCarriedTaskPackage());
    TestEqual(TEXT("新委托交还成功后旧暂停不会覆盖实际结果"), Snapshot().CompanionErrandReason, FString(TEXT("PackageReturned")));
    CheckNotDelivered();

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
        // 只替代模型传输，不伪造服务器候选、任务状态或提议采纳结果
        Agent->ActiveRequest = Request;
        Agent->bDecisionPending = true;
        Proposal = {};
        Proposal.CapabilityVersion = Request.RequestedCapabilityVersion;
        Proposal.CandidateId = Candidate->CandidateId;
        Proposal.WorldEpoch = Request.WorldEpoch;
        Proposal.NpcId = Request.NpcId;
        Proposal.RequestId = Request.RequestId;
        Proposal.ContextRevision = Request.ContextRevision;
        Proposal.PlanRevision = Request.PlanRevision;
        Proposal.Action = Candidate->Action;
        Proposal.TargetId = Candidate->TargetId;
        Proposal.EvidenceIds = Candidate->EvidenceIds;
        Proposal.Duration = 0;
        return true;
    };

    FDeliveryNpcDecisionRequest BeforeDestroy;
    FDeliveryNpcDecisionProposal OldDeliveryReply;
    if (!TestTrue(TEXT("实际持件时生成主人 Deliver 请求与合法候选"), PrepareDeliveryReply(BeforeDestroy, OldDeliveryReply))) return false;
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
    Companion->UpdateCaterpillarOpportunity();
    FDeliveryNpcDecisionRequest OpportunityRequest;
    if (!TestTrue(TEXT("院子机会经正常 Agent 构建主动请求"),Agent->BuildDecisionRequest(FGuid::NewGuid(),OpportunityRequest))) return false;
    TestFalse(TEXT("主动机会不伪造主人发言身份"),OpportunityRequest.bRequesterIsCompanionOwner);
    TestTrue(TEXT("主动机会只开放小绿提议"),OpportunityRequest.AllowedActions.Contains(EDeliveryNpcAction::OfferCaterpillar));
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
    TestFalse(TEXT("重复E不会再给一条"),EAction->Use.Execute(Owner));
    TestFalse(TEXT("接过小绿仍没有自动投放外卖"),Package->ContainsCaterpillar());
    CheckNotDelivered();

    // 用同一委托转移、交付及结算；测试直接布置站位，路径和画面由试玩检查
    const int32 DeliveryReward = Tasks->PreviewReward(Task).FinalReward;
    TestTrue(TEXT("正对照奖励配置非零"), DeliveryReward > 0);
    // 先复现开箱被打断，再验证主人升级为代送会重新申请同一个包裹
    auto* Bike=World->SpawnActor<ADeliveryMotorbike>(FVector(2600,0,88),FRotator::ZeroRotator);
    auto* Summon=Owner->FindComponentByClass<UDeliveryVehicleSummonComponent>();
    if (!TestNotNull(TEXT("生成真实主人车辆"),Bike) || !TestNotNull(TEXT("主人车辆登记组件存在"),Summon)) return false;
    Summon->RegisterVehicle(Bike);
    if (!TestTrue(TEXT("把原包裹放进登记车辆"),Owner->ReleaseTaskPackage(Package) && Bike->AttachCargoForPlayer(Package,OwnerState))) return false;
    Npc->SetActorLocation(Bike->GetTaskPackageTransferPoint()-FVector(150,0,0),false,nullptr,ETeleportType::TeleportPhysics);
    if (!TestTrue(TEXT("主人先要求接件"),Apply(EDeliveryNpcAction::TakePackage,OwnerState,Snapshot()))) return false;
    Companion->UpdateErrand();
    const FGuid Interrupted=Companion->ErrandCargoHandoffId;
    if (!TestTrue(TEXT("实际开箱已接收"),Interrupted.IsValid())) return false;
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
    Bike->CancelCargoHandoff(Interrupted);
    Bike->UpdateTrunkAnimation(10.f);
    Companion->UpdateErrand();
    TestEqual(TEXT("帮送先从主人车上接到同一个包裹"), Npc->GetCarriedTaskPackage(), Package);
    TestNull(TEXT("实际转移后后备箱为空"),Bike->GetCargo());
    Bike->Destroy();
    CheckNotDelivered();
    Npc->SetActorLocation(FVector(1980, 0, 88), false, nullptr, ETeleportType::TeleportPhysics);
    Companion->UpdateErrand();
    TestTrue(TEXT("二狗到门外地址仍保留委托"), Companion->HasActiveErrand());
    CheckNotDelivered();
    const auto RejectRelease=[&]()
    {
        TestFalse(TEXT("解除携带时的重复结算被拒绝"),Tasks->TryCompleteDelivery(Task,OwnerState));
        return false;
    };
    TestFalse(TEXT("实物释放失败时不结算"),Tasks->CompleteDelivery(Task,OwnerState,Npc,FGameplayTag(),RejectRelease));
    CheckNotDelivered();
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
    TestFalse(TEXT("重复完成不能再次结算"), Tasks->TryCompleteDelivery(Task, OwnerState));
    TestEqual(TEXT("重复完成不多发奖励"), OwnerWallet->GetBalance(), OriginalBalance + DeliveryReward);
    return true;
}
#endif
