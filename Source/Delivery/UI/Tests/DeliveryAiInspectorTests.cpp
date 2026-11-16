#include "Misc/AutomationTest.h"
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/ScopeExit.h"
#include "UI/Controllers/DeliveryAiInspectorController.h"
#include "UI/Conversation/DeliveryAiInspectorWidget.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryAiInspectorLifecycleTest,"Delivery.AI.InspectorLifecycle",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryAiInspectorLifecycleTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool bWasEnabled=Settings->bEnabled;
    Settings->bEnabled=false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
        Settings->bEnabled=bWasEnabled;
    };
    auto MakePawn=[&](FVector Location)
    {
        auto* Pawn=World->SpawnActor<APawn>();
        auto* Root=NewObject<USceneComponent>(Pawn);
        Pawn->SetRootComponent(Root); Root->RegisterComponent(); Pawn->SetActorLocation(Location);
        return Pawn;
    };
    auto MakePlayer=[&](FVector Location)
    {
        auto* PC=World->SpawnActor<APlayerController>();
        PC->Player=NewObject<ULocalPlayer>(GEngine);
        PC->PlayerState=World->SpawnActor<APlayerState>(); PC->PlayerState->SetIsABot(false);
        PC->Possess(MakePawn(Location));
        auto* Source=NewObject<UDeliveryConversationComponent>(PC);
        PC->AddInstanceComponent(Source); Source->RegisterComponent();
        return PC;
    };
    auto* FirstPlayer=MakePlayer(FVector(0,0,90));
    auto* SecondPlayer=MakePlayer(FVector(0,200,90));
    auto* FirstSource=FirstPlayer->FindComponentByClass<UDeliveryConversationComponent>();
    auto* SecondSource=SecondPlayer->FindComponentByClass<UDeliveryConversationComponent>();
    FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* FirstNpc=World->SpawnActor<ADeliveryNpcCharacter>(FVector(200,0,90),FRotator::ZeroRotator,Spawn);
    auto* SecondNpc=World->SpawnActor<ADeliveryNpcCharacter>(FVector(200,200,90),FRotator::ZeroRotator,Spawn);
    FirstNpc->SetActorTickEnabled(false); SecondNpc->SetActorTickEnabled(false);
    FirstNpc->DisplayName=FText::FromString(TEXT("观察甲")); SecondNpc->DisplayName=FText::FromString(TEXT("观察乙"));

    // 原生 Widget 仅接收数据显示；本测试不构建 Slate，也不把委托调用当成真实按钮验收
    auto* FirstWidget=CreateWidget<UDeliveryAiInspectorWidget>(FirstPlayer);
    auto* SecondWidget=CreateWidget<UDeliveryAiInspectorWidget>(SecondPlayer);
    if (!TestNotNull(TEXT("第一个玩家的原生展示夹具"),FirstWidget) ||
        !TestNotNull(TEXT("第二个玩家的原生展示夹具"),SecondWidget)) return false;
    auto* First=NewObject<UDeliveryAiInspectorController>(FirstPlayer);
    auto* Second=NewObject<UDeliveryAiInspectorController>(SecondPlayer);
    ON_SCOPE_EXIT { First->Shutdown(); Second->Shutdown(); };
    First->Initialize(FirstPlayer,nullptr,SecondWidget);
    TestFalse(TEXT("不能把另一玩家的界面绑给当前玩家"),First->bInitialized);
    First->Initialize(FirstPlayer,nullptr,FirstWidget);
    First->Initialize(FirstPlayer,nullptr,FirstWidget);
    Second->Initialize(SecondPlayer,nullptr,SecondWidget);
    if (!TestTrue(TEXT("真实本机拥有者初始化成功"),First->bInitialized && Second->bInitialized)) return false;
    First->ObserveNpc(FirstNpc); Second->ObserveNpc(SecondNpc);
    TestTrue(TEXT("服务器分别绑定各玩家选择的 NPC"),FirstSource->ServerInspectorTarget.Get()==FirstNpc &&
        SecondSource->ServerInspectorTarget.Get()==SecondNpc);
    const uint64 FirstGeneration=FirstSource->LocalInspectorGeneration;
    const FGuid FirstEpoch=FGuid::NewGuid(),SecondEpoch=FGuid::NewGuid();
    auto MakeTrace=[](FGuid Epoch,int32 Revision,const TCHAR* Name)
    {
        FDeliveryNpcDecisionTrace Value;
        Value.WorldEpoch=Epoch; Value.RequestId=FGuid::NewGuid(); Value.Revision=Revision;
        Value.NpcName=Name; Value.Stage=TEXT("Decision"); Value.ProposedAction=TEXT("Wait");
        Value.ExecutionResult=TEXT("等待实际执行结果");
        return Value;
    };
    // 只替代已生成的诊断载荷，经真实 Agent 委托、PC 组件和处理器传播，不启动模型
    auto FirstTrace=MakeTrace(FirstEpoch,5,TEXT("观察甲"));
    FirstNpc->Agent->OnDecisionTraceChanged.Broadcast(FirstTrace);
    TestEqual(TEXT("第一名玩家收到所订阅的真实通知链"),First->LatestTrace.RequestId,FirstTrace.RequestId);
    TestTrue(TEXT("第一名玩家的通知不会污染第二名玩家"),Second->History.IsEmpty() && !Second->LatestTrace.RequestId.IsValid());

    auto Rejected=MakeTrace(FirstEpoch,6,TEXT("迟到内容"));
    FirstSource->ClientNpcDiagnostics_Implementation(FirstGeneration-1,Rejected);
    TestEqual(TEXT("旧订阅代次不能更新当前记录"),First->LatestTrace.RequestId,FirstTrace.RequestId);
    Rejected.WorldEpoch=SecondEpoch;
    FirstSource->ClientNpcDiagnostics_Implementation(FirstGeneration,Rejected);
    TestEqual(TEXT("旧世界或其他世界的记录被拒绝"),First->LatestTrace.RequestId,FirstTrace.RequestId);
    Rejected.WorldEpoch=FirstEpoch; Rejected.Revision=FirstTrace.Revision;
    FirstSource->ClientNpcDiagnostics_Implementation(FirstGeneration,Rejected);
    Rejected.Revision=FirstTrace.Revision-1;
    FirstSource->ClientNpcDiagnostics_Implementation(FirstGeneration,Rejected);
    TestTrue(TEXT("重复或倒退版本不覆盖也不增加历史"),First->LatestTrace.RequestId==FirstTrace.RequestId && First->History.Num()==1);

    First->ObserveNpc(SecondNpc);
    const uint64 SecondGeneration=FirstSource->LocalInspectorGeneration;
    TestTrue(TEXT("换人物清空原历史并切换订阅代次"),SecondGeneration>FirstGeneration && First->History.IsEmpty());
    TestFalse(TEXT("旧 NPC 已解除第一名玩家的服务器订阅"),FirstNpc->Agent->OnDecisionTraceChanged.IsBoundToObject(FirstSource));
    FirstSource->ServerWatchNpcDiagnostics_Implementation(FirstNpc,FirstGeneration);
    TestTrue(TEXT("旧服务器订阅请求不能重新绑定旧 NPC"),FirstSource->ServerInspectorTarget.Get()==SecondNpc &&
        FirstSource->ServerInspectorGeneration==SecondGeneration);
    FirstSource->ClientNpcDiagnostics_Implementation(FirstGeneration,MakeTrace(FirstEpoch,100,TEXT("旧人物")));
    FirstNpc->Agent->OnDecisionTraceChanged.Broadcast(MakeTrace(FirstEpoch,101,TEXT("旧人物")));
    TestTrue(TEXT("换目标后旧回包和旧 NPC 新通知都不能带回历史"),First->History.IsEmpty());

    FDeliveryNpcDecisionTrace Latest;
    TArray<FGuid> Requests;
    for (int32 Index=1;Index<=10;++Index)
    {
        Latest=MakeTrace(SecondEpoch,Index,TEXT("观察乙")); Requests.Add(Latest.RequestId);
        SecondNpc->Agent->OnDecisionTraceChanged.Broadcast(Latest);
    }
    if (!TestEqual(TEXT("本机最多保存八轮历史"),First->History.Num(),8) ||
        !TestEqual(TEXT("另一玩家独立维护自己的八轮历史"),Second->History.Num(),8)) return false;
    TestEqual(TEXT("最新轮位于历史开头"),First->History[0].RequestId,Requests[9]);
    TestEqual(TEXT("历史确实移除了最早两轮"),First->History.Last().RequestId,Requests[2]);
    FirstWidget->OnHistorySelected.Broadcast(3);
    TestTrue(TEXT("历史选择事件使当前玩家进入历史查看"),First->bShowingHistory);
    TestFalse(TEXT("历史选择不改变另一玩家的查看状态"),Second->bShowingHistory);
    Latest.Revision=11; Latest.ExecutionResult=TEXT("动作已结束");
    SecondNpc->Agent->OnDecisionTraceChanged.Broadcast(Latest);
    TestTrue(TEXT("同轮新版本更新缓存但保留历史查看状态"),First->History.Num()==8 &&
        First->History[0].Revision==11 && First->LatestTrace.Revision==11 && First->bShowingHistory);
    FirstWidget->OnLatestRequested.Broadcast();
    TestFalse(TEXT("返回最新事件退出历史查看"),First->bShowingHistory);

    FirstPlayer->Possess(MakePawn(FVector(0,0,90)));
    First->ValidateContext();
    TestTrue(TEXT("换 Pawn 清空目标和历史"),!First->ObservedNpc.IsValid() && First->History.IsEmpty());
    TestTrue(TEXT("换 Pawn 解除服务器和本机订阅"),!FirstSource->LocalInspectorTarget.IsValid() && !FirstSource->ServerInspectorTarget.IsValid());
    FirstSource->ClientNpcDiagnostics_Implementation(SecondGeneration,Latest);
    TestTrue(TEXT("换 Pawn 后旧回包不能复活历史"),First->History.IsEmpty());
    TestEqual(TEXT("换 Pawn 不清理另一玩家的历史"),Second->History.Num(),8);

    First->ObserveNpc(SecondNpc);
    Latest.Revision=12;
    SecondNpc->Agent->OnDecisionTraceChanged.Broadcast(Latest);
    FirstWidget->OnHistorySelected.Broadcast(0);
    FirstPlayer->GetPawn()->SetActorLocation(FVector(8000,0,90));
    Latest.Revision=13;
    SecondNpc->Agent->OnDecisionTraceChanged.Broadcast(Latest);
    TestTrue(TEXT("超出观察范围立即清空目标、历史和历史查看状态"),!First->ObservedNpc.IsValid() &&
        First->History.IsEmpty() && !First->bShowingHistory);
    TestTrue(TEXT("不可用目标已解除服务器通知"),!FirstSource->ServerInspectorTarget.IsValid());
    TestEqual(TEXT("一名玩家离开不影响另一玩家更新"),Second->LatestTrace.Revision,13);

    SecondNpc->Destroy();
    Second->ValidateContext();
    TestTrue(TEXT("NPC 销毁清空本机历史与订阅"),Second->History.IsEmpty() &&
        !Second->ObservedNpc.IsValid() && !SecondSource->LocalInspectorTarget.IsValid());
    FirstPlayer->GetPawn()->SetActorLocation(FVector(0,0,90));
    First->ObserveNpc(FirstNpc);
    FirstNpc->Agent->OnDecisionTraceChanged.Broadcast(MakeTrace(FirstEpoch,102,TEXT("观察甲")));
    TestEqual(TEXT("目标销毁后可重新观察有效人物"),First->History.Num(),1);
    First->Shutdown(); First->Shutdown();
    TestTrue(TEXT("重复关闭清空处理器持有的世界与记录"),!First->bInitialized && !First->GetWorld() && First->History.IsEmpty());
    TestFalse(TEXT("关闭解除组件通知"),FirstSource->OnDiagnosticsChanged.IsBoundToObject(First));
    TestFalse(TEXT("关闭解除原 NPC 的服务器通知"),FirstNpc->Agent->OnDecisionTraceChanged.IsBoundToObject(FirstSource));
    TestFalse(TEXT("关闭解除界面操作委托"),FirstWidget->OnHistorySelected.IsBoundToObject(First));
    FirstNpc->Agent->OnDecisionTraceChanged.Broadcast(MakeTrace(FirstEpoch,103,TEXT("关闭后通知")));
    TestTrue(TEXT("关闭后的晚到通知不能恢复历史"),First->History.IsEmpty());
    FirstSource->ShutdownInspector(); FirstSource->ShutdownInspector();
    TestTrue(TEXT("组件观察清理可以重复执行"),!FirstSource->InspectorTraceHandle.IsValid() && !FirstSource->LocalInspectorTarget.IsValid());
    SecondPlayer->Destroy();
    Second->ValidateContext();
    TestTrue(TEXT("拥有者销毁后处理器退出并释放世界引用"),!Second->bInitialized && !Second->GetWorld() && Second->History.IsEmpty());
    return true;
}
#endif
