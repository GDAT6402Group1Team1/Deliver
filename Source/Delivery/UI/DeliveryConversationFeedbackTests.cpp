#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Components/SceneComponent.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "UI/DeliveryConversationWidget.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryConversationFeedbackTest,"Delivery.AI.ConversationFeedback",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryConversationFeedbackTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool bWasEnabled=Settings->bEnabled;
    const bool bWasStreaming=Settings->bStreamReplies;
    Settings->bEnabled=false;
    Settings->bStreamReplies=true;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit);
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        Settings->bEnabled=bWasEnabled;
        Settings->bStreamReplies=bWasStreaming;
    };

    auto* Pawn=World->SpawnActor<APawn>();
    auto* Root=NewObject<USceneComponent>(Pawn);
    Pawn->SetRootComponent(Root);
    Root->RegisterComponent();
    Pawn->SetActorLocation(FVector(0,0,90));
    auto* Controller=World->SpawnActor<APlayerController>();
    Controller->Player=NewObject<ULocalPlayer>(GEngine);
    Controller->PlayerState=World->SpawnActor<APlayerState>();
    Controller->PlayerState->SetIsABot(false);
    Controller->Possess(Pawn);
    auto* Conversation=NewObject<UDeliveryConversationComponent>(Controller);
    Conversation->RegisterComponent();
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Npc=World->SpawnActor<ADeliveryNpcCharacter>(FVector(200,0,90),FRotator::ZeroRotator,Spawn);
    auto* Neighbor=World->SpawnActor<ADeliveryNpcCharacter>(FVector(200,160,90),FRotator::ZeroRotator,Spawn);
    Npc->SetActorTickEnabled(false);
    Neighbor->SetActorTickEnabled(false);
    Npc->DisplayName=FText::FromString(TEXT("翠花"));
    Neighbor->DisplayName=FText::FromString(TEXT("大壮"));

    // 使用实际发送接口和窗口刷新，模型保持关闭，不播放语音
    auto* Panel=CreateWidget<UDeliveryConversationWidget>(Controller);
    if (!TestNotNull(TEXT("创建交谈窗口"),Panel)) return false;
    Panel->Reply=NewObject<UTextBlock>(Panel);
    Panel->SpeakerName=NewObject<UTextBlock>(Panel);
    Panel->SetResident(Npc);
    Panel->Provoke();
    const FGuid ProvokeId=Panel->SubmittedEventId;
    const FString Provoke=Panel->LastSubmittedText;
    TestTrue(TEXT("拱火确实通过交谈组件发送"),ProvokeId.IsValid() && ProvokeId==Conversation->GetLastSubmissionId());
    TestTrue(TEXT("拱火点名双方，玩家直接发起挑战"),Provoke.StartsWith(TEXT("翠花，你敢不敢跟大壮比划两下")));
    TestTrue(TEXT("聊天框显示刚才发送的完整原话"),Panel->Reply->GetText().ToString().Contains(Provoke));
    TestTrue(TEXT("标题显示当前交谈对象"),Panel->SpeakerName->GetText().ToString().Contains(TEXT("翠花")));
    TestEqual(TEXT("服务器确实收到这次发言"),Conversation->GetSubmissionResult(ProvokeId),FName(TEXT("Heard")));
    TestTrue(TEXT("收到服务器确认后才显示你说"),Panel->Reply->GetText().ToString().StartsWith(TEXT("你说：")));
    TestFalse(TEXT("刚发送后进入统一冷却"),Conversation->CanSendNow());

    // 即使发送冷却已经结束，同一句仍在等回复时也不能重复发出
    Conversation->NextLocalSendAt=0;
    Conversation->NextSendAt=0;
    Panel->Provoke();
    TestTrue(TEXT("再次点同一句拱火没有建立新请求"),Conversation->GetLastSubmissionId()==ProvokeId);
    Conversation->ResultExpiresAt=0;
    Panel->RefreshReply();
    TestEqual(TEXT("短提示过期不会丢失本次发送结果"),Panel->SubmissionResult,FName(TEXT("Heard")));
    Conversation->ClientResult_Implementation(TEXT("Unavailable"),FGuid::NewGuid());
    TestEqual(TEXT("其他发言的延迟回执不会覆盖当前结果"),Conversation->GetSubmissionResult(ProvokeId),FName(TEXT("Heard")));

    // 请求只入本地队列就移除，再用真实失败入口检查重试和界面，不访问后台
    auto* Agent=Npc->Agent.Get();
    auto BeginQueuedRequest=[&]()
    {
        Settings->bEnabled=true;
        const bool bQueued=Agent->RequestDecision();
        Settings->bEnabled=false;
        TestTrue(TEXT("失败检查的请求已经入队"),bQueued);
        const auto Request=Agent->ActiveRequest;
        World->GetSubsystem<UDeliveryAiGatewaySubsystem>()->Cancel(Agent);
        return Request;
    };
    FGuid LastFailedRequest;
    for (int32 Attempt=0;Attempt<3;++Attempt)
    {
        LastFailedRequest=BeginQueuedRequest().RequestId;
        TestTrue(TEXT("请求期间的新事实可以合并"),Agent->RequestDecision());
        Settings->bEnabled=true;
        Agent->ReceiveDecision(LastFailedRequest,nullptr,TEXT("TransportFailure"));
        Settings->bEnabled=false;
        Panel->RefreshReply();
        if (Attempt<2)
        {
            TestTrue(TEXT("重试期间保留玩家和等待状态"),Agent->IsListening() && Agent->bPlayerRequest &&
                Npc->IsDecisionRetryScheduled() && Panel->bAwaitingReply);
            TestEqual(TEXT("重试不提前报告失败"),Conversation->GetSubmissionResult(ProvokeId),FName(TEXT("Heard")));
        }
    }
    TestTrue(TEXT("重试用尽立即结束倾听和玩家等待"),!Agent->IsListening() && !Agent->bPlayerRequest &&
        !Agent->IsDecisionPending() && !Npc->IsDecisionRetryScheduled() && !Panel->bAwaitingReply);
    TestTrue(TEXT("失败提示保留已送达的玩家原话"),Panel->Reply->GetText().ToString().StartsWith(TEXT("你说：")) &&
        Panel->Reply->GetText().ToString().Contains(TEXT("这次没能回复，可以再试一次")));

    // 服务器拒绝下一句时保留玩家原话，同时明确标成没有送到
    Conversation->NextLocalSendAt=0;
    Conversation->NextSendAt=FPlatformTime::Seconds()+30;
    Panel->SendText(TEXT("我现在要送货"));
    TestTrue(TEXT("普通输入和快捷按钮使用同一发送入口"),Panel->SubmittedEventId!=ProvokeId);
    TestEqual(TEXT("发送太快的拒绝来自服务器"),Panel->SubmissionResult,FName(TEXT("TooFast")));
    TestTrue(TEXT("被拒绝的文字保留并显示没送到"),Panel->Reply->GetText().ToString().StartsWith(TEXT("没送到：我现在要送货")));
    TestFalse(TEXT("被拒绝后不继续等模型回复"),Panel->bAwaitingReply);

    Conversation->NextLocalSendAt=0;
    Conversation->NextSendAt=0;
    Panel->Apologize();
    TestTrue(TEXT("道歉也显示按钮实际发送的原话"),Panel->Reply->GetText().ToString().Contains(TEXT("对不起，刚才是我太急了")));
    const FGuid ApologyId=Panel->SubmittedEventId;
    Agent->ReceiveDecision(LastFailedRequest,nullptr,TEXT("TransportFailure"));
    Conversation->ClientResult_Implementation(TEXT("ReplyFailed"),ProvokeId);
    Panel->RefreshReply();
    TestTrue(TEXT("旧请求和旧失败回执不能结束新发言"),Agent->IsListening() && Panel->bAwaitingReply &&
        Conversation->GetSubmissionResult(ApologyId)==TEXT("Heard"));
    auto NewRequest=BeginQueuedRequest();
    Agent->ReceiveDecision(NewRequest.RequestId,nullptr,TEXT("TransportFailure"));
    TestTrue(TEXT("新的玩家发言重新获得重试机会"),Npc->IsDecisionRetryScheduled() && Agent->IsListening());
    const double RetryAt=Npc->NextDecision;
    Npc->WakeDecision();
    TestTrue(TEXT("旁人插话保留原定重试时间和次数"),Npc->NextDecision==RetryAt &&
        Npc->DecisionRetryCount==1 && Npc->IsDecisionRetryScheduled());

    // 模型已选动作后，倾听对象已经清空，后续连接失败仍应对应原玩家发言
    NewRequest=BeginQueuedRequest();
    FDeliveryNpcDecisionProposal Proposal;
    Proposal.RequestId=NewRequest.RequestId; Proposal.NpcId=NewRequest.NpcId; Proposal.WorldEpoch=NewRequest.WorldEpoch;
    Proposal.ContextRevision=NewRequest.ContextRevision; Proposal.PlanRevision=NewRequest.PlanRevision;
    Proposal.Action=EDeliveryNpcAction::Wait; Proposal.Duration=1; Proposal.Emotion=TEXT("Calm"); Proposal.Speech=TEXT("这事我听见了。");
    TestTrue(TEXT("接收模型动作后进入语音阶段"),Agent->ReceiveStreamDecision(NewRequest.RequestId,Proposal));
    TestFalse(TEXT("语音阶段不继续报告倾听"),Agent->IsListening());
    TestTrue(TEXT("回复期间的新事实合并进后续请求"),Agent->RequestDecision());
    Agent->FinishReply(NewRequest.RequestId,false);
    TestTrue(TEXT("语音阶段失败重试时恢复原玩家注视"),Agent->IsListening() && Npc->IsDecisionRetryScheduled());
    // 请求失败后动作仍独立结束，测试不把停止动作当成模型已经回应
    Agent->TryFinishAction(Agent->GetPublicState().ActionId,EDeliveryNpcActionStatus::Cancelled,TEXT("TestRetry"));
    NewRequest=BeginQueuedRequest();
    Panel->LastReplyText=TEXT("这事我听见了。");
    Agent->ReceiveDecision(NewRequest.RequestId,nullptr,TEXT("TransportFailure"));
    Panel->RefreshReply();
    TestTrue(TEXT("最终失败保留此前已显示的回复"),!Panel->bAwaitingReply &&
        Panel->Reply->GetText().ToString().Contains(TEXT("这事我听见了。")) &&
        Panel->Reply->GetText().ToString().Contains(TEXT("这次没能回复，可以再试一次")));
    Panel->SetResident(Neighbor);
    TestTrue(TEXT("切换对象后不保留上一个人的发送编号"),!Panel->SubmittedEventId.IsValid());
    TestTrue(TEXT("切换对象后不显示旧原话"),Panel->LastSubmittedText.IsEmpty());
    TestTrue(TEXT("新标题使用新交谈对象"),Panel->SpeakerName->GetText().ToString().Contains(TEXT("大壮")));
    return true;
}
#endif
