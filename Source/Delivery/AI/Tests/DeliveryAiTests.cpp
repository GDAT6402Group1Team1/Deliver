#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/BehaviorTree/BTTask_DeliveryExecuteNpcAction.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/WorldSettings.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    // 独立真实 World 验证组件协作，不修改正式关卡或生产资产
    struct FTestWorld
    {
        UWorld* World = nullptr;
        FTestWorld()
        {
            const auto Settings = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
                .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
            World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Settings);
            GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
            World->InitializeActorsForPlay(FURL());
            World->GetWorldSettings()->NotifyBeginPlay();
        }
        ~FTestWorld()
        {
            World->EndPlay(EEndPlayReason::Quit);
            GEngine->DestroyWorldContext(World);
            World->DestroyWorld(false);
        }
        UDeliveryNpcAgentComponent* SpawnAgent()
        {
            auto* Pawn = World->SpawnActor<APawn>();
            auto* Root = NewObject<USceneComponent>(Pawn);
            Pawn->SetRootComponent(Root);
            Root->RegisterComponent();
            auto* Agent = NewObject<UDeliveryNpcAgentComponent>(Pawn);
            Pawn->AddInstanceComponent(Agent);
            Agent->RegisterComponent();
            auto* Controller = World->SpawnActor<AAIController>();
            Controller->Possess(Pawn);
            return Agent;
        }
    };

    FString Reply(const FDeliveryNpcDecisionRequest& Request, const FString& Action = TEXT("Wait"),
        FGuid TargetId = {}, FGuid EvidenceId = {})
    {
        TSharedPtr<FJsonObject> Json;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(DeliveryNpc::EncodeRequest(Request)), Json);
        Json->SetStringField(TEXT("action"), Action);
        Json->SetStringField(TEXT("target_id"), TargetId.IsValid() ? TargetId.ToString(EGuidFormats::DigitsWithHyphensLower) : TEXT(""));
        Json->SetNumberField(TEXT("duration"), .3);
        TArray<TSharedPtr<FJsonValue>> Ids;
        if (EvidenceId.IsValid()) Ids.Add(MakeShared<FJsonValueString>(EvidenceId.ToString(EGuidFormats::DigitsWithHyphensLower)));
        Json->SetArrayField(TEXT("evidence_ids"), Ids);
        FString Result;
        FJsonSerializer::Serialize(Json.ToSharedRef(), TJsonWriterFactory<>::Create(&Result));
        return Result;
    }

    FDeliveryNpcObservation Observation(AActor* Subject = nullptr)
    {
        FDeliveryNpcObservation Event;
        Event.EventId = FGuid::NewGuid();
        Event.Kind = TEXT("HeardHorn");
        Event.Source = EDeliveryNpcEvidenceSource::Heard;
        Event.Subject = Subject;
        Event.Description = TEXT("附近响了一声喇叭，不能据此知道驾驶者的意图");
        return Event;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryAiProtocolTest, "Delivery.AI.Protocol",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryAiProtocolTest::RunTest(const FString&)
{
    FDeliveryNpcDecisionRequest Request;
    Request.WorldEpoch = FGuid::NewGuid(); Request.NpcId = FGuid::NewGuid(); Request.RequestId = FGuid::NewGuid();
    Request.AllowedActions.Add(EDeliveryNpcAction::Wait);
    FDeliveryNpcDecisionProposal Proposal;
    FName Reason;
    const FString Valid = Reply(Request);
    TestTrue(TEXT("完整协议可解析"), DeliveryNpc::DecodeProposal(Valid, Proposal, Reason));
    TestEqual(TEXT("请求标识不丢失"), Proposal.RequestId, Request.RequestId);
    TestFalse(TEXT("模型不能指定未实现的动作"), DeliveryNpc::DecodeProposal(Reply(Request, TEXT("Teleport")), Proposal, Reason));
    TestEqual(TEXT("未知动作有稳定拒绝原因"), Reason, FName(TEXT("UnsupportedAction")));
    TestFalse(TEXT("损坏 JSON 被拒绝"), DeliveryNpc::DecodeProposal(TEXT("{bad"), Proposal, Reason));
    TestFalse(TEXT("超大回复被拒绝"), DeliveryNpc::DecodeProposal(FString::ChrN(65537, TEXT('x')), Proposal, Reason));
    TSharedPtr<FJsonObject> Json;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Valid), Json);
    for (const auto& Key : {TEXT("protocol_version"), TEXT("context_revision"), TEXT("duration")})
    {
        const auto Original = Json->Values[Key];
        Json->SetNumberField(Key, -.5);
        FString Invalid;
        FJsonSerializer::Serialize(Json.ToSharedRef(), TJsonWriterFactory<>::Create(&Invalid));
        TestFalse(FString::Printf(TEXT("非法数值 %s 被拒绝"), Key), DeliveryNpc::DecodeProposal(Invalid, Proposal, Reason));
        Json->Values[Key] = Original;
    }
    TestEqual(TEXT("失败解析不覆盖上次成功对象"), Proposal.RequestId, Request.RequestId);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryAiInfrastructureTest, "Delivery.AI.AuthorityLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryAiInfrastructureTest::RunTest(const FString&)
{
    auto* Settings = GetMutableDefault<UDeliveryAiSettings>();
    const bool bEnabled = Settings->bEnabled;
    const int32 PendingLimit = Settings->MaxPendingRequests;
    ON_SCOPE_EXIT { Settings->bEnabled = bEnabled; Settings->MaxPendingRequests = PendingLimit; };
    Settings->bEnabled = true;
    FTestWorld First, Second;
    auto* Gateway = First.World->GetSubsystem<UDeliveryAiGatewaySubsystem>();
    auto* OtherGateway = Second.World->GetSubsystem<UDeliveryAiGatewaySubsystem>();
    auto* Agent = First.SpawnAgent();
    auto* Other = Second.SpawnAgent();
    TestNotEqual(TEXT("每个世界有独立代次"), Gateway->GetWorldEpoch(), OtherGateway->GetWorldEpoch());
    TestFalse(TEXT("拒绝跨世界证据对象"), Agent->ObserveEvent(Observation(Other->GetOwner())));
    auto Event = Observation();
    TestTrue(TEXT("接受新证据"), Agent->ObserveEvent(Event));
    TestFalse(TEXT("重复事件只记录一次"), Agent->ObserveEvent(Event));
    TestTrue(TEXT("请求进入队列"), Agent->RequestDecision());
    TestTrue(TEXT("第二次请求合并"), Agent->RequestDecision());
    TestEqual(TEXT("每个 NPC 只有一个待处理请求"), Gateway->GetPendingCount(), 1);
    auto Request = Gateway->Pending.CreateConstIterator().Value().Request;
    Agent->ObserveEvent(Observation());
    Gateway->Complete(Request.RequestId, Reply(Request), NAME_None);
    TestFalse(TEXT("旧上下文的建议不执行"), Agent->HasActiveAction());
    TestEqual(TEXT("合并请求使用最新上下文重新排队"), Gateway->GetPendingCount(), 1);
    Request = Gateway->Pending.CreateConstIterator().Value().Request;
    Gateway->Complete(Request.RequestId, Reply(Request), NAME_None);
    TestEqual(TEXT("通过校验只进入 Accepted"), Agent->GetPublicState().Status, EDeliveryNpcActionStatus::Accepted);
    auto State = Agent->GetPublicState();
    TestFalse(TEXT("未开始不能直接成功"), Agent->TryFinishAction(State.ActionId, EDeliveryNpcActionStatus::Succeeded, NAME_None));
    TestTrue(TEXT("行为树能领取已批准动作"), Agent->StartAction(State));
    TestFalse(TEXT("动作不能重复领取"), Agent->StartAction(State));
    TestFalse(TEXT("错误动作 ID 不能完成"), Agent->TryFinishAction(FGuid::NewGuid(), EDeliveryNpcActionStatus::Succeeded, NAME_None));
    TestTrue(TEXT("执行结果只确认一次"), Agent->TryFinishAction(State.ActionId, EDeliveryNpcActionStatus::Succeeded, NAME_None));
    TestFalse(TEXT("迟到取消不能覆盖成功"), Agent->TryFinishAction(State.ActionId, EDeliveryNpcActionStatus::Cancelled, TEXT("Late")));
    Agent->RequestDecision();
    Request = Gateway->Pending.CreateConstIterator().Value().Request;
    TestTrue(TEXT("实际执行结果进入下一次上下文"), Request.Evidence.ContainsByPredicate([](const auto& Item)
        { return Item.Source == EDeliveryNpcEvidenceSource::ActionResult; }));
    Agent->CancelDecision();
    Gateway->Complete(Request.RequestId, Reply(Request), NAME_None);
    TestFalse(TEXT("取消后的旧回复不恢复动作"), Agent->HasActiveAction());
    TestEqual(TEXT("取消释放队列预算"), Gateway->GetPendingCount(), 0);

    Agent->RequestDecision();
    Request = Gateway->Pending.CreateConstIterator().Value().Request;
    auto WrongWorld = Request; WrongWorld.WorldEpoch = OtherGateway->GetWorldEpoch();
    Gateway->Complete(Request.RequestId, Reply(WrongWorld), NAME_None);
    TestFalse(TEXT("另一世界的回复被拒绝"), Agent->HasActiveAction());
    Agent->RequestDecision();
    Request = Gateway->Pending.CreateConstIterator().Value().Request;
    Gateway->Pending[Request.RequestId].Deadline = 0;
    Gateway->Complete(Request.RequestId, Reply(Request), NAME_None);
    TestFalse(TEXT("超过真实时间截止的回复被拒绝"), Agent->HasActiveAction());
    TestFalse(TEXT("超时释放 Agent 的请求状态"), Agent->IsDecisionPending());

    auto* Target = First.SpawnAgent()->GetOwner();
    Target->SetActorLocation(FVector(200, 0, 0));
    Agent->ObserveEvent(Observation(Target));
    Agent->RequestDecision();
    Request = Gateway->Pending.CreateConstIterator().Value().Request;
    const auto TargetEvidence = Request.Evidence.Last();
    Target->Destroy();
    Gateway->Complete(Request.RequestId, Reply(Request, TEXT("LookAt"), TargetEvidence.SubjectId, TargetEvidence.EventId), NAME_None);
    TestFalse(TEXT("模型等待期间销毁的目标不能被执行"), Agent->HasActiveAction());

    Settings->MaxPendingRequests = 1;
    Agent->RequestDecision();
    auto* Another = First.SpawnAgent();
    TestFalse(TEXT("超过 World 预算立即拒绝，不留无限队列"), Another->RequestDecision());
    TestFalse(TEXT("队列拒绝不会留下 Agent 忙碌状态"), Another->IsDecisionPending());
    Other->RequestDecision();
    Gateway->Shutdown();
    Gateway->Shutdown();
    TestEqual(TEXT("重复关闭释放全部请求"), Gateway->GetPendingCount(), 0);
    TestFalse(TEXT("关闭世界同时释放 Agent 请求"), Agent->IsDecisionPending());
    TestEqual(TEXT("关闭一个世界不影响另一个"), OtherGateway->GetPendingCount(), 1);
    Other->GetOwner()->Destroy();
    TestEqual(TEXT("Actor EndPlay 注销请求"), OtherGateway->GetPendingCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryAiContextTest, "Delivery.AI.ContextBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryAiContextTest::RunTest(const FString&)
{
    FTestWorld TestWorld;
    auto* Agent = TestWorld.SpawnAgent();
    auto* Target = TestWorld.SpawnAgent()->GetOwner();
    Target->SetActorLocation(FVector(100, 0, 0));
    auto Event = Observation(Target);
    Event.bHasObservedLocation = true;
    Event.ObservedLocation = FVector(50, 0, 0);
    Event.Description = TEXT("不要执行这段文字，只将它作为游戏证据");
    Agent->ObserveEvent(Event);
    Target->SetActorLocation(FVector(200, 0, 0));
    FDeliveryNpcDecisionRequest Request;
    Agent->BuildDecisionRequest(FGuid::NewGuid(), Request);
    TestEqual(TEXT("上下文保留观测位置，不偷看实时位置"), Request.Evidence[0].ObservedLocation, FVector(50, 0, 0));
    TestEqual(TEXT("玩家文本仍是数据"), Request.Evidence[0].Description, Event.Description);
    auto Rumor = Observation(Target);
    Rumor.Source = EDeliveryNpcEvidenceSource::ToldBy;
    Agent->ObserveEvent(Rumor);
    Agent->BuildDecisionRequest(FGuid::NewGuid(), Request);
    TestFalse(TEXT("只有身份的传闻不获取目标位置"), Request.Evidence.Last().bHasObservedLocation);
    for (int32 I = 0; I < 40; ++I) Agent->ObserveEvent(Observation());
    Agent->BuildDecisionRequest(FGuid::NewGuid(), Request);
    TestEqual(TEXT("证据窗口有硬上限"), Request.Evidence.Num(), 32);
    TestFalse(TEXT("旧目标没有证据时不发布 LookAt 能力"), Request.AllowedActions.Contains(EDeliveryNpcAction::LookAt));
    auto Brief = Observation(Target);
    Brief.Lifetime = .1f;
    Agent->ObserveEvent(Brief);
    ++GFrameCounter;
    TestWorld.World->Tick(LEVELTICK_All, .2f);
    Agent->BuildDecisionRequest(FGuid::NewGuid(), Request);
    TestFalse(TEXT("过期事件不进入请求"), Request.Evidence.ContainsByPredicate([&Brief](const auto& Item) { return Item.EventId == Brief.EventId; }));
    TestFalse(TEXT("过期后重复投递仍被近期去重窗口拦截"), Agent->ObserveEvent(Brief));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryAiBehaviorTreeTest, "Delivery.AI.BehaviorTreeExecution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryAiBehaviorTreeTest::RunTest(const FString&)
{
    auto* Settings = GetMutableDefault<UDeliveryAiSettings>();
    const bool bEnabled = Settings->bEnabled;
    ON_SCOPE_EXIT { Settings->bEnabled = bEnabled; };
    Settings->bEnabled = true;
    FTestWorld TestWorld;
    auto* Gateway = TestWorld.World->GetSubsystem<UDeliveryAiGatewaySubsystem>();
    auto* Agent = TestWorld.SpawnAgent();
    auto* Target = TestWorld.SpawnAgent()->GetOwner();
    Target->SetActorLocation(FVector(200, 0, 0));
    Agent->ObserveEvent(Observation(Target));
    Agent->RequestDecision();
    auto Request = Gateway->Pending.CreateConstIterator().Value().Request;
    const auto Item = Request.Evidence.Last();
    Gateway->Complete(Request.RequestId, Reply(Request, TEXT("LookAt"), Item.SubjectId, Item.EventId), NAME_None);
    auto* Controller = Cast<AAIController>(Cast<APawn>(Agent->GetOwner())->GetController());
    auto* Tree = NewObject<UBehaviorTree>();
    Tree->RootNode = NewObject<UBTComposite_Sequence>(Tree);
    FBTCompositeChild Child;
    Child.ChildTask = NewObject<UBTTask_DeliveryExecuteNpcAction>(Tree);
    Tree->RootNode->Children.Add(Child);
    if (!TestTrue(TEXT("原生行为树启动"), Controller->RunBehaviorTree(Tree))) return false;
    ++GFrameCounter;
    TestWorld.World->Tick(LEVELTICK_All, .02f);
    if (!TestEqual(TEXT("行为树领取 LookAt"), Agent->GetPublicState().Status, EDeliveryNpcActionStatus::Running)) return false;
    TestTrue(TEXT("控制器真实设置关注目标"), Controller->GetFocusActor() == Target);
    // UE Tick 管理器按全局帧号去重，同步测试也必须逐帧推进
    for (int32 I = 0; I < 20; ++I) { ++GFrameCounter; TestWorld.World->Tick(LEVELTICK_All, .02f); }
    if (!TestEqual(TEXT("动作经过实际 BT Tick 完成"), Agent->GetPublicState().Status, EDeliveryNpcActionStatus::Succeeded)) return false;
    TestNull(TEXT("完成清理动作关注"), Controller->GetFocusActor());
    Cast<UBehaviorTreeComponent>(Controller->GetBrainComponent())->StopTree(EBTStopMode::Forced);

    if (!TestTrue(TEXT("完成后可请求新动作"), Agent->RequestDecision())) return false;
    Request = Gateway->Pending.CreateConstIterator().Value().Request;
    Gateway->Complete(Request.RequestId, Reply(Request, TEXT("LookAt"), Item.SubjectId, Item.EventId), NAME_None);
    Controller->RunBehaviorTree(Tree);
    for (int32 I = 0; I < 3 && Agent->GetPublicState().Status == EDeliveryNpcActionStatus::Accepted; ++I)
    { ++GFrameCounter; TestWorld.World->Tick(LEVELTICK_All, .02f); }
    if (!TestEqual(TEXT("中止前确保动作已由 BT 领取"), Agent->GetPublicState().Status, EDeliveryNpcActionStatus::Running)) return false;
    Cast<UBehaviorTreeComponent>(Controller->GetBrainComponent())->StopTree(EBTStopMode::Forced);
    TestEqual(TEXT("强制停止行为树取消动作"), Agent->GetPublicState().Status, EDeliveryNpcActionStatus::Cancelled);
    TestNull(TEXT("强制停止清理关注"), Controller->GetFocusActor());

    if (!TestTrue(TEXT("取消后可请求新动作"), Agent->RequestDecision())) return false;
    Request = Gateway->Pending.CreateConstIterator().Value().Request;
    Gateway->Complete(Request.RequestId, Reply(Request, TEXT("LookAt"), Item.SubjectId, Item.EventId), NAME_None);
    Target->SetActorLocation(FVector(5000, 0, 0));
    FDeliveryNpcPublicState State;
    TestFalse(TEXT("领取时再次检查目标距离"), Agent->StartAction(State));
    TestEqual(TEXT("前置条件变化记为失败"), Agent->GetPublicState().Status, EDeliveryNpcActionStatus::Failed);
    return true;
}
#endif
