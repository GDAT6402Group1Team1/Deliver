#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Components/SceneComponent.h"
#include "Components/TextBlock.h"
#include "Components/EditableTextBox.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/Button.h"
#include "Components/Border.h"
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
    Panel->SubmittedLine=NewObject<UBorder>(Panel);
    Panel->SpeakerName=NewObject<UTextBlock>(Panel);
    Panel->Input=NewObject<UEditableTextBox>(Panel);
    Panel->Suggestions=NewObject<UMultiLineEditableTextBox>(Panel);
    Panel->Suggestions->SetIsReadOnly(true);
    Panel->SendButton=NewObject<UButton>(Panel);
    Panel->SuggestionToggle=NewObject<UButton>(Panel);
    Panel->NativeConstruct();
    Panel->SetResident(Npc);
    TestEqual(TEXT("还没发言时不占用原话行的空间"),Panel->SubmittedLine->GetVisibility(),ESlateVisibility::Collapsed);
    TestEqual(TEXT("输入提示明确当前交谈对象"),Panel->SpeakerName->GetText().ToString(),FString(TEXT("对 翠花 说")));
    TestFalse(TEXT("建议默认折叠"),Panel->IsSuggestionsVisible());
    Panel->SuggestionToggle->OnClicked.Broadcast();
    TestTrue(TEXT("展开建议不会发送消息"),Panel->IsSuggestionsVisible() && !Conversation->GetLastSubmissionId().IsValid());
    const FString SuggestedChallenge=Conversation->GetProvokeText(Npc);
    TestTrue(TEXT("建议文字包含现场双方和道歉例句"),Panel->Suggestions->GetText().ToString().Contains(SuggestedChallenge) &&
        Panel->Suggestions->GetText().ToString().Contains(TEXT("对不起")));
    Panel->Suggestions->OnTextCommitted.Broadcast(Panel->Suggestions->GetText(),ETextCommit::OnEnter);
    Panel->Input->SetText(FText::FromString(SuggestedChallenge));
    Panel->Input->OnTextCommitted.Broadcast(Panel->Input->GetText(),ETextCommit::OnUserMovedFocus);
    TestFalse(TEXT("阅读建议、在建议上回车和粘贴输入都不会发送消息"),Conversation->GetLastSubmissionId().IsValid());
    Panel->SendButton->OnClicked.Broadcast();
    const FGuid ProvokeId=Panel->SubmittedEventId;
    const FString Provoke=Panel->LastSubmittedText;
    TestTrue(TEXT("按发送后才通过交谈组件提交建议文字"),ProvokeId.IsValid() && ProvokeId==Conversation->GetLastSubmissionId());
    TestEqual(TEXT("拱火点名在场双方，由玩家明确发起比划挑战"),Provoke,FString(TEXT("翠花，大壮就在旁边。你俩到底谁厉害？敢不敢比划两下，输的当面认怂？")));
    TestTrue(TEXT("输入区保留刚才发送的完整原话"),Panel->Reply->GetText().ToString().Contains(Provoke));
    TestTrue(TEXT("发言后显示原话行，末尾不留空白段落"),Panel->SubmittedLine->GetVisibility()==ESlateVisibility::Visible &&
        !Panel->Reply->GetText().ToString().EndsWith(TEXT("\n")));
    TestTrue(TEXT("标题显示当前交谈对象"),Panel->SpeakerName->GetText().ToString().Contains(TEXT("翠花")));
    TestEqual(TEXT("服务器确实收到这次发言"),Conversation->GetSubmissionResult(ProvokeId),FName(TEXT("Heard")));
    TestTrue(TEXT("收到服务器确认后才显示你说"),Panel->Reply->GetText().ToString().StartsWith(TEXT("你说：")));
    TestFalse(TEXT("刚发送后进入统一冷却"),Conversation->CanSendNow());

    // 即使发送冷却已经结束，同一句仍在等回复时也不能重复发出
    Conversation->NextLocalSendAt=0;
    Conversation->NextSendAt=0;
    Panel->SendButton->OnClicked.Broadcast();
    TestTrue(TEXT("再次发送正在等待的同一句不会建立新请求"),Conversation->GetLastSubmissionId()==ProvokeId);
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
    TestTrue(TEXT("修改文字后可以提交新的一句"),Panel->SubmittedEventId!=ProvokeId);
    TestEqual(TEXT("发送太快的拒绝来自服务器"),Panel->SubmissionResult,FName(TEXT("TooFast")));
    TestTrue(TEXT("被拒绝的文字保留并显示没送到"),Panel->Reply->GetText().ToString().StartsWith(TEXT("没送到：我现在要送货")));
    TestTrue(TEXT("服务器拒绝时保留原话行，并分行说明原因"),Panel->SubmittedLine->GetVisibility()==ESlateVisibility::Visible &&
        Panel->Reply->GetText().ToString().Contains(TEXT("我现在要送货\n刚才发得太快")));
    TestFalse(TEXT("被拒绝后不继续等模型回复"),Panel->bAwaitingReply);

    Conversation->NextLocalSendAt=0;
    Conversation->NextSendAt=0;
    Panel->Input->SetText(FText::FromString(TEXT("对不起，刚才是我太急了，我先送货去了。")));
    Panel->Input->OnTextCommitted.Broadcast(Panel->Input->GetText(),ETextCommit::OnEnter);
    TestTrue(TEXT("在输入框回车发送玩家修改后的原话"),Panel->LastSubmittedText==TEXT("对不起，刚才是我太急了，我先送货去了。"));
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
    Panel->RefreshReply();
    TestEqual(TEXT("失败前已通过正式刷新接收首句"),Panel->LastReplyText,FString(TEXT("这事我听见了。")));
    TestTrue(TEXT("回复期间的新事实合并进后续请求"),Agent->RequestDecision());
    Agent->FinishReply(NewRequest.RequestId,false);
    TestTrue(TEXT("语音阶段失败重试时恢复原玩家注视"),Agent->IsListening() && Npc->IsDecisionRetryScheduled());
    // 请求失败后动作仍独立结束，测试不把停止动作当成模型已经回应
    Agent->TryFinishAction(Agent->GetPublicState().ActionId,EDeliveryNpcActionStatus::Cancelled,TEXT("TestRetry"));
    TestTrue(TEXT("重试期间旧动作结束仍保留原玩家回执"),Agent->bPlayerRequest &&
        Agent->PlayerReplyMessage==ApologyId && Agent->PlayerReplyController.Get()==Controller);
    NewRequest=BeginQueuedRequest();
    Agent->ReceiveDecision(NewRequest.RequestId,nullptr,TEXT("TransportFailure"));
    Panel->RefreshReply();
    TestTrue(TEXT("最终失败保留已收到的回复状态，下方只显示必要反馈"),!Panel->bAwaitingReply &&
        Panel->LastReplyText==TEXT("这事我听见了。") && !Panel->Reply->GetText().ToString().Contains(Panel->LastReplyText) &&
        Panel->Reply->GetText().ToString().Contains(TEXT("这次没能回复，可以再试一次")));

    // 用真实提交、动作开始和完成入口检查零台词回应，不访问模型或播放声音
    if (!Npc->GetController()) Npc->SpawnDefaultController();
    auto PrepareActionReply=[&](bool bStream,const FString& Speech)
    {
        Agent->CancelPlan(TEXT("NextActionReplyTest"));
        Settings->bStreamReplies=bStream;
        Panel->SetResident(Npc);
        Conversation->NextLocalSendAt=Conversation->NextSendAt=0;
        Panel->SendText(TEXT("你看我一下"));
        const auto Request=BeginQueuedRequest();
        const auto* Candidate=Request.Candidates.FindByPredicate([&](const auto& Item)
            { return Item.Action==EDeliveryNpcAction::LookAt && Agent->KnownSubjects.FindRef(Item.TargetId).Get()==Controller->GetPawn(); });
        if (!TestTrue(TEXT("使用实际生成的注视玩家候选"),Candidate!=nullptr)) return FGuid();
        FDeliveryNpcDecisionProposal Reply;
        Reply.RequestId=Request.RequestId; Reply.NpcId=Request.NpcId; Reply.WorldEpoch=Request.WorldEpoch;
        Reply.ContextRevision=Request.ContextRevision; Reply.PlanRevision=Request.PlanRevision;
        Reply.Action=Candidate->Action; Reply.TargetId=Candidate->TargetId; Reply.EvidenceIds=Candidate->EvidenceIds;
        Reply.Duration=1; Reply.Emotion=TEXT("Calm"); Reply.Speech=Speech;
        if (bStream)
        {
            if (!TestTrue(TEXT("接收逐句动作建议"),Agent->ReceiveStreamDecision(Request.RequestId,Reply))) return FGuid();
        }
        else Agent->ReceiveDecision(Request.RequestId,&Reply,NAME_None);
        Panel->RefreshReply();
        if (bStream && !Speech.IsEmpty())
            TestTrue(TEXT("完整首句通过检查后结束等待，下方不重复台词"),!Panel->bAwaitingReply &&
                Panel->LastReplyText==Speech && !Panel->Reply->GetText().ToString().Contains(Speech) &&
                !Npc->Presentation->GetDisplayedSpeech().bActive);
        else
            TestTrue(TEXT("零台词动作刚被接受不能结束界面等待"),Panel->bAwaitingReply &&
                Conversation->GetSubmissionResult(Panel->SubmittedEventId)==TEXT("Heard"));
        FDeliveryNpcPublicState Action;
        if (!TestTrue(TEXT("通过正式入口开始动作"),Agent->StartAction(Action))) return FGuid();
        return Request.RequestId;
    };

    FGuid ActionRequest=PrepareActionReply(true,FString());
    if (!ActionRequest.IsValid()) return false;
    const FGuid FirstActionMessage=Panel->SubmittedEventId;
    Agent->FinishReply(ActionRequest,true,0);
    Panel->RefreshReply();
    TestTrue(TEXT("确认零句后仍等待正在执行的动作"),Panel->bAwaitingReply &&
        Conversation->GetSubmissionResult(FirstActionMessage)==TEXT("Heard"));
    TestTrue(TEXT("模型结束后保留原玩家直到动作完成"),Agent->PlayerReplyController.Get()==Controller &&
        Agent->PlayerReplyMessage==FirstActionMessage);
    const FGuid FirstActionId=Agent->GetPublicState().ActionId;
    TestTrue(TEXT("零台词动作可以正常完成"),Agent->TryFinishAction(FirstActionId,EDeliveryNpcActionStatus::Succeeded,NAME_None));
    Panel->RefreshReply();
    TestTrue(TEXT("动作成功通知结束界面等待"),!Panel->bAwaitingReply &&
        Conversation->GetSubmissionResult(FirstActionMessage)==TEXT("ActionReply"));
    TestTrue(TEXT("动作回应结束等待，下方保留玩家原话"),Panel->Reply->GetText().ToString().Contains(TEXT("你说：你看我一下")) &&
        !Panel->Reply->GetText().ToString().Contains(TEXT("用动作回应")) && Panel->LastReplyText.IsEmpty());
    Panel->ReplyWaitUntil=0;
    Panel->RefreshReply();
    TestFalse(TEXT("已完成的动作回应不再出现三十秒超时提示"),Panel->bReplyTimedOut);
    TestFalse(TEXT("同一动作重复完成被拒绝"),Agent->TryFinishAction(FirstActionId,EDeliveryNpcActionStatus::Succeeded,NAME_None));
    TestTrue(TEXT("动作完成后释放原玩家回执引用"),!Agent->PlayerReplyMessage.IsValid() && !Agent->PlayerReplyController.IsValid());
    Conversation->NextLocalSendAt=Conversation->NextSendAt=0;
    Panel->SendText(TEXT("你看我一下"));
    TestTrue(TEXT("动作回应结束后同一句可以再次发送"),Panel->SubmittedEventId!=FirstActionMessage);

    ActionRequest=PrepareActionReply(true,FString());
    if (!ActionRequest.IsValid()) return false;
    Agent->TryFinishAction(Agent->GetPublicState().ActionId,EDeliveryNpcActionStatus::Succeeded,NAME_None);
    Panel->RefreshReply();
    TestTrue(TEXT("动作先成功时等待后端确认是否还有台词"),Panel->bAwaitingReply &&
        Conversation->GetSubmissionResult(Panel->SubmittedEventId)==TEXT("Heard"));
    Agent->FinishReply(ActionRequest,true,0);
    Panel->RefreshReply();
    TestTrue(TEXT("动作先完成后收到零句也能通知玩家"),!Panel->bAwaitingReply &&
        Conversation->GetSubmissionResult(Panel->SubmittedEventId)==TEXT("ActionReply"));

    for (const auto Result:{EDeliveryNpcActionStatus::Failed,EDeliveryNpcActionStatus::Cancelled})
    {
        ActionRequest=PrepareActionReply(true,FString());
        if (!ActionRequest.IsValid()) return false;
        Agent->FinishReply(ActionRequest,true,0);
        Agent->TryFinishAction(Agent->GetPublicState().ActionId,Result,TEXT("TestNotSucceeded"));
        Panel->RefreshReply();
        TestEqual(TEXT("失败或取消发送失败回执而非动作成功"),Conversation->GetSubmissionResult(Panel->SubmittedEventId),FName(TEXT("ReplyFailed")));
        TestFalse(TEXT("零台词动作失败后不再等三十秒"),Panel->bAwaitingReply);
        TestTrue(TEXT("失败或取消后释放等待动作完成的引用"),!Agent->PlayerReplyMessage.IsValid() && !Agent->PlayerReplyController.IsValid());
    }

    for (const auto Result:{EDeliveryNpcActionStatus::Failed,EDeliveryNpcActionStatus::Cancelled})
    {
        ActionRequest=PrepareActionReply(true,FString());
        if (!ActionRequest.IsValid()) return false;
        Agent->TryFinishAction(Agent->GetPublicState().ActionId,Result,TEXT("TestFailureBeforeDone"));
        TestEqual(TEXT("动作先失败时尚不能断定后端没有台词"),Conversation->GetSubmissionResult(Panel->SubmittedEventId),FName(TEXT("Heard")));
        // 模拟下一帧已停止对白，而后台的结束通知稍后才到达
        Npc->Presentation->TickComponent(.02f,LEVELTICK_All,nullptr);
        TestFalse(TEXT("失败动作的待播数据已先清除"),Npc->Presentation->HasPendingSpeech());
        Agent->FinishReply(ActionRequest,true,0);
        Panel->RefreshReply();
        TestTrue(TEXT("对白先清除再确认零句也结束界面等待"),!Panel->bAwaitingReply &&
            Conversation->GetSubmissionResult(Panel->SubmittedEventId)==TEXT("ReplyFailed"));
        Agent->FinishReply(ActionRequest,true,0);
        TestEqual(TEXT("重复结束通知不把失败改成成功"),Conversation->GetSubmissionResult(Panel->SubmittedEventId),FName(TEXT("ReplyFailed")));
    }

    ActionRequest=PrepareActionReply(true,TEXT("我看着呢"));
    if (!ActionRequest.IsValid()) return false;
    FDeliveryNpcSentence Spoken; Spoken.Sequence=0; Spoken.Text=TEXT("我看着呢"); Spoken.Emotion=TEXT("Calm");
    Agent->ReceiveSentence(ActionRequest,Spoken);
    Agent->TryFinishAction(Agent->GetPublicState().ActionId,EDeliveryNpcActionStatus::Succeeded,NAME_None);
    Agent->FinishReply(ActionRequest,true,1);
    Panel->RefreshReply();
    TestTrue(TEXT("有台词的回复先显示首句，不发送零台词动作回执"),!Panel->bAwaitingReply &&
        Panel->LastReplyText==TEXT("我看着呢") &&
        Conversation->GetSubmissionResult(Panel->SubmittedEventId)==TEXT("Heard"));

    const FString FirstText=TEXT("早啊，今天挺舒坦。"),SecondText=TEXT("你送货还顺当吧？");
    ActionRequest=PrepareActionReply(true,FirstText);
    if (!ActionRequest.IsValid()) return false;
    auto* Presentation=Npc->Presentation.Get();
    const auto ReplyState=Agent->GetPublicState();
    auto MakeDisplayedSentence=[&](int32 Sequence,const FString& Text)
    {
        FDeliveryNpcSpeechState Sentence;
        Sentence.TurnId=FGuid::NewGuid(); Sentence.ActionId=Agent->GetPublicState().ActionId;
        Sentence.ReplyToEventId=Panel->SubmittedEventId; Sentence.Sequence=Sequence;
        Sentence.Text=Text; Sentence.Emotion=TEXT("Calm"); Sentence.bActive=true;
        return Sentence;
    };
    // 这里只模拟客户端收到状态的先后，播放器和真实音频已由 AudioStreaming 检查
    const auto FirstDisplayed=MakeDisplayedSentence(0,FirstText);
    const auto SecondDisplayed=MakeDisplayedSentence(1,SecondText);
    Presentation->DisplayedSpeech=FirstDisplayed; Panel->RefreshReply(); Panel->RefreshReply();
    TestEqual(TEXT("提前显示的首句在开播时不再追加"),Panel->LastReplyText,FirstText);
    Presentation->DisplayedSpeech=SecondDisplayed; Panel->RefreshReply(); Panel->RefreshReply();
    TestEqual(TEXT("第二句开播后只追加一次"),Panel->LastReplyText,FirstText+SecondText);
    TestTrue(TEXT("人物开始说话后，下方仍不重复显示两句台词"),!Panel->Reply->GetText().ToString().Contains(FirstText) &&
        !Panel->Reply->GetText().ToString().Contains(SecondText));
    Presentation->DisplayedSpeech=FirstDisplayed; Panel->RefreshReply();
    TestTrue(TEXT("迟到的第零句不抹掉后句或退回旧编号"),Panel->LastReplyText==FirstText+SecondText &&
        Panel->DisplayedTurnId==SecondDisplayed.TurnId);
    Presentation->DisplayedSpeech=SecondDisplayed; Panel->RefreshReply();
    TestEqual(TEXT("旧首句之后再次刷新第二句也不重复"),Panel->LastReplyText,FirstText+SecondText);

    // 反过来，播放通知先到时也只显示一份首句
    Panel->LastReplyText.Reset(); Panel->DisplayedTurnId.Invalidate(); Panel->bAwaitingReply=true;
    Agent->PublicState={}; Presentation->DisplayedSpeech=FirstDisplayed; Panel->RefreshReply();
    TestTrue(TEXT("播放通知先到也能显示首句"),!Panel->bAwaitingReply && Panel->LastReplyText==FirstText);
    Agent->PublicState=ReplyState; Presentation->DisplayedSpeech={}; Panel->RefreshReply();
    TestEqual(TEXT("随后收到完整首句状态不重复追加"),Panel->LastReplyText,FirstText);
    Presentation->DisplayedSpeech=SecondDisplayed; Panel->RefreshReply();
    TestEqual(TEXT("播放先到的顺序也能接上第二句"),Panel->LastReplyText,FirstText+SecondText);

    auto ResetUnreadPanel=[&]
    {
        Panel->LastReplyText.Reset(); Panel->DisplayedTurnId.Invalidate();
        Panel->bAwaitingReply=true; Panel->bReplyTimedOut=false;
        Presentation->DisplayedSpeech={};
    };
    for (const auto Status:{EDeliveryNpcActionStatus::Accepted,EDeliveryNpcActionStatus::Running,EDeliveryNpcActionStatus::Succeeded})
    {
        ResetUnreadPanel(); Agent->PublicState=ReplyState; Agent->PublicState.Status=Status; Panel->RefreshReply();
        TestTrue(TEXT("本轮有效动作在语音准备期间均可先显示首句"),!Panel->bAwaitingReply && Panel->LastReplyText==FirstText);
    }
    for (const auto Status:{EDeliveryNpcActionStatus::Failed,EDeliveryNpcActionStatus::Cancelled})
    {
        ResetUnreadPanel(); Agent->PublicState=ReplyState; Agent->PublicState.Status=Status; Panel->RefreshReply();
        TestTrue(TEXT("失败或取消的动作不提前显示首句"),Panel->bAwaitingReply && Panel->LastReplyText.IsEmpty());
    }
    ResetUnreadPanel(); Agent->PublicState=ReplyState; Agent->PublicState.ReplyToEventId=FGuid::NewGuid(); Panel->RefreshReply();
    TestTrue(TEXT("其他发言的首句不能结束本次等待"),Panel->bAwaitingReply && Panel->LastReplyText.IsEmpty());
    Agent->PublicState=ReplyState; Agent->PublicState.ActionId=FGuid::NewGuid(); Panel->RefreshReply();
    TestTrue(TEXT("动作编号与本机待播回复不符时不提前显示"),Panel->bAwaitingReply && Panel->LastReplyText.IsEmpty());
    Agent->PublicState=ReplyState; Agent->PublicState.Speech.Reset();
    Neighbor->Agent->PublicState=ReplyState;
    Neighbor->Presentation->BeginReply(ReplyState.ActionId,Panel->SubmittedEventId,Neighbor->Agent->GetActionConversationDepth()); Panel->RefreshReply();
    TestTrue(TEXT("相同发言编号也不能读取另一NPC的首句"),Panel->bAwaitingReply && Panel->LastReplyText.IsEmpty());
    Neighbor->Agent->CancelPlan(TEXT("EndOtherNpcPreviewTest"));
    Agent->PublicState=ReplyState;
    Presentation->StopStreamSpeech_Implementation(ReplyState.ActionId,true); Panel->RefreshReply();
    TestTrue(TEXT("停止通知先到时不从晚到的动作状态显示旧首句"),Panel->bAwaitingReply && Panel->LastReplyText.IsEmpty());

    ActionRequest=PrepareActionReply(false,FString());
    if (!ActionRequest.IsValid()) return false;
    Agent->TryFinishAction(Agent->GetPublicState().ActionId,EDeliveryNpcActionStatus::Succeeded,NAME_None);
    Panel->RefreshReply();
    TestTrue(TEXT("旧整段结果的零台词动作也会结束等待"),!Panel->bAwaitingReply &&
        Conversation->GetSubmissionResult(Panel->SubmittedEventId)==TEXT("ActionReply"));

    ActionRequest=PrepareActionReply(false,FString());
    if (!ActionRequest.IsValid()) return false;
    const FGuid OldMessage=Panel->SubmittedEventId,OldAction=Agent->GetPublicState().ActionId;
    Conversation->NextLocalSendAt=Conversation->NextSendAt=0;
    Panel->SendText(TEXT("先别管刚才的，你听我说"));
    const FGuid NewMessage=Panel->SubmittedEventId;
    Agent->TryFinishAction(OldAction,EDeliveryNpcActionStatus::Succeeded,NAME_None);
    Conversation->ClientResult_Implementation(TEXT("ActionReply"),OldMessage);
    Panel->RefreshReply();
    TestTrue(TEXT("旧动作和迟到回执不能结束新发言"),NewMessage!=OldMessage && Panel->bAwaitingReply &&
        Conversation->GetSubmissionResult(NewMessage)==TEXT("Heard") && Agent->PlayerReplyMessage==NewMessage);

    ActionRequest=PrepareActionReply(true,FString());
    if (!ActionRequest.IsValid()) return false;
    const FGuid PossessionMessage=Panel->SubmittedEventId;
    Agent->FinishReply(ActionRequest,true,0);
    auto* NewPawn=World->SpawnActor<APawn>();
    auto* NewRoot=NewObject<USceneComponent>(NewPawn); NewPawn->SetRootComponent(NewRoot); NewRoot->RegisterComponent();
    NewPawn->SetActorLocation(Pawn->GetActorLocation());
    Controller->Possess(NewPawn);
    auto* OtherController=World->SpawnActor<APlayerController>();
    OtherController->Possess(Pawn);
    auto* OtherConversation=NewObject<UDeliveryConversationComponent>(OtherController); OtherConversation->RegisterComponent();
    // 给另一控制器相同编号，确认成功回执确实发给原玩家，而不只是被编号过滤掉
    OtherConversation->LastSubmissionId=PossessionMessage; OtherConversation->LastResult=TEXT("Heard");
    Agent->TryFinishAction(Agent->GetPublicState().ActionId,EDeliveryNpcActionStatus::Succeeded,NAME_None);
    Panel->RefreshReply();
    TestTrue(TEXT("换Pawn后原玩家仍收到动作回应"),!Panel->bAwaitingReply &&
        Conversation->GetSubmissionResult(PossessionMessage)==TEXT("ActionReply"));
    TestEqual(TEXT("后来占有原Pawn的人不会收到这次回应"),OtherConversation->GetSubmissionResult(PossessionMessage),FName(TEXT("Heard")));
    Panel->SetResident(Neighbor);
    TestEqual(TEXT("更换对象后收起旧原话行"),Panel->SubmittedLine->GetVisibility(),ESlateVisibility::Collapsed);
    TestTrue(TEXT("切换对象后不保留上一个人的发送编号"),!Panel->SubmittedEventId.IsValid());
    TestTrue(TEXT("切换对象后不显示旧原话"),Panel->LastSubmittedText.IsEmpty());
    TestTrue(TEXT("新标题使用新交谈对象"),Panel->SpeakerName->GetText().ToString().Contains(TEXT("大壮")));
    TestFalse(TEXT("换交谈对象后重新折叠建议"),Panel->IsSuggestionsVisible());
    Panel->NativeDestruct();
    TestEqual(TEXT("关闭后发送按钮解除绑定"),Panel->SendButton->OnClicked.GetAllObjects().Num(),0);
    TestEqual(TEXT("关闭后输入框回车解除绑定"),Panel->Input->OnTextCommitted.GetAllObjects().Num(),0);
    TestTrue(TEXT("关闭后清空当前交谈内容与对象"),!Panel->Resident.IsValid() && Panel->LastSubmittedText.IsEmpty() &&
        Panel->LastReplyText.IsEmpty() && Panel->Suggestions->GetText().IsEmpty());
    return true;
}
#endif
