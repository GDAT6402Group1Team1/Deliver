#include "Misc/AutomationTest.h"
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcPlayerInterruptionTest,"Delivery.AI.PlayerInterruption",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcPlayerInterruptionTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool Enabled=Settings->bEnabled,Stream=Settings->bStreamReplies;
    Settings->bEnabled=false; Settings->bStreamReplies=true;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
        Settings->bEnabled=Enabled; Settings->bStreamReplies=Stream;
    };
    FActorSpawnParameters Spawn; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Npc=World->SpawnActor<ADeliveryNpcCharacter>(FVector(0,0,90),FRotator::ZeroRotator,Spawn);
    auto* Target=World->SpawnActor<ADeliveryNpcCharacter>(FVector(300,0,90),FRotator::ZeroRotator,Spawn);
    auto* Player=World->SpawnActor<APlayerController>();
    auto* Other=World->SpawnActor<APlayerController>();
    Player->PlayerState=World->SpawnActor<APlayerState>();
    Other->PlayerState=World->SpawnActor<APlayerState>();
    auto* Pawn=World->SpawnActor<APawn>(FVector(100,0,90),FRotator::ZeroRotator,Spawn);
    Player->Possess(Pawn);
    Npc->SetActorTickEnabled(false); Target->SetActorTickEnabled(false);
    auto* Agent=Npc->Agent.Get(); auto* View=Npc->Presentation.Get();
    Agent->PublicState.ActionId=FGuid::NewGuid(); Agent->PublicState.PlanRevision=42;
    Agent->PublicState.Action=EDeliveryNpcAction::Fight; Agent->PublicState.Status=EDeliveryNpcActionStatus::Running;
    Agent->PublicState.Target=Target; Agent->PublicState.Requester=Other->PlayerState;
    Agent->PublicState.ServerStartTime=5; Agent->PublicState.Duration=8;
    Agent->ActionDeadline=World->GetTimeSeconds()+20; Agent->ActionEndsAt=World->GetTimeSeconds()+15;
    Agent->CombatDeadline=World->GetTimeSeconds()+20;
    const auto Before=Agent->GetPublicState();
    const double Deadline=Agent->ActionDeadline,EndsAt=Agent->ActionEndsAt,CombatDeadline=Agent->CombatDeadline;
    auto Unchanged=[&]()
    {
        const auto Current=Agent->GetPublicState();
        TestEqual(TEXT("活动编号不变"),Current.ActionId,Before.ActionId);
        TestEqual(TEXT("计划代次不变"),Current.PlanRevision,Before.PlanRevision);
        TestEqual(TEXT("活动类型不变"),Current.Action,Before.Action);
        TestEqual(TEXT("活动目标不变"),Current.Target.Get(),Before.Target.Get());
        TestEqual(TEXT("活动所有者不变"),Current.Requester.Get(),Before.Requester.Get());
        TestEqual(TEXT("开始时间不变"),Current.ServerStartTime,Before.ServerStartTime);
        TestEqual(TEXT("时长不变"),Current.Duration,Before.Duration);
        TestEqual(TEXT("执行时限不变"),Agent->ActionDeadline,Deadline);
        TestEqual(TEXT("活动结束时间不变"),Agent->ActionEndsAt,EndsAt);
        TestEqual(TEXT("连续交战上限不变"),Agent->CombatDeadline,CombatDeadline);
    };
    TArray<FGuid> Ended;
    const FDelegateHandle ReplyEndedHandle=Agent->OnPlayerReplyEnded.AddLambda([&](APlayerController* PC,FGuid Message,FName)
    { if (PC==Player) Ended.Add(Message); });
    ON_SCOPE_EXIT { Agent->OnPlayerReplyEnded.Remove(ReplyEndedHandle); };
    auto AddReply=[&](APlayerController* PC)
    {
        const FGuid Request=FGuid::NewGuid();
        auto& Reply=Agent->Replies.Add(Request);
        Reply.ReplyId=FGuid::NewGuid(); Reply.MessageId=FGuid::NewGuid(); Reply.ActionId=Before.ActionId;
        Reply.RequesterPC=PC; Reply.RequesterPlayerState=PC->PlayerState;
        // 模型已经完成，语音仍未完成；所有权不能依赖 ActiveRequest 或 SpeechTarget
        Reply.bModelFinished=true; Reply.SentenceCount=1;
        Reply.Listener=Target; Reply.Action=Before.Action; Reply.Target=Target;
        return Reply;
    };
    const auto First=AddReply(Player),Second=AddReply(Player),Foreign=AddReply(Other);
    View->BeginReply(First.ReplyId,First.MessageId,0,Target);
    View->BeginReply(Second.ReplyId,Second.MessageId,0,Target);
    FDeliveryNpcLocalVoice OtherTail;
    OtherTail.State.ActionId=Foreign.ReplyId; OtherTail.State.TurnId=FGuid::NewGuid(); OtherTail.State.bActive=true;
    View->LocalVoices.Add(OtherTail);
    FDeliveryNpcObservation Said;
    Said.Kind=TEXT("SelfSpeech"); Said.Description=TEXT("已经完整说过的话"); Said.Subject=Npc; Said.Lifetime=60;
    Agent->ObserveEvent(Said);
    Agent->InterruptPlayerReply(Player);
    Unchanged();
    TestFalse(TEXT("本玩家模型结束后的旧回复也被取消"),Agent->IsPlayerReplyActive(Player));
    TestTrue(TEXT("另一玩家的回复仍存在"),Agent->IsPlayerReplyActive(Other));
    TestFalse(TEXT("同批待播回复不会在取消当前句时被提升"),View->ReplyAction.IsValid() || View->PendingReply.ReplyId.IsValid());
    TestTrue(TEXT("另一玩家本机尾音不被 stop-all 清掉"),View->LocalVoices.ContainsByPredicate([&](const auto& Item){return Item.State.ActionId==Foreign.ReplyId;}));
    TestTrue(TEXT("已播记忆仍保留"),Agent->Evidence.ContainsByPredicate([](const auto& Item){return Item.Description==TEXT("已经完整说过的话");}));
    TestEqual(TEXT("两份旧回复各结束一次"),Ended.Num(),2);
    Agent->InterruptPlayerReply(Player);
    TestEqual(TEXT("重复取消不重复发结束事件"),Ended.Num(),2);
    TestFalse(TEXT("不能覆盖另一玩家未结束的回复"),Agent->BeginListening(Pawn,FGuid::NewGuid()));
    Agent->InterruptPlayerReply(Other);
    const FGuid Message=FGuid::NewGuid();
    TestTrue(TEXT("玩家插话进入倾听"),Agent->BeginListening(Pawn,Message));
    TestFalse(TEXT("有活动时不进入停止移动的 Listen 分支"),Agent->ShouldPauseForConversation());
    Unchanged();

    FDeliveryNpcObservation Speech;
    Speech.EventId=Message; Speech.Kind=TEXT("PlayerSpeech"); Speech.Subject=Pawn; Speech.Addressee=Npc;
    Speech.Description=TEXT("你继续忙，我就问一句"); Speech.Lifetime=45;
    Agent->ObserveEvent(Speech);
    FDeliveryNpcDecisionRequest Request;
    if (!TestTrue(TEXT("活动中仍能构建玩家请求"),Agent->BuildDecisionRequest(FGuid::NewGuid(),Request))) return false;
    TestEqual(TEXT("玩家使用 v6 能力"),Request.RequestedCapabilityVersion,6);
    const auto* Candidate=Request.Candidates.FindByPredicate([](const auto& Item){return Item.Action==EDeliveryNpcAction::ContinueCurrent;});
    if (!TestNotNull(TEXT("服务器提供明确的 ContinueCurrent 候选"),Candidate)) return false;
    FDeliveryNpcDecisionProposal Proposal;
    Proposal.CapabilityVersion=6; Proposal.CandidateId=Candidate->CandidateId;
    Proposal.WorldEpoch=Request.WorldEpoch; Proposal.NpcId=Request.NpcId; Proposal.RequestId=Request.RequestId;
    Proposal.ContextRevision=Request.ContextRevision; Proposal.PlanRevision=Request.PlanRevision;
    Proposal.Action=EDeliveryNpcAction::ContinueCurrent; Proposal.Duration=0; Proposal.Speech=TEXT("我接着忙，你说吧。");
    Agent->ActiveRequest=Request; Agent->bDecisionPending=true;
    FName Reason;
    auto Bad=Proposal; Bad.CandidateId=FGuid::NewGuid();
    TestFalse(TEXT("不能用不存在的候选请求继续"),Agent->TryAcceptProposal(Bad,Reason));
    Bad=Proposal; Bad.Duration=.1f;
    TestFalse(TEXT("继续不能续期"),Agent->TryAcceptProposal(Bad,Reason));
    Bad=Proposal; Bad.Speech.Reset();
    TestFalse(TEXT("面对玩家的 Continue 必须有真实回复文字"),Agent->TryAcceptProposal(Bad,Reason));
    TestTrue(TEXT("精确的 Continue 只采纳本轮回复"),Agent->ReceiveStreamDecision(Request.RequestId,Proposal));
    Unchanged();
    TestTrue(TEXT("模型完成通知被接受"),Agent->FinishModelReply(Request.RequestId,1));
    TestTrue(TEXT("模型完成不能提前结束玩家语音轮次"),Agent->IsPlayerReplyActive(Player));
    FDeliveryNpcSentence Sentence; Sentence.Sequence=0; Sentence.Text=Proposal.Speech;
    Agent->ReceiveSentence(Request.RequestId,Sentence);
    Agent->FinishReply(Request.RequestId,true,1);
    View->TickSentences();
    TestTrue(TEXT("Continue 回复可以播放而无需重启活动"),View->Speech.bActive);
    View->Speech.StartTime=View->ServerTime()-View->Speech.Duration-1;
    View->TickSentences();
    TestFalse(TEXT("实际整段播放结束才结束玩家回复"),Agent->IsPlayerReplyActive(Player));
    Unchanged();
    Agent->ActiveRequest=Request;
    Agent->PublicState.Status=EDeliveryNpcActionStatus::Succeeded;
    TestFalse(TEXT("活动已结束时拒绝迟到 Continue，不重新执行"),Agent->TryAcceptProposal(Proposal,Reason));
    TestEqual(TEXT("已结束的状态保持结束"),Agent->PublicState.Status,EDeliveryNpcActionStatus::Succeeded);
    Agent->ActiveRequest={}; Agent->PublicState.Status=EDeliveryNpcActionStatus::Running;
    TestTrue(TEXT("下一轮仍可开始倾听"),Agent->BeginListening(Pawn,FGuid::NewGuid()));
    Agent->ListeningUntil=FPlatformTime::Seconds()-1;
    Agent->TickComponent(.1f,LEVELTICK_All,nullptr);
    TestFalse(TEXT("超时结束倾听"),Agent->IsListening());
    Unchanged();

    // 玩家在旧动作快结束时开口，旧快照应失效，但这句话仍要得到回复
    auto PreparePendingPlayer=[&](FDeliveryNpcDecisionRequest& Pending)
    {
        Agent->CancelPlan(TEXT("NextActivityReplyTest"));
        Agent->PublicState={};
        Agent->PublicState.ActionId=FGuid::NewGuid();
        Agent->PublicState.Action=EDeliveryNpcAction::LookAt;
        Agent->PublicState.Status=EDeliveryNpcActionStatus::Running;
        Agent->PublicState.Target=Target;
        Agent->PublicState.ServerStartTime=World->GetTimeSeconds();
        Agent->PublicState.Duration=2;
        Agent->ActionEndsAt=World->GetTimeSeconds()+2;
        Agent->ActionDeadline=World->GetTimeSeconds()+3;
        const FGuid NewMessage=FGuid::NewGuid();
        if (!Agent->BeginListening(Pawn,NewMessage)) return false;
        FDeliveryNpcObservation NewSpeech;
        NewSpeech.EventId=NewMessage; NewSpeech.Kind=TEXT("PlayerSpeech");
        NewSpeech.Subject=Pawn; NewSpeech.Addressee=Npc; NewSpeech.Lifetime=45;
        NewSpeech.Description=TEXT("我刚才说的话，你听到了吗？");
        if (!Agent->ObserveEvent(NewSpeech) || !Agent->BuildDecisionRequest(FGuid::NewGuid(),Pending)) return false;
        Agent->ActiveRequest=Pending; Agent->bDecisionPending=true;
        return true;
    };
    FDeliveryNpcDecisionRequest Pending;
    if (!TestTrue(TEXT("旧动作中收到新的玩家发言"),PreparePendingPlayer(Pending))) return false;
    const int32 EndedBeforeActivity=Ended.Num();
    const FGuid FinishedAction=Agent->PublicState.ActionId;
    TestTrue(TEXT("旧动作正常完成"),Agent->TryFinishAction(FinishedAction,EDeliveryNpcActionStatus::Succeeded,NAME_None));
    TestFalse(TEXT("旧现场请求已经停止"),Agent->IsDecisionPending());
    TestTrue(TEXT("玩家这句话仍然等着回答"),Agent->IsWaitingToReplyToPlayer());
    TestEqual(TEXT("重发保留玩家原发言编号"),Agent->PlayerReplyMessage,Pending.PlayerMessageId);
    TestEqual(TEXT("重发保留原玩家控制器"),Agent->PlayerReplyController.Get(),Player);
    TestEqual(TEXT("正常完成不会通知玩家回复结束"),Ended.Num(),EndedBeforeActivity);
    TestFalse(TEXT("重复完成不会再次安排重发"),Agent->TryFinishAction(FinishedAction,EDeliveryNpcActionStatus::Succeeded,NAME_None));
    Agent->ReceiveDecision(Pending.RequestId,&Proposal,NAME_None);
    TestEqual(TEXT("旧请求迟到不能恢复已经结束的动作"),Agent->PublicState.Status,EDeliveryNpcActionStatus::Succeeded);

    FDeliveryNpcDecisionRequest Refreshed;
    if (!TestTrue(TEXT("按新现场重新构建同一句发言"),Agent->BuildDecisionRequest(Pending.WorldEpoch,Refreshed))) return false;
    TestNotEqual(TEXT("重新请求使用新编号"),Refreshed.RequestId,Pending.RequestId);
    TestEqual(TEXT("更新现场不改变玩家原话编号"),Refreshed.PlayerMessageId,Pending.PlayerMessageId);
    TestFalse(TEXT("新请求不再绑定旧动作"),Refreshed.RunningActionId.IsValid());
    TestFalse(TEXT("新请求不再提供继续旧动作"),Refreshed.Candidates.ContainsByPredicate(
        [](const auto& Item){return Item.Action==EDeliveryNpcAction::ContinueCurrent;}));
    const auto* Wait=Refreshed.Candidates.FindByPredicate([](const auto& Item){return Item.Action==EDeliveryNpcAction::Wait;});
    if (!TestNotNull(TEXT("新现场仍有有效回复候选"),Wait)) return false;
    FDeliveryNpcDecisionProposal Reply;
    Reply.CapabilityVersion=Refreshed.RequestedCapabilityVersion; Reply.CandidateId=Wait->CandidateId;
    Reply.WorldEpoch=Refreshed.WorldEpoch; Reply.NpcId=Refreshed.NpcId; Reply.RequestId=Refreshed.RequestId;
    Reply.ContextRevision=Refreshed.ContextRevision; Reply.PlanRevision=Refreshed.PlanRevision;
    Reply.Action=Wait->Action; Reply.Duration=DeliveryNpc::CandidateDuration(Wait->Action);
    Reply.TargetId=Wait->TargetId; Reply.EvidenceIds=Wait->EvidenceIds;
    Reply.Speech=TEXT("听到了，你接着说。");
    Agent->ActiveRequest=Refreshed; Agent->bDecisionPending=true;
    Agent->ReceiveDecision(Refreshed.RequestId,&Reply,NAME_None);
    TestEqual(TEXT("新回复通过检查并对应原玩家发言"),Agent->PublicState.ReplyToEventId,Pending.PlayerMessageId);
    TestEqual(TEXT("新回复保留模型台词"),Agent->PublicState.Speech,Reply.Speech);
    TestFalse(TEXT("新回复完成决定后不重复请求"),Agent->IsDecisionPending() || Agent->IsWaitingToReplyToPlayer());

    if (!TestTrue(TEXT("准备真正取消的玩家请求"),PreparePendingPlayer(Pending))) return false;
    TestTrue(TEXT("倒地取消旧动作"),Agent->TryFinishAction(Agent->PublicState.ActionId,EDeliveryNpcActionStatus::Cancelled,TEXT("KnockedDown")));
    TestFalse(TEXT("真正取消不会重发玩家消息"),Agent->IsWaitingToReplyToPlayer() || Agent->IsDecisionPending());
    TestFalse(TEXT("真正取消解除玩家回复归属"),Agent->PlayerReplyMessage.IsValid());

    if (!TestTrue(TEXT("准备控制器失效的玩家请求"),PreparePendingPlayer(Pending))) return false;
    Player->Destroy();
    Agent->TryFinishAction(Agent->PublicState.ActionId,EDeliveryNpcActionStatus::Succeeded,NAME_None);
    TestFalse(TEXT("玩家已退出时不因正常完成重新发请求"),Agent->IsWaitingToReplyToPlayer() || Agent->IsDecisionPending());
    return true;
}
#endif
