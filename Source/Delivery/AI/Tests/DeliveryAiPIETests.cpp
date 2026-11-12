#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "AI/DeliveryAiGatewaySubsystem.h"
#include "AI/DeliveryAiSettings.h"
#include "AI/DeliveryNpcAgentComponent.h"
#include "AI/BTTask_DeliveryExecuteApprovedAction.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Pawn.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/StrongObjectPtr.h"

// 真正通过本机 HTTP 后端和监听服务器复制，联调桩不是模型效果验收
class FDeliveryAiPIECommand : public IAutomationLatentCommand
{
public:
    FDeliveryAiPIECommand(FAutomationTestBase* InTest, FString InEndpoint) : Test(InTest), Endpoint(MoveTemp(InEndpoint)) {}
    bool Update() override
    {
        UWorld* Server = nullptr;
        UWorld* Client = nullptr;
        for (const auto& Context : GEngine->GetWorldContexts())
        {
            if (Context.WorldType != EWorldType::PIE || !Context.World()) continue;
            if (Context.World()->GetNetMode() == NM_Client) Client = Context.World();
            else Server = Context.World();
        }
        if (bEnding)
        {
            if (Server || Client) return false;
            RestoreSettings();
            return true;
        }
        if (!bRequested)
        {
            if (Server || Client) { Test->AddError(TEXT("测试需要空闲编辑器，不接管已有 PIE")); return true; }
            if (!FEditorFileUtils::LoadMap(TEXT("/Engine/Maps/Entry"), false, false))
            { Test->AddError(TEXT("无法载入基础设施测试地图")); return true; }
            auto* Config = GetMutableDefault<UDeliveryAiSettings>();
            bOldEnabled = Config->bEnabled;
            OldEndpoint = Config->DecisionEndpoint;
            Config->bEnabled = true;
            Config->DecisionEndpoint = Endpoint;
            bChangedSettings = true;
            Settings.Reset(DuplicateObject<ULevelEditorPlaySettings>(GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage()));
            Settings->SetPlayNetMode(PIE_ListenServer);
            Settings->SetPlayNumberOfClients(2);
            Settings->SetRunUnderOneProcess(true);
            FRequestPlaySessionParams Params;
            Params.EditorPlaySettings = Settings.Get();
            Params.bAllowOnlineSubsystem = false;
            GEditor->RequestPlaySession(Params);
            bRequested = true;
            Started = FPlatformTime::Seconds();
            return false;
        }
        if (FPlatformTime::Seconds() - Started > 90)
        {
            Test->AddError(FString::Printf(TEXT("HTTP/多人基础设施测试超时 stage=%d"), Stage));
            return End();
        }
        if (!Server || !Client || !Client->GetFirstPlayerController()) return false;
        auto* ClientGateway = Client->GetSubsystem<UDeliveryAiGatewaySubsystem>();
        if (Stage == 0)
        {
            APawn* Pawn = Server->SpawnActor<APawn>();
            Pawn->SetReplicates(true);
            Pawn->bAlwaysRelevant = true;
            auto* Root = NewObject<USceneComponent>(Pawn);
            Pawn->SetRootComponent(Root);
            Root->RegisterComponent();
            auto* Agent = NewObject<UDeliveryNpcAgentComponent>(Pawn, TEXT("AiRegressionAgent"));
            Pawn->AddInstanceComponent(Agent);
            Agent->RegisterComponent();
            ServerAgent = Agent;
            OwnerName = Pawn->GetFName();
            auto* Controller = Server->SpawnActor<AAIController>();
            Controller->Possess(Pawn);
            ServerController = Controller;
            FDeliveryNpcObservation Event;
            Event.EventId = FGuid::NewGuid();
            Event.Kind = TEXT("RegressionObservation");
            Event.Description = TEXT("只属于服务器的测试证据");
            Agent->ObserveEvent(Event);
            if (!Test->TestTrue(TEXT("服务器向真实 HTTP 队列提交请求"), Agent->RequestDecision())) return End();
            Stage = 1;
        }
        if (!RemoteAgent.IsValid())
            for (TActorIterator<APawn> It(Client); It; ++It)
                if (It->GetFName() == OwnerName) RemoteAgent = It->FindComponentByClass<UDeliveryNpcAgentComponent>();
        if (Stage == 4)
        {
            if (RemoteAgent.IsValid()) return false;
            Test->AddInfo(TEXT("HTTP → 服务器审批 → BT 等待 → 客户端复制 → 销毁清理完成"));
            return End();
        }
        auto* Agent = ServerAgent.Get();
        if (!Agent) { Test->AddError(TEXT("测试服务器 Agent 提前销毁")); return End(); }
        const auto State = Agent->GetPublicState();
        if (Stage == 1 && State.Status == EDeliveryNpcActionStatus::Accepted)
        {
            Test->TestFalse(TEXT("HTTP 返回后请求预算已释放"), Agent->IsDecisionPending());
            auto* Tree = NewObject<UBehaviorTree>(ServerController.Get());
            Tree->RootNode = NewObject<UBTComposite_Sequence>(Tree);
            FBTCompositeChild Child;
            Child.ChildTask = NewObject<UBTTask_DeliveryExecuteApprovedAction>(Tree);
            Tree->RootNode->Children.Add(Child);
            if (!Test->TestTrue(TEXT("服务器行为树开始执行 HTTP 建议"), ServerController->RunBehaviorTree(Tree))) return End();
            Stage = 2;
        }
        else if (Stage == 1 && !Agent->IsDecisionPending())
        {
            Test->AddError(TEXT("HTTP 没有产生 Accepted 动作，请检查联调服务及协议"));
            return End();
        }
        if (!RemoteAgent.IsValid()) return false;
        const auto RemoteState = RemoteAgent->GetPublicState();
        if (Stage == 2 && RemoteState.Status == EDeliveryNpcActionStatus::Running)
        {
            Test->TestEqual(TEXT("双方看到同一动作标识"), RemoteState.ActionId, State.ActionId);
            Test->TestEqual(TEXT("双方共享服务器动作起点"), RemoteState.ServerStartTime, State.ServerStartTime);
            Test->TestFalse(TEXT("客户端不能请求模型"), RemoteAgent->RequestDecision());
            FDeliveryNpcObservation Event; Event.Kind = TEXT("ForgedEvent");
            Test->TestFalse(TEXT("客户端不能伪造服务器证据"), RemoteAgent->ObserveEvent(Event));
            Test->TestFalse(TEXT("客户端不能完成动作"), RemoteAgent->TryFinishAction(RemoteState.ActionId, EDeliveryNpcActionStatus::Succeeded, NAME_None));
            Test->TestFalse(TEXT("私有 NPC 标识没有复制到客户端"), RemoteAgent->GetNpcId().IsValid());
            Test->TestEqual(TEXT("客户端 World 没有模型请求"), ClientGateway->GetPendingCount(), 0);
            Stage = 3;
        }
        if (Stage == 3 && State.Status == EDeliveryNpcActionStatus::Succeeded && RemoteState.Status == EDeliveryNpcActionStatus::Succeeded)
        {
            Test->TestEqual(TEXT("真实 BT 完成结果复制到远端"), State.ActionId, RemoteState.ActionId);
            if (!Test->TestTrue(TEXT("结束后可再次提交"), Agent->RequestDecision())) return End();
            Agent->CancelDecision();
            Test->TestEqual(TEXT("取消请求立即释放服务器预算"), Server->GetSubsystem<UDeliveryAiGatewaySubsystem>()->GetPendingCount(), 0);
            Agent->GetOwner()->Destroy();
            Stage = 4;
        }
        if (Stage == 4)
        {
            if (RemoteAgent.IsValid()) return false;
            Test->AddInfo(TEXT("HTTP → 服务器审批 → BT 等待 → 客户端复制 → 销毁清理完成"));
            return End();
        }
        return false;
    }
    ~FDeliveryAiPIECommand() { RestoreSettings(); }

private:
    bool End() { GEditor->RequestEndPlayMap(); bEnding = true; return false; }
    void RestoreSettings()
    {
        if (!bChangedSettings) return;
        auto* Config = GetMutableDefault<UDeliveryAiSettings>();
        Config->bEnabled = bOldEnabled;
        Config->DecisionEndpoint = OldEndpoint;
        bChangedSettings = false;
    }
    FAutomationTestBase* Test;
    FString Endpoint, OldEndpoint;
    FName OwnerName;
    TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
    TWeakObjectPtr<UDeliveryNpcAgentComponent> ServerAgent, RemoteAgent;
    TWeakObjectPtr<AAIController> ServerController;
    bool bOldEnabled = false, bChangedSettings = false, bRequested = false, bEnding = false;
    double Started = 0;
    int32 Stage = 0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryAiPIETest, "Delivery.AI.ListenServerHttp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryAiPIETest::RunTest(const FString&)
{
    FString Endpoint;
    if (!FApp::CanEverRender() || !FParse::Value(FCommandLine::Get(), TEXT("DeliveryAITestEndpoint="), Endpoint))
    {
        AddError(TEXT("需要真实双人 PIE 和显式 -DeliveryAITestEndpoint=http://127.0.0.1:端口/v1/decisions 联调服务"));
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FDeliveryAiPIECommand(this, Endpoint));
    return true;
}
#endif
