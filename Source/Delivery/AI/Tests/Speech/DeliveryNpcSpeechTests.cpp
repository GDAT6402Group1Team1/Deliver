#include "Misc/AutomationTest.h"
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/ScopeExit.h"
#include "Async/TaskGraphInterfaces.h"
#include "Sound/SoundWaveProcedural.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/Animation/DeliveryNpcConversationAnimInstance.h"
#include "AI/Presentation/Face/DeliveryNpcExpressionSet.h"
#include "AI/Presentation/Speech/DeliveryNpcSpeechTiming.h"
#include "AI/Presentation/Speech/DeliveryNpcVoiceCodec.h"
#include "Character/DeliveryCharacter.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"
#include "Vehicle/Motorbike/DeliveryMotorbike.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcSpeechStreamTest,"Delivery.AI.SentencePlayback",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcSpeechStreamTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
    const bool Enabled=Settings->bEnabled,Streaming=Settings->bStreamReplies;
    Settings->bEnabled=false; Settings->bStreamReplies=true;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); Settings->bEnabled=Enabled;Settings->bStreamReplies=Streaming; };
    auto* Npc=World->SpawnActor<ADeliveryNpcCharacter>();
    auto* SpeakerController=World->SpawnActor<APlayerController>();
    SpeakerController->PlayerState=World->SpawnActor<APlayerState>();
    auto* Player=World->SpawnActor<APawn>();
    SpeakerController->Possess(Player);
    auto* Agent=Npc->Agent.Get();auto* Presentation=Npc->Presentation.Get();
    auto WaitForEncoding=[](const UDeliveryNpcPresentationComponent* VoicePresentation)
    {
        const double Deadline=FPlatformTime::Seconds()+3;
        while (VoicePresentation->PendingEncodeTasks>0 && FPlatformTime::Seconds()<Deadline)
            FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        return VoicePresentation->PendingEncodeTasks==0;
    };
    Npc->SetActorTickEnabled(false);
    World->Tick(LEVELTICK_TimeOnly,1.f);
    FDeliveryNpcObservation Message; Message.Kind=TEXT("PlayerSpeech");Message.Description=TEXT("别挡路");Message.Subject=Player;Message.EventId=FGuid::NewGuid();
    TestTrue(TEXT("记录玩家发言"),Agent->ObserveEvent(Message));
    TestTrue(TEXT("真实玩家可以开始倾听"),Agent->BeginListening(Player,Message.EventId));
    TestEqual(TEXT("立即保存倾听对象"),Agent->GetListeningSpeaker(),static_cast<AActor*>(Player));
    Settings->bEnabled=true;
    TestTrue(TEXT("交谈请求被接受"),Agent->RequestDecision());
    Settings->bEnabled=false;
    const auto Request=Agent->ActiveRequest;
    TestTrue(TEXT("玩家请求获得优先级"),Request.bPlayerRequest);
    World->GetSubsystem<UDeliveryAiGatewaySubsystem>()->Cancel(Agent);
    FDeliveryNpcDecisionProposal Proposal;
    const auto* WaitCandidate=Request.Candidates.FindByPredicate([](const auto& Candidate)
        { return Candidate.Action==EDeliveryNpcAction::Wait && !Candidate.TargetId.IsValid(); });
    if (!TestNotNull(TEXT("使用服务器实际提供的等待候选"),WaitCandidate)) return false;
    Proposal.CapabilityVersion=Request.RequestedCapabilityVersion;
    Proposal.CandidateId=WaitCandidate->CandidateId;
    Proposal.TargetId=WaitCandidate->TargetId;
    Proposal.EvidenceIds=WaitCandidate->EvidenceIds;
    Proposal.RequestId=Request.RequestId;Proposal.NpcId=Request.NpcId;Proposal.WorldEpoch=Request.WorldEpoch;
    Proposal.ContextRevision=Request.ContextRevision;Proposal.PlanRevision=Request.PlanRevision;
    Proposal.Action=WaitCandidate->Action;Proposal.Duration=DeliveryNpc::CandidateDuration(WaitCandidate->Action);Proposal.Speech=TEXT("我听见了。");Proposal.Emotion=TEXT("Calm");
    TestTrue(TEXT("接收首个完整动作建议"),Agent->ReceiveStreamDecision(Request.RequestId,Proposal));
    TestFalse(TEXT("选好回复后已经结束倾听"),Agent->IsListening());
    TestTrue(TEXT("首句语音尚未到达时仍在等待回复"),Agent->IsDecisionPending());
    TestEqual(TEXT("等待语音时保留收话人"),Agent->GetPublicState().SpeechTarget.Get(),static_cast<AActor*>(Player));

    // 只复制客户端能收到的状态，检查等待语音时的视线不会依赖服务器请求记录
    FActorSpawnParameters ViewSpawn;ViewSpawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* ViewNpc=World->SpawnActor<ADeliveryNpcCharacter>(FVector(200,0,0),FRotator::ZeroRotator,ViewSpawn);
    ViewNpc->SetActorTickEnabled(false);
    ViewNpc->Agent->PublicState=Agent->GetPublicState();
    ViewNpc->Agent->bDecisionPending=Agent->IsDecisionPending();
    ViewNpc->Presentation->Expressions=NewObject<UDeliveryNpcExpressionSet>(ViewNpc);
    auto* ViewPresentation=ViewNpc->Presentation.Get();
    ViewPresentation->Expressions->Poses.FindOrAdd(TEXT("Thinking")).MorphWeights.Add(TEXT("FaceThinking"),1.f);
    ViewPresentation->Expressions->Poses.FindOrAdd(TEXT("Listening")).MorphWeights.Add(TEXT("FaceListening"),1.f);
    ViewPresentation->Expressions->Poses.FindOrAdd(TEXT("Angry")).MorphWeights.Add(TEXT("EyesAngry"),1.f);
    ViewPresentation->Expressions->Poses.FindOrAdd(TEXT("Down")).MorphWeights.Add(TEXT("EyesBlink"),1.f);
    ViewPresentation->NextBlink=ViewPresentation->ServerTime()+100;
    auto* FaceMesh=LoadObject<USkeletalMesh>(nullptr,TEXT("/Game/NPC/Residents/Appearance/SK_NpcManFace.SK_NpcManFace"));
    if (!TestNotNull(TEXT("注视回归使用正式角色网格"),FaceMesh)) return false;
    ViewNpc->GetMesh()->SetSkeletalMeshAsset(FaceMesh);
    auto* ConversationPose=NewObject<UDeliveryNpcConversationAnimInstance>(ViewNpc->GetMesh());
    ConversationPose->NativeInitializeAnimation();
    // 远端只用复制的说话者和服务器开始时间，短倾听结束后继续看着玩家
    const auto BeforeListeningState=ViewNpc->Agent->PublicState;
    ViewNpc->Agent->PublicState={};
    ViewNpc->Agent->ListeningSpeaker=Player;
    ViewNpc->Agent->ListeningStartedAt=Agent->GetListeningStartTime();
    ConversationPose->NativeUpdateAnimation(1.f);
    ViewPresentation->TickExpression(1.f);
    TestTrue(TEXT("远端刚听见玩家时显示倾听眼神和前倾"),ViewPresentation->IsShowingListeningReaction() &&
        ViewNpc->GetMesh()->GetMorphTarget(TEXT("FaceListening"))>.9f && !ConversationPose->ChestRotation.IsNearlyZero());
    ViewNpc->Agent->ListeningStartedAt=ViewPresentation->ServerTime()-ViewPresentation->Expressions->ListeningReactionDuration-.1;
    ConversationPose->NativeUpdateAnimation(1.f);
    ViewPresentation->TickExpression(1.f);
    TestTrue(TEXT("短倾听结束后目光保留，眼神和头姿一同转为思考"),!ViewPresentation->IsShowingListeningReaction() &&
        ViewNpc->Agent->IsListening() && ConversationPose->LookWeight>.9f && ConversationPose->ChestRotation.IsNearlyZero() &&
        !ConversationPose->HeadRotation.IsNearlyZero() && ViewNpc->GetMesh()->GetMorphTarget(TEXT("FaceThinking"))>.9f);
    ViewNpc->Agent->ListeningSpeaker=nullptr;
    ViewNpc->Agent->PublicState=BeforeListeningState;
    ConversationPose->NativeUpdateAnimation(1.f);
    ViewPresentation->TickExpression(1.f);
    TestTrue(TEXT("显示对象没有服务器正在处理的回复"),ViewNpc->Agent->Replies.IsEmpty());
    TestTrue(TEXT("Wait 回复等待首句时仍看向收话人"),ConversationPose->LookWeight>.9f);
    TestFalse(TEXT("等待语音时保留思考姿态"),ConversationPose->HeadRotation.IsNearlyZero());
    TestTrue(TEXT("平静动作等待首句时眼神与思考头姿一致"),ViewNpc->GetMesh()->GetMorphTarget(TEXT("FaceThinking"))>.9f);

    // 客户端只收到本轮编号时，后台仍在合成声音，本机还没有可排队的音频
    const auto BeforeVoiceState=ViewNpc->Agent->PublicState;
    ViewNpc->Agent->PublicState.Status=EDeliveryNpcActionStatus::Succeeded;
    ViewNpc->Agent->PublicState.Emotion=TEXT("Angry");
    ViewPresentation->ReplyAction=ViewNpc->Agent->PublicState.GetReplyId();
    TestTrue(TEXT("合成首句期间本机声音队列仍为空"),ViewPresentation->LocalVoices.IsEmpty());
    TestTrue(TEXT("声音尚未到达本机时仍等待本轮播放"),ViewPresentation->IsWaitingForSpeechPlayback());
    ConversationPose->NativeUpdateAnimation(1.f);
    ViewPresentation->TickExpression(1.f);
    TestTrue(TEXT("动作结束后仍按同轮编号保留生气表情"),ViewNpc->GetMesh()->GetMorphTarget(TEXT("EyesAngry"))>.9f);
    TestTrue(TEXT("后台合成首句时不改回思考歪头"),ConversationPose->HeadRotation.IsNearlyZero());
    ViewPresentation->ReplyAction=FGuid::NewGuid();
    TestTrue(TEXT("不同轮的后台合成不会延长旧情绪"),ViewPresentation->GetActionEmotion().IsNone());
    ViewPresentation->ReplyAction=ViewNpc->Agent->PublicState.GetReplyId();
    ViewPresentation->CancelledVoiceActions.Add(ViewPresentation->ReplyAction);
    TestTrue(TEXT("停止通知先到时立即结束旧表情"),ViewPresentation->GetActionEmotion().IsNone());
    ViewPresentation->CancelledVoiceActions.Reset();
    for (const auto Status:{EDeliveryNpcActionStatus::Failed,EDeliveryNpcActionStatus::Cancelled})
    {
        ViewNpc->Agent->PublicState.Status=Status;
        TestEqual(TEXT("活动失败或取消仍保留独立回复的情绪"),ViewPresentation->GetActionEmotion(),FName(TEXT("Angry")));
        TestTrue(TEXT("活动结束不结束本轮语音等待"),ViewPresentation->IsWaitingForSpeechPlayback());
    }
    ViewPresentation->StopStreamSpeech_Implementation(ViewPresentation->ReplyAction,false);
    TestTrue(TEXT("本轮回复确切取消后结束旧表情"),ViewPresentation->GetActionEmotion().IsNone());
    TestFalse(TEXT("本轮回复确切取消后结束等待"),ViewPresentation->IsWaitingForSpeechPlayback());
    ViewPresentation->CancelledVoiceActions.Reset();
    ViewNpc->Agent->PublicState=BeforeVoiceState;
    ViewPresentation->ReplyAction.Invalidate();

    // 后台先发来完成通知，首句编码还未返回；客户端此时只有本轮编号和收话人
    ViewNpc->Agent->bDecisionPending=false;
    ViewNpc->Agent->PublicState.Status=EDeliveryNpcActionStatus::Succeeded;
    ViewPresentation->ReplyAction=ViewNpc->Agent->PublicState.GetReplyId();
    TestTrue(TEXT("后台完成到声音入队之间仍有等待表现"),ViewPresentation->LocalVoices.IsEmpty() &&
        !ViewNpc->Agent->IsDecisionPending() && ViewPresentation->IsWaitingForSpeechPlayback());
    ConversationPose->NativeUpdateAnimation(1.f);
    ViewPresentation->TickExpression(1.f);
    TestTrue(TEXT("首句尚未入队时继续看向收话人"),ConversationPose->LookWeight>.9f);
    TestFalse(TEXT("首句尚未入队时保持思考歪头"),ConversationPose->HeadRotation.IsNearlyZero());
    TestTrue(TEXT("首句尚未入队时保持思考眼神"),ViewNpc->GetMesh()->GetMorphTarget(TEXT("FaceThinking"))>.9f);
    ViewPresentation->ReplyAction=FGuid::NewGuid();
    TestFalse(TEXT("其他轮次的编号不能延长本轮等待"),ViewPresentation->IsWaitingForSpeechPlayback());
    ViewPresentation->ReplyAction=ViewNpc->Agent->PublicState.GetReplyId();
    ViewPresentation->CancelledVoiceActions.Add(ViewPresentation->ReplyAction);
    TestFalse(TEXT("声音入队前收到取消也立即结束等待"),ViewPresentation->IsWaitingForSpeechPlayback());
    ViewPresentation->CancelledVoiceActions.Reset();
    ViewPresentation->ReplyAction.Invalidate();
    ConversationPose->NativeUpdateAnimation(1.f);
    ViewPresentation->TickExpression(1.f);
    TestTrue(TEXT("本轮编号清除后解除注视与思考表情"),!ViewPresentation->IsWaitingForSpeechPlayback() &&
        FMath::IsNearlyZero(ConversationPose->LookWeight) &&
        FMath::IsNearlyZero(ViewNpc->GetMesh()->GetMorphTarget(TEXT("FaceThinking"))));

    // 后台已完成，客户端收到台词但声音还未缓冲到可播放的长度
    ViewNpc->Agent->bDecisionPending=false;
    ViewNpc->Agent->PublicState.Status=EDeliveryNpcActionStatus::Succeeded;
    FDeliveryNpcSpeechState WaitingSpeech;
    WaitingSpeech.ActionId=ViewNpc->Agent->PublicState.GetReplyId();WaitingSpeech.TurnId=FGuid::NewGuid();
    WaitingSpeech.ReplyToEventId=ViewNpc->Agent->PublicState.ReplyToEventId;
    WaitingSpeech.Text=Proposal.Speech;WaitingSpeech.Emotion=TEXT("Calm");
    WaitingSpeech.bActive=true;WaitingSpeech.bRecordedVoice=true;WaitingSpeech.SampleRate=16000;
    WaitingSpeech.AudioFormatVersion=DeliveryNpcVoice::FormatVersion;WaitingSpeech.PcmBytes=6400;WaitingSpeech.EncodedBytes=32;
    WaitingSpeech.StartTime=ViewPresentation->ServerTime()+.25;WaitingSpeech.Duration=.24f;
    ViewPresentation->StartStreamSpeech_Implementation(WaitingSpeech);
    ViewPresentation->PlayRecordedVoice();
    TestFalse(TEXT("后台已结束且本机尚未开始播放"),ViewNpc->Agent->IsDecisionPending() || ViewPresentation->GetDisplayedSpeech().bActive);
    TestTrue(TEXT("当前回复仍在本机等待声音"),ViewPresentation->IsWaitingForSpeechPlayback());
    ConversationPose->NativeUpdateAnimation(1.f);
    ViewPresentation->TickExpression(1.f);
    TestTrue(TEXT("后台完成后待播期间仍看向这次回复的收话人"),ConversationPose->LookWeight>.9f);
    TestFalse(TEXT("待播期间仍保持思考歪头"),ConversationPose->HeadRotation.IsNearlyZero());
    TestTrue(TEXT("待播期间眼部继续显示思考表情"),ViewNpc->GetMesh()->GetMorphTarget(TEXT("FaceThinking"))>.9f);
    TestFalse(TEXT("本机等待查询不依赖服务器播放编号"),ViewPresentation->ReplyAction.IsValid());

    auto& WaitingVoice=ViewPresentation->LocalVoices[0];
    WaitingVoice.Pcm.SetNumZeroed(WaitingVoice.TotalBytes);
    WaitingVoice.State.StartTime=ViewPresentation->ServerTime()-1;
    ViewPresentation->PlayRecordedVoice();
    TestTrue(TEXT("声音就绪后实际开始显示台词"),ViewPresentation->GetDisplayedSpeech().bActive);
    TestFalse(TEXT("实际播放后不再报告待播"),ViewPresentation->IsWaitingForSpeechPlayback());
    ConversationPose->NativeUpdateAnimation(1.f);
    ViewPresentation->TickExpression(1.f);
    TestTrue(TEXT("实际开口后不再保持思考歪头"),ConversationPose->HeadRotation.IsNearlyZero());
    TestTrue(TEXT("实际播放时使用台词表情"),FMath::IsNearlyZero(ViewNpc->GetMesh()->GetMorphTarget(TEXT("FaceThinking"))));
    ViewPresentation->FinishLocalSentence();
    WaitingSpeech.TurnId=FGuid::NewGuid();
    ViewPresentation->StartStreamSpeech_Implementation(WaitingSpeech);

    ViewNpc->Agent->PublicState.Action=EDeliveryNpcAction::StrikeOnce;
    ViewNpc->Agent->PublicState.Status=EDeliveryNpcActionStatus::Running;
    ViewNpc->Agent->PublicState.Emotion=TEXT("Angry");
    ConversationPose->NativeUpdateAnimation(1.f);
    ViewPresentation->TickExpression(1.f);
    TestTrue(TEXT("接近攻击对象时不叠加交谈注视"),FMath::IsNearlyZero(ConversationPose->LookWeight));
    TestTrue(TEXT("动作的生气表情优先于声音待播"),ViewNpc->GetMesh()->GetMorphTarget(TEXT("EyesAngry"))>.9f &&
        FMath::IsNearlyZero(ViewNpc->GetMesh()->GetMorphTarget(TEXT("FaceThinking"))));
    ViewNpc->Agent->PublicState.Status=EDeliveryNpcActionStatus::Succeeded;
    ConversationPose->NativeUpdateAnimation(1.f);
    ViewPresentation->TickExpression(1.f);
    TestTrue(TEXT("动作完成但同轮声音待播时继续生气"),ViewNpc->GetMesh()->GetMorphTarget(TEXT("EyesAngry"))>.9f &&
        FMath::IsNearlyZero(ViewNpc->GetMesh()->GetMorphTarget(TEXT("FaceThinking"))));
    TestTrue(TEXT("已经生气时不切回思考歪头"),ConversationPose->HeadRotation.IsNearlyZero());
    for (const auto Status:{EDeliveryNpcActionStatus::Failed,EDeliveryNpcActionStatus::Cancelled})
    {
        ViewNpc->Agent->PublicState.Status=Status;
        ViewPresentation->TickExpression(1.f);
        TestTrue(TEXT("活动失败或取消后独立待播回复仍保持表情"),ViewNpc->GetMesh()->GetMorphTarget(TEXT("EyesAngry"))>.9f);
        TestTrue(TEXT("待播查询只跟随本轮回复"),ViewPresentation->IsWaitingForSpeechPlayback());
    }
    ViewPresentation->StopStreamSpeech_Implementation(WaitingSpeech.ActionId,false);
    ViewPresentation->TickExpression(1.f);
    TestTrue(TEXT("精确取消待播回复后解除生气表情"),FMath::IsNearlyZero(ViewNpc->GetMesh()->GetMorphTarget(TEXT("EyesAngry"))));
    TestFalse(TEXT("精确取消后本轮不再等待"),ViewPresentation->IsWaitingForSpeechPlayback());
    // 以下姿态检查重新安排一份有效的待播句，不沿用被取消的本机声音
    ViewPresentation->CancelledVoiceActions.Reset();
    WaitingSpeech.TurnId=FGuid::NewGuid();
    ViewPresentation->StartStreamSpeech_Implementation(WaitingSpeech);
    ViewNpc->Agent->PublicState=Agent->GetPublicState();
    ViewNpc->Agent->PublicState.Status=EDeliveryNpcActionStatus::Succeeded;
    ViewNpc->Combat->SetDeliberateProne(true);
    ConversationPose->NativeUpdateAnimation(1.f);
    ViewPresentation->TickExpression(1.f);
    TestTrue(TEXT("倒地时声音待播不能继续交谈注视"),FMath::IsNearlyZero(ConversationPose->LookWeight));
    TestTrue(TEXT("倒地表情优先于声音待播"),ViewNpc->GetMesh()->GetMorphTarget(TEXT("EyesBlink"))>.9f);
    ViewNpc->Combat->SetDeliberateProne(false);
    const float LookDistance=ViewNpc->Agent->MaxLookAtDistance;
    ViewNpc->Agent->MaxLookAtDistance=50;
    ConversationPose->NativeUpdateAnimation(1.f);
    TestTrue(TEXT("收话人太远时解除注视"),FMath::IsNearlyZero(ConversationPose->LookWeight));
    ViewNpc->Agent->MaxLookAtDistance=LookDistance;
    auto* LostSpeaker=World->SpawnActor<AActor>();
    ViewNpc->Agent->PublicState.SpeechTarget=LostSpeaker;LostSpeaker->Destroy();
    ConversationPose->NativeUpdateAnimation(1.f);
    TestTrue(TEXT("待播期间收话人失效后解除注视"),FMath::IsNearlyZero(ConversationPose->LookWeight));
    ViewNpc->Agent->PublicState.SpeechTarget=Player;
    ViewNpc->Agent->PublicState.ActionId=FGuid::NewGuid();
    ViewNpc->Agent->PublicState.ReplyId=FGuid::NewGuid();
    ConversationPose->NativeUpdateAnimation(1.f);
    ViewPresentation->TickExpression(1.f);
    TestFalse(TEXT("旧队列不能让新的动作继续等待"),ViewPresentation->IsWaitingForSpeechPlayback());
    TestTrue(TEXT("替换动作后不保留旧回复的注视和表情"),FMath::IsNearlyZero(ConversationPose->LookWeight) &&
        FMath::IsNearlyZero(ViewNpc->GetMesh()->GetMorphTarget(TEXT("FaceThinking"))));
    ViewNpc->Agent->PublicState=Agent->GetPublicState();
    ViewNpc->Agent->PublicState.ReplyToEventId=FGuid::NewGuid();
    TestFalse(TEXT("不同玩家发言的旧语音不能保持等待"),ViewPresentation->IsWaitingForSpeechPlayback());
    ViewNpc->Agent->PublicState.ReplyToEventId=WaitingSpeech.ReplyToEventId;
    ViewNpc->Agent->PublicState.Status=EDeliveryNpcActionStatus::Failed;
    TestTrue(TEXT("活动失败时同一回复的有效待播队列仍保持等待"),ViewPresentation->IsWaitingForSpeechPlayback());
    ViewNpc->Agent->PublicState.Status=EDeliveryNpcActionStatus::Succeeded;
    ViewPresentation->LocalVoices[0].FrontSince=FPlatformTime::Seconds()-WaitingSpeech.Duration-6;
    TestFalse(TEXT("声音等待超时后不再保留等待表现"),ViewPresentation->IsWaitingForSpeechPlayback());
    ViewPresentation->LocalVoices[0].FrontSince=FPlatformTime::Seconds();
    TestTrue(TEXT("取消前仍有这次回复的待播声音"),ViewPresentation->IsWaitingForSpeechPlayback());
    // 显示对象只有客户端收到的数据，取消也通过真实的客户端停止通知检查
    ViewPresentation->StopStreamSpeech_Implementation(WaitingSpeech.ActionId,false);
    TestTrue(TEXT("指定回复的停止通知清掉该轮全部待播句子"),ViewPresentation->LocalVoices.IsEmpty());
    ConversationPose->NativeUpdateAnimation(1.f);
    ViewPresentation->TickExpression(1.f);
    TestTrue(TEXT("取消回复后解除等待语音的注视"),FMath::IsNearlyZero(ConversationPose->LookWeight));
    TestFalse(TEXT("取消回复后本机不再等待播放"),ViewPresentation->IsWaitingForSpeechPlayback());
    TestTrue(TEXT("取消回复后结束思考表情"),FMath::IsNearlyZero(ViewNpc->GetMesh()->GetMorphTarget(TEXT("FaceThinking"))));
    // 同一段动作的新回复使用独立编号，迟到的旧停止通知不能影响它
    const FGuid StillRunningAction=ViewNpc->Agent->PublicState.ActionId;
    const FGuid NewReply=FGuid::NewGuid();
    ViewNpc->Agent->PublicState.ReplyId=NewReply;
    FDeliveryNpcSpeechState NewSpeech=WaitingSpeech;
    NewSpeech.ActionId=NewReply; NewSpeech.TurnId=FGuid::NewGuid();
    ViewPresentation->StartStreamSpeech_Implementation(NewSpeech);
    TestTrue(TEXT("同一动作的新回复可以正常等待声音"),ViewPresentation->IsWaitingForSpeechPlayback());
    ViewPresentation->StopStreamSpeech_Implementation(WaitingSpeech.ActionId,false);
    TestEqual(TEXT("新回复仍属于原来正在执行的动作"),ViewNpc->Agent->PublicState.ActionId,StillRunningAction);
    TestTrue(TEXT("旧停止通知保留新回复的声音队列"),ViewPresentation->LocalVoices.Num()==1 && ViewPresentation->LocalVoices[0].State.ActionId==NewReply);
    TestTrue(TEXT("旧停止通知不会结束新回复的等待表现"),ViewPresentation->IsWaitingForSpeechPlayback());
    TestFalse(TEXT("旧停止通知不会把新编号记为已取消"),ViewPresentation->CancelledVoiceActions.Contains(NewReply));
    ViewPresentation->StopStreamSpeech_Implementation(NewReply,false);
    TestTrue(TEXT("新回复自己的停止通知仍能清空队列"),ViewPresentation->LocalVoices.IsEmpty());
    TestFalse(TEXT("新回复自己的停止通知结束等待"),ViewPresentation->IsWaitingForSpeechPlayback());
    ConversationPose->NativeUninitializeAnimation();
    ViewNpc->Destroy();

    FDeliveryNpcSentence Second;Second.Sequence=1;Second.Text=TEXT("这就让开。");Second.Emotion=TEXT("Calm");Second.Pcm.SetNumZeroed(6400);
    Agent->ReceiveSentence(Request.RequestId,Second);
    Presentation->TickSentences();
    TestFalse(TEXT("没有第一句时不能先播第二句"),Presentation->GetSpeech().bActive);
    FDeliveryNpcSentence First=Second;First.Sequence=0;First.Text=Proposal.Speech;
    Agent->ReceiveSentence(Request.RequestId,First);
    if (!TestTrue(TEXT("处理后台编码完成的游戏线程回调"),WaitForEncoding(Presentation))) return false;
    TArray<uint8> ReferenceEncoded;FString EncodeError;
    TestTrue(TEXT("同一份声音可用于同步编码对照"),DeliveryNpcVoice::Encode(First.Pcm,First.SampleRate,ReferenceEncoded,EncodeError));
    TestTrue(TEXT("后台编码没有改变传输内容"),Presentation->EncodedSentences.Contains(0) && Presentation->EncodedSentences[0].Audio==ReferenceEncoded);
    Presentation->TickSentences();
    TestFalse(TEXT("动作尚未开始时不说行动承诺"),Presentation->GetSpeech().bActive);
    FDeliveryNpcPublicState Action;
    TestTrue(TEXT("开始动作"),Agent->StartAction(Action));
    TestTrue(TEXT("角色执行动作"),Npc->ExecuteAction(Action));
    Presentation->TickSentences();
    TestEqual(TEXT("先播放第一句"),Presentation->GetSpeech().Sequence,0);
    TestTrue(TEXT("使用合成语音而非电子音"),Presentation->GetSpeech().bRecordedVoice);
    const FGuid SavedTurn=Presentation->Speech.TurnId;
    TArray<uint8> Repair;int32 RepairTotal=0;
    TestTrue(TEXT("服务器保存了可补发的语音"),Presentation->GetSpeechAudioChunk(SavedTurn,0,4096,Repair,RepairTotal));
    Presentation->Speech.TurnId=FGuid::NewGuid();
    TestTrue(TEXT("当前句切换后仍可补发前一句"),Presentation->GetSpeechAudioChunk(SavedTurn,1,4096,Repair,RepairTotal));
    TestEqual(TEXT("补发返回原句编码总长度"),RepairTotal,Presentation->Speech.EncodedBytes);
    TestTrue(TEXT("补发内容经过压缩且单次不超过两千字节"),RepairTotal<First.Pcm.Num() && Repair.Num()<=2048);
    TestEqual(TEXT("服务器仍保留原始声音"),Presentation->ServerVoices[SavedTurn].Pcm.Num(),First.Pcm.Num());
    Presentation->ServerVoices[SavedTurn].ExpiresAt=FPlatformTime::Seconds()-1;
    TestFalse(TEXT("超过保留时间后不再补发"),Presentation->GetSpeechAudioChunk(SavedTurn,0,4096,Repair,RepairTotal));
    Presentation->ServerVoices[SavedTurn].ExpiresAt=0;Presentation->Speech.TurnId=SavedTurn;
    Agent->ReceiveSentence(Request.RequestId,First);
    TestEqual(TEXT("重复第一句不会再入队"),Presentation->Sentences.Num(),1);
    Agent->FinishReply(Request.RequestId,true,2);
    TestTrue(TEXT("模型结束后仍保留待播语音"),Presentation->HasPendingSpeech());
    Agent->CancelPlan(TEXT("TestInterrupt"));
    TestFalse(TEXT("打断停止字幕"),Presentation->GetSpeech().bActive);
    TestFalse(TEXT("打断移除所有等待句子"),Presentation->HasPendingSpeech());
    TestTrue(TEXT("打断释放待传音频"),Presentation->SendingAudio.IsEmpty());
    TestFalse(TEXT("打断后停止补发旧语音"),Presentation->GetSpeechAudioChunk(SavedTurn,0,4096,Repair,RepairTotal));
    Agent->ReceiveSentence(Request.RequestId,Second);
    TestTrue(TEXT("晚到音频不能复活旧对白"),Presentation->Sentences.IsEmpty());
    TestFalse(TEXT("被打断的发言没有记为完整回答"),Agent->Evidence.ContainsByPredicate([&](const auto& Item){return Item.Kind==TEXT("SelfSpeech")&&Item.ReplyToEventId==Message.EventId;}));
    Agent->BeginListening(Player,FGuid::NewGuid());Player->Destroy();
    Agent->TickComponent(.1f,LEVELTICK_All,nullptr);
    TestFalse(TEXT("说话者销毁后结束倾听"),Agent->IsListening());
    FActorSpawnParameters PeerSpawn;PeerSpawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Peer=World->SpawnActor<ADeliveryNpcCharacter>(FVector(120,0,0),FRotator::ZeroRotator,PeerSpawn);
    Peer->SetActorTickEnabled(false);
    auto CountSelfSpeech=[Agent]()
    {
        return Agent->Evidence.FilterByPredicate([](const auto& Item){return Item.Kind==TEXT("SelfSpeech");}).Num();
    };
    auto StartTwoSentences=[&](FGuid MessageId)
    {
        const FGuid ActionId=FGuid::NewGuid();
        const FGuid ReplyId=FGuid::NewGuid();
        Agent->PublicState={};
        Agent->PublicState.ActionId=ActionId;Agent->PublicState.ReplyId=ReplyId;
        Agent->PublicState.ReplyToEventId=MessageId;Agent->PublicState.bStreamSpeech=true;
        Agent->PublicState.Status=EDeliveryNpcActionStatus::Running;
        Presentation->BeginReply(ReplyId,MessageId,Npc->GetConversationDepth());
        FDeliveryNpcSentence FirstText=First,SecondText=Second;
        FirstText.Pcm.Reset();SecondText.Pcm.Reset();
        Presentation->QueueSentence(ReplyId,MessageId,FirstText);
        Presentation->QueueSentence(ReplyId,MessageId,SecondText);
        Presentation->FinishSentences(ReplyId,2);
        Presentation->TickSentences();
    };
    auto FinishScheduledSentence=[&]()
    {
        // 本测试关闭音频，按服务器安排推进；实际 PCM 读取由 CompleteSpeech 检查
        Presentation->Speech.StartTime=Presentation->ServerTime()-Presentation->Speech.Duration-.1;
        Presentation->TickSentences();
    };
    const int32 BeforeInterruptedReply=CountSelfSpeech();
    StartTwoSentences(Message.EventId);
    FinishScheduledSentence();
    TestEqual(TEXT("第一句结束后才累计已说内容"),Presentation->SpokenReply,First.Text);
    const FGuid InterruptedTurn=Presentation->Speech.TurnId;
    Presentation->CancelSpeech();
    TestEqual(TEXT("打断时只新增一条已说内容"),CountSelfSpeech(),BeforeInterruptedReply+1);
    TestTrue(TEXT("已说完第一句仍在记忆中，但不表示回答已完成"),Agent->Evidence.ContainsByPredicate([&](const auto& Item)
        {return Item.Kind==TEXT("SelfSpeech") && Item.Description==First.Text && !Item.ReplyToEventId.IsValid();}));
    TestFalse(TEXT("第二句未说完时不把整段回复记为已回答"),Agent->Evidence.ContainsByPredicate([&](const auto& Item)
        {return Item.Kind==TEXT("SelfSpeech") && Item.ReplyToEventId==Message.EventId;}));
    TestFalse(TEXT("被打断的第二句没有混入已说内容"),Agent->Evidence.ContainsByPredicate([&](const auto& Item)
        {return Item.Kind==TEXT("SelfSpeech") && Item.Description.Contains(Second.Text);}));
    TestFalse(TEXT("取消不向邻居广播未说完的回复"),Peer->Agent->RecentEventIds.Contains(InterruptedTurn));
    Presentation->CancelSpeech();
    TestEqual(TEXT("重复取消不会重复保存前一句"),CountSelfSpeech(),BeforeInterruptedReply+1);
    StartTwoSentences(FGuid::NewGuid());
    Presentation->CancelSpeech();
    TestEqual(TEXT("第一句尚未结束时不新增已说内容"),CountSelfSpeech(),BeforeInterruptedReply+1);
    const FGuid CompleteMessage=FGuid::NewGuid();
    StartTwoSentences(CompleteMessage);
    FinishScheduledSentence();FinishScheduledSentence();
    TestFalse(TEXT("关闭音频时整段回复仍能正常结束"),Presentation->HasPendingSpeech());
    TestEqual(TEXT("正常完成只保存整段，不额外保存每一句"),CountSelfSpeech(),BeforeInterruptedReply+2);
    TestTrue(TEXT("两句都结束后才标记玩家已获得完整回答"),Agent->Evidence.ContainsByPredicate([&](const auto& Item)
        {return Item.Kind==TEXT("SelfSpeech") && Item.ReplyToEventId==CompleteMessage && Item.Description==First.Text+Second.Text;}));
    TestEqual(TEXT("完整回复只通知邻居一次，并保留原问题编号"),Peer->Agent->Evidence.FilterByPredicate([&](const auto& Item)
        {return Item.Kind==TEXT("OverheardSpeech") && Item.ReplyToEventId==CompleteMessage && Item.Description==First.Text+Second.Text;}).Num(),1);
    StartTwoSentences(FGuid::NewGuid());
    if (!TestFalse(TEXT("超时检查前存在本机待播内容"),Presentation->LocalVoices.IsEmpty())) return false;
    Presentation->LocalVoices[0].FrontSince=FPlatformTime::Seconds()-20;
    Presentation->PlayRecordedVoice();
    TestFalse(TEXT("本机播放异常超时后结束回复"),Presentation->HasPendingSpeech());
    TestEqual(TEXT("超时的未完成句子不会写入记忆"),CountSelfSpeech(),BeforeInterruptedReply+2);
    auto* ProneNpc=World->SpawnActor<ADeliveryNpcCharacter>(FVector(5000,0,0),FRotator::ZeroRotator,PeerSpawn);
    ProneNpc->SetActorTickEnabled(false);
    FDeliveryNpcPublicState ProneAction;ProneAction.ActionId=FGuid::NewGuid();ProneAction.Action=EDeliveryNpcAction::LieDown;
    ProneAction.Status=EDeliveryNpcActionStatus::Running;ProneAction.bStreamSpeech=true;
    ProneNpc->Agent->PublicState=ProneAction;
    TestFalse(TEXT("兼容用例未设置新的回复编号"),ProneAction.ReplyId.IsValid());
    TestEqual(TEXT("没有ReplyId时仍沿用旧ActionId播放"),ProneAction.GetReplyId(),ProneAction.ActionId);
    ProneNpc->Presentation->BeginReply(ProneAction.ActionId,FGuid(),ProneNpc->GetConversationDepth());
    TestTrue(TEXT("主动躺下使用原有战斗姿态接口"),ProneNpc->ExecuteAction(ProneAction));
    TestTrue(TEXT("主动躺下与受击倒地可区分"),ProneNpc->Combat->IsDeliberatelyProne());
    FDeliveryNpcSentence ProneLine;ProneLine.Sequence=0;ProneLine.Text=TEXT("我就在这儿歇一会儿");
    ProneNpc->Presentation->QueueSentence(ProneAction.ActionId,FGuid(),ProneLine);
    ProneNpc->Presentation->FinishSentences(ProneAction.ActionId,1);
    ProneNpc->Presentation->TickSentences();
    TestTrue(TEXT("主动躺下仍可开始当前回复"),ProneNpc->Presentation->GetSpeech().bActive);
    TestTrue(TEXT("主动躺下没有取消当前动作"),ProneNpc->Agent->HasActiveAction());
    ProneNpc->Combat->SetDeliberateProne(false);
    ProneNpc->ReceiveImpact(FVector(100,0,0),ProneNpc->GetActorLocation());
    TestTrue(TEXT("真正受击进入原有倒地状态"),ProneNpc->IsKnockedDown() && !ProneNpc->Combat->IsDeliberatelyProne());
    TestFalse(TEXT("真正被击倒立即停止台词"),ProneNpc->Presentation->HasPendingSpeech());
    TestFalse(TEXT("真正被击倒立即取消动作"),ProneNpc->Agent->HasActiveAction());
    ProneNpc->Destroy();
    for (int32 Depth=0;Depth<=3;++Depth)
    {
        auto* Speaker=Depth%2==0?Npc:Peer;
        auto* Listener=Depth%2==0?Peer:Npc;
        auto* Speech=Speaker->Presentation.Get();
        const FGuid ReplyId=FGuid::NewGuid();
        auto& State=Speaker->Agent->PublicState;
        State={}; State.ActionId=FGuid::NewGuid(); State.ReplyId=ReplyId;
        State.Action=EDeliveryNpcAction::Wait; State.Status=EDeliveryNpcActionStatus::Running;
        State.bStreamSpeech=true; State.SpeechTarget=Listener;
        Speech->BeginReply(ReplyId,FGuid(),Speaker->GetConversationDepth(),Listener);
        TestEqual(TEXT("流式回复保留当前接话轮数"),Speech->SpeechDepth,Depth);
        const int32 PreviousListenerDepth=Listener->GetConversationDepth();
        // 按正式播放流程结束本轮，否则旧回复仍会占着当前播放位置
        FDeliveryNpcSentence Line; Line.Sequence=0; Line.Text=FString::Printf(TEXT("这是第%d轮接话"),Depth);
        Speech->QueueSentence(ReplyId,FGuid(),Line);
        Speech->FinishSentences(ReplyId,1);
        Speech->TickSentences();
        const FGuid Turn=Speech->Speech.TurnId;
        TestTrue(TEXT("本轮台词进入播放"),Speech->Speech.bActive);
        Speech->Speech.StartTime=Speech->ServerTime()-Speech->Speech.Duration-.1;
        Speech->TickSentences();
        TestFalse(TEXT("整句结束后释放本轮回复"),Speech->HasPendingSpeech());
        const bool Heard=Listener->Agent->RecentEventIds.Contains(Turn);
        if (Depth<3)
        {
            TestTrue(TEXT("前几轮完整台词通知附近居民"),Heard);
            TestEqual(TEXT("附近居民接话时增加一轮"),Listener->GetConversationDepth(),Depth+1);
        }
        else
        {
            TestTrue(TEXT("第三轮完整台词仍被邻居记住"),Heard);
            TestEqual(TEXT("最后一句不会推进邻居的接话轮数"),Listener->GetConversationDepth(),PreviousListenerDepth);
        }
    }
    // 原有完整回复也要通知最后一句，不能只有流式回复保留这段记忆
    Peer->Presentation->CancelSpeech();
    const int32 PreviousListenerDepth=Npc->GetConversationDepth();
    TestTrue(TEXT("开始第三轮完整回复"),Peer->Presentation->PublishSpeech(TEXT("话说到这儿吧。"),TEXT("Calm"),3));
    const FGuid FinalTurn=Peer->Presentation->Speech.TurnId;
    Peer->Presentation->Speech.StartTime=Peer->Presentation->ServerTime()-Peer->Presentation->Speech.Duration-.1;
    Peer->Presentation->TickComponent(0,LEVELTICK_All,nullptr);
    TestTrue(TEXT("完整回复的最后一句也被邻居记住"),Npc->Agent->RecentEventIds.Contains(FinalTurn));
    TestEqual(TEXT("完整回复的最后一句也不推进接话轮数"),Npc->GetConversationDepth(),PreviousListenerDepth);
    Peer->ObserveFact(TEXT("Recovered"),nullptr,TEXT("我已经恢复站立"),EDeliveryNpcEvidenceSource::ActionResult);
    TestEqual(TEXT("自己的动作结果不会重新开启闲聊"),Peer->GetConversationDepth(),3);
    Peer->ObserveFact(TEXT("PlayerBrush"),Npc,TEXT("这名快递员刚与你擦身碰到"),EDeliveryNpcEvidenceSource::Seen);
    TestEqual(TEXT("新的擦身事件可以开启下一次互动"),Peer->GetConversationDepth(),0);
    // 让玩家控制一个 Pawn，经过 HearSpeech 的正式玩家发言判断重新开启话题
    auto* PlayerController=World->SpawnActor<APlayerController>();
    PlayerController->PlayerState=World->SpawnActor<APlayerState>();
    PlayerController->PlayerState->SetIsABot(false);
    PlayerController->Possess(Npc);
    TestTrue(TEXT("闲聊结束后仍可接收玩家的新发言"),Peer->HearSpeech(Npc,TEXT("请问哪边能送货？"),FGuid::NewGuid(),0));
    TestEqual(TEXT("玩家新发言把接话轮数重置为零"),Peer->GetConversationDepth(),0);
    Peer->Presentation->BeginReply(FGuid::NewGuid(),FGuid::NewGuid(),Peer->GetConversationDepth());
    TestEqual(TEXT("新玩家话题的流式回复从零开始"),Peer->Presentation->SpeechDepth,0);
    const FGuid CancelledAction=FGuid::NewGuid(), CancelledMessage=FGuid::NewGuid();
    Presentation->BeginReply(CancelledAction,CancelledMessage,Npc->GetConversationDepth());
    Presentation->QueueSentence(CancelledAction,CancelledMessage,First);
    Presentation->CancelSpeech();
    TestTrue(TEXT("取消后仍处理已开始编码任务的完成回调"),WaitForEncoding(Presentation));
    TestTrue(TEXT("晚到的编码结果不能恢复已取消的回复"),Presentation->EncodedSentences.IsEmpty() && Presentation->Sentences.IsEmpty() && !Presentation->HasPendingSpeech());
    const FGuid EndingAction=FGuid::NewGuid(), EndingMessage=FGuid::NewGuid();
    Presentation->BeginReply(EndingAction,EndingMessage,Npc->GetConversationDepth());
    Presentation->QueueSentence(EndingAction,EndingMessage,First);
    Presentation->EndPlay(EEndPlayReason::EndPlayInEditor);
    TestTrue(TEXT("组件结束后后台任务能正常返回"),WaitForEncoding(Presentation));
    TestTrue(TEXT("组件结束后不再接受旧编码结果"),Presentation->EncodedSentences.IsEmpty() && Presentation->Sentences.IsEmpty());

    const auto* EncounterProperty=FindFProperty<FStructProperty>(ADeliveryCompanionCharacter::StaticClass(),TEXT("Encounter"));
    const auto* OwnerProperty=FindFProperty<FObjectPropertyBase>(UDeliveryCompanionComponent::StaticClass(),TEXT("OwnerPlayerState"));
    const auto* StartRagdoll=FindFProperty<FBoolProperty>(UDeliveryActiveRagdollComponent::StaticClass(),TEXT("bStartOnBeginPlay"));
    if (!TestTrue(TEXT("已结识和无模型角色的夹具字段存在"),EncounterProperty && OwnerProperty && StartRagdoll)) return false;
    const FTransform OwnerStart(FVector(7200,0,100));
    auto* OwnerBody=World->SpawnActorDeferred<ADeliveryCharacter>(ADeliveryCharacter::StaticClass(),OwnerStart,
        nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    auto* OwnerController=World->SpawnActor<APlayerController>();
    auto* Bike=World->SpawnActor<ADeliveryMotorbike>(FVector(7300,0,100),FRotator::ZeroRotator,PeerSpawn);
    auto* Companion=World->SpawnActor<ADeliveryCompanionCharacter>(FVector(7100,0,100),FRotator::ZeroRotator,PeerSpawn);
    auto* Visitor=World->SpawnActor<APawn>(OwnerStart.GetLocation(),FRotator::ZeroRotator,PeerSpawn);
    if (!TestTrue(TEXT("建立真实上下车与同位旁观测试"),OwnerBody && OwnerController && Bike && Companion && Visitor)) return false;
    StartRagdoll->SetPropertyValue_InContainer(OwnerBody->GetActiveRagdoll(),false);
    OwnerBody->FinishSpawning(OwnerStart);
    OwnerController->PlayerState=World->SpawnActor<APlayerState>();
    if (!TestNotNull(TEXT("主人有真实 PlayerState"),OwnerController->PlayerState.Get())) return false;
    OwnerController->PlayerState->SetOwner(OwnerController);
    OwnerController->Possess(OwnerBody);
    // 只布置已经结识的前置，之后走原事件、候选、防串话和玩家上下车
    EncounterProperty->ContainerPtrToValuePtr<FDeliveryCompanionEncounterState>(Companion)->Phase=EDeliveryCompanionEncounterPhase::Joined;
    OwnerProperty->SetObjectPropertyValue_InContainer(Companion->Companion,OwnerController->PlayerState);
    auto* JoinedAgent=Companion->Agent.Get();
    const auto RecordSubject=[&](FName Kind,APawn* Subject,EDeliveryNpcEvidenceSource Source,bool bNeedsReply)
    {
        FDeliveryNpcObservation Event;
        Event.EventId=FGuid::NewGuid(); Event.Kind=Kind; Event.Subject=Subject; Event.Source=Source;
        Event.Description=TEXT("初遇后的真实场景对象"); Event.bNeedsReply=bNeedsReply;
        return JoinedAgent->ObserveEvent(Event)?Event.EventId:FGuid();
    };
    const FGuid VisitorEvent=RecordSubject(TEXT("NearbyVisitor"),Visitor,EDeliveryNpcEvidenceSource::Seen,false);
    const FGuid JoinedEvent=RecordSubject(TEXT("CompanionJoined"),OwnerBody,EDeliveryNpcEvidenceSource::ActionResult,true);
    const auto PrepareJoinedReply=[&](FDeliveryNpcDecisionRequest& Snapshot,FGuid TargetEvent,FDeliveryNpcDecisionProposal& Reply)
    {
        if (!JoinedAgent->BuildDecisionRequest(FGuid::NewGuid(),Snapshot) || Snapshot.JoinedResponseEvent!=JoinedEvent) return false;
        const auto* Fact=Snapshot.Evidence.FindByPredicate([TargetEvent](const auto& Item){ return Item.EventId==TargetEvent; });
        const auto* Wait=Snapshot.Candidates.FindByPredicate([](const auto& Item){ return Item.Action==EDeliveryNpcAction::Wait; });
        if (!Fact || !Wait) return false;
        Reply={}; Reply.CapabilityVersion=Snapshot.RequestedCapabilityVersion;
        Reply.NpcId=Snapshot.NpcId; Reply.WorldEpoch=Snapshot.WorldEpoch; Reply.RequestId=Snapshot.RequestId;
        Reply.ContextRevision=Snapshot.ContextRevision; Reply.PlanRevision=Snapshot.PlanRevision;
        Reply.CandidateId=Wait->CandidateId; Reply.Action=Wait->Action; Reply.Duration=1.f;
        Reply.Speech=TEXT("谢了，刚才是你扶我起来的。"); Reply.bHasSpeechTarget=true; Reply.SpeechTargetId=Fact->SubjectId;
        JoinedAgent->ActiveRequest=Snapshot;
        return true;
    };
    FDeliveryNpcDecisionRequest WalkingSnapshot, RidingSnapshot;
    FDeliveryNpcDecisionProposal JoinedReply;
    TestTrue(TEXT("同一层相邻主人可以听见现场道谢"),JoinedAgent->CanHearJoinedResponse(OwnerBody));
    if (!TestTrue(TEXT("实际候选在主人步行时封存"),PrepareJoinedReply(WalkingSnapshot,JoinedEvent,JoinedReply))) return false;
    if (!TestTrue(TEXT("主人实际控制已登记的车辆"),Bike->TryEnter(OwnerBody) && OwnerController->GetPawn()==Bike &&
        Bike->GetRegisteredPlayer()==OwnerController->PlayerState)) return false;
    const auto* VisitorFact=WalkingSnapshot.Evidence.FindByPredicate([VisitorEvent](const auto& Item){ return Item.EventId==VisitorEvent; });
    if (!TestNotNull(TEXT("同位置访客也在原请求表中"),VisitorFact)) return false;
    auto VisitorReply=JoinedReply;
    VisitorReply.SpeechTargetId=VisitorFact->SubjectId;
    FName JoinedReason;
    TestFalse(TEXT("同位访客不能当作原救援主体"),JoinedAgent->TryAcceptProposal(VisitorReply,JoinedReason));
    TestEqual(TEXT("正确报告旧收话人不符"),JoinedReason,FName(TEXT("JoinedReplyTargetChanged")));
    auto StaleReply=JoinedReply;
    ++StaleReply.PlanRevision;
    TestFalse(TEXT("真正换控也不放行旧修订"),JoinedAgent->TryAcceptProposal(StaleReply,JoinedReason));
    JoinedAgent->ActiveRequest.JoinedResponseEvent=FGuid::NewGuid();
    TestFalse(TEXT("真正换控也不放行原事件已变的请求"),JoinedAgent->TryAcceptProposal(JoinedReply,JoinedReason));
    JoinedAgent->ActiveRequest=WalkingSnapshot;
    const FVector BikeNear=Bike->GetActorLocation();
    const FVector BikeFar=BikeNear+FVector(3000,0,0);
    Bike->SetActorLocation(BikeFar,false,nullptr,ETeleportType::TeleportPhysics);
    TestFalse(TEXT("模型返回前走远的同一主人无法听见现场道谢"),JoinedAgent->CanHearJoinedResponse(Bike));
    FDeliveryNpcDecisionRequest DistantSnapshot;
    if (!TestTrue(TEXT("远离后仍可生成普通自动请求"),JoinedAgent->BuildDecisionRequest(FGuid::NewGuid(),DistantSnapshot))) return false;
    TestFalse(TEXT("远处的自动请求不带本轮道谢标记"),DistantSnapshot.JoinedResponseEvent.IsValid());
    TestFalse(TEXT("远处的自动请求不发送待道谢事件"),DistantSnapshot.Evidence.ContainsByPredicate([JoinedEvent](const auto& Fact)
        { return Fact.EventId==JoinedEvent; }));
    TestEqual(TEXT("远离期间保留原救助事件供回到近处再说"),JoinedAgent->PendingJoinedEvent,JoinedEvent);
    Bike->SetActorLocation(BikeNear,false,nullptr,ETeleportType::TeleportPhysics);
    JoinedAgent->ActiveRequest=WalkingSnapshot;
    if (!TestTrue(TEXT("原步行主体可接续到主人当前真实车辆"),JoinedAgent->ReceiveStreamDecision(WalkingSnapshot.RequestId,JoinedReply))) return false;
    TestTrue(TEXT("已接受流式道谢使用当前车辆及原事件"),JoinedAgent->GetPublicState().SpeechTarget==Bike &&
        JoinedAgent->GetPublicState().ReplyToEventId==JoinedEvent);
    JoinedAgent->CancelPlan(TEXT("TestInterruptBeforeThankYouPlayed"));

    const FGuid BikeEvent=RecordSubject(TEXT("VisibleNow"),Bike,EDeliveryNpcEvidenceSource::Seen,false);
    if (!TestTrue(TEXT("实际候选在驾驶中封存登记车辆"),PrepareJoinedReply(RidingSnapshot,BikeEvent,JoinedReply))) return false;
    TSharedPtr<FJsonObject> RidingJson;
    const TArray<TSharedPtr<FJsonValue>>* EncodedSubjects=nullptr;
    const FString EncodedListenerId=JoinedReply.SpeechTargetId.ToString(EGuidFormats::DigitsWithHyphensLower);
    if (!TestTrue(TEXT("道谢请求实际编码了人物表"),
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(DeliveryNpc::EncodeRequest(RidingSnapshot)),RidingJson) &&
        RidingJson.IsValid() && RidingJson->TryGetArrayField(TEXT("subjects"),EncodedSubjects))) return false;
    TestTrue(TEXT("后台名单把已核验的驾驶主人当作可选玩家收话对象"),EncodedSubjects->ContainsByPredicate([&](const auto& Item)
    {
        const auto& Subject=Item->AsObject();
        return Subject->GetStringField(TEXT("subject_id"))==EncodedListenerId && Subject->GetStringField(TEXT("kind"))==TEXT("player");
    }));
    AddExpectedError(FString::Printf(TEXT("%s: Skeletal mesh has no Physics Asset."),*OwnerBody->GetName()),
        EAutomationExpectedErrorFlags::Contains,1);
    Bike->ExitVehicle();
    if (!TestTrue(TEXT("主人通过实际下车取回原角色控制权"),OwnerController->GetPawn()==OwnerBody && !Bike->GetDriver())) return false;
    if (!TestTrue(TEXT("原车辆主体可接续到主人当前步行角色"),JoinedAgent->ReceiveStreamDecision(RidingSnapshot.RequestId,JoinedReply))) return false;
    TestTrue(TEXT("下车后仍使用原事件和已选台词"),JoinedAgent->GetPublicState().SpeechTarget==OwnerBody &&
        JoinedAgent->GetPublicState().ReplyToEventId==JoinedEvent && JoinedAgent->GetPublicState().Speech==JoinedReply.Speech);
    FDeliveryNpcPublicState StartedReply;
    if (!TestTrue(TEXT("行为树接手前主人再次上车"),Bike->TryEnter(OwnerBody) && OwnerController->GetPawn()==Bike) ||
        !TestTrue(TEXT("同一个道谢动作可正常开始"),JoinedAgent->StartAction(StartedReply))) return false;
    TestTrue(TEXT("开始动作前按新控制对象重绑且不改原事件和已选台词"),StartedReply.SpeechTarget==Bike &&
        StartedReply.ReplyToEventId==JoinedEvent && StartedReply.Speech==JoinedReply.Speech);
    TestTrue(TEXT("已排队道谢也能解析到玩家当前控制的车辆"),
        JoinedAgent->GetReplyListenerForPlayback(StartedReply.GetReplyId(),OwnerBody)==Bike);

    auto* Wall=World->SpawnActor<AActor>();
    if (!TestNotNull(TEXT("构造真实道谢遮挡墙"),Wall)) return false;
    auto* WallCollision=NewObject<UBoxComponent>(Wall);
    Wall->SetRootComponent(WallCollision);
    Wall->AddInstanceComponent(WallCollision);
    WallCollision->SetBoxExtent(FVector(12,140,150));
    WallCollision->SetCollisionProfileName(TEXT("BlockAll"));
    WallCollision->RegisterComponent();
    Wall->SetActorLocation((Companion->GetActorLocation()+BikeNear)*.5);
    TestFalse(TEXT("同层近距离的墙挡住现场道谢"),JoinedAgent->CanHearJoinedResponse(Bike));
    TestFalse(TEXT("隔墙不开始已排队的自动道谢"),JoinedAgent->CanStartReplySpeech(StartedReply.GetReplyId()));
    WallCollision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    TestTrue(TEXT("移开墙后同一个主人可接着听"),JoinedAgent->CanStartReplySpeech(StartedReply.GetReplyId()));
    Wall->Destroy();

    auto* JoinedPresentation=Companion->Presentation.Get();
    FDeliveryNpcSentence JoinedSentence;
    JoinedSentence.Sequence=0;
    JoinedSentence.Text=JoinedReply.Speech;
    JoinedPresentation->QueueSentence(StartedReply.GetReplyId(),JoinedEvent,JoinedSentence);
    JoinedPresentation->TickSentences();
    const auto BegunSpeech=JoinedPresentation->GetSpeech();
    if (!TestTrue(TEXT("句子沿正式播放流程开始并分配实际语音编号"),BegunSpeech.bActive &&
        BegunSpeech.ActionId==StartedReply.GetReplyId() && BegunSpeech.TurnId.IsValid())) return false;
    if (!TestTrue(TEXT("原模型轮次声明一段实际台词"),JoinedAgent->FinishModelReply(RidingSnapshot.RequestId,1))) return false;
    World->Tick(LEVELTICK_TimeOnly,.2f);
    Bike->SetActorLocation(BikeFar,false,nullptr,ETeleportType::TeleportPhysics);
    JoinedAgent->TickComponent(0.f,LEVELTICK_All,nullptr);
    const auto* BegunReply=JoinedAgent->Replies.Find(RidingSnapshot.RequestId);
    TestTrue(TEXT("道谢已开始后走出范围，真实回复记录错过播放"),BegunReply && BegunReply->bJoinedPlaybackMissed);
    Bike->SetActorLocation(BikeNear,false,nullptr,ETeleportType::TeleportPhysics);

    const auto RecordCompletedSpeech=[&](FGuid SpeechId)
    {
        FDeliveryNpcObservation Spoken;
        Spoken.EventId=SpeechId; Spoken.Kind=TEXT("SelfSpeech"); Spoken.Source=EDeliveryNpcEvidenceSource::ActionResult;
        Spoken.Subject=Companion; Spoken.Addressee=Bike; Spoken.ReplyToEventId=JoinedEvent; Spoken.Description=JoinedSentence.Text;
        return JoinedAgent->ObserveEvent(Spoken);
    };
    TestTrue(TEXT("错过的播放仍有二狗说完的事实"),RecordCompletedSpeech(BegunSpeech.TurnId));
    JoinedAgent->FinishReplyPlayback(StartedReply.GetReplyId(),BegunSpeech.TurnId);
    TestEqual(TEXT("主人已错过时，返回不让旧播放结清获救事件"),JoinedAgent->PendingJoinedEvent,JoinedEvent);

    const auto StartAnotherJoinedReply=[&](FDeliveryNpcDecisionRequest& Snapshot,FDeliveryNpcPublicState& Started)
    {
        JoinedAgent->CancelPlan(TEXT("TestPreviousJoinedReplyFinished"));
        if (!PrepareJoinedReply(Snapshot,BikeEvent,JoinedReply) || !JoinedAgent->ReceiveStreamDecision(Snapshot.RequestId,JoinedReply) ||
            !JoinedAgent->StartAction(Started) || !JoinedAgent->FinishModelReply(Snapshot.RequestId,1)) return false;
        JoinedPresentation->QueueSentence(Started.GetReplyId(),JoinedEvent,JoinedSentence);
        JoinedPresentation->TickSentences();
        return JoinedPresentation->GetSpeech().bActive && JoinedPresentation->GetSpeech().ActionId==Started.GetReplyId();
    };
    FDeliveryNpcDecisionRequest RetrySnapshot;
    if (!TestTrue(TEXT("同一原事件可以再次请求道谢"),StartAnotherJoinedReply(RetrySnapshot,StartedReply))) return false;
    JoinedAgent->FinishReplyPlayback(StartedReply.GetReplyId(),FGuid::NewGuid());
    TestEqual(TEXT("已有同一原事件的旧台词也不能代替当前完成语音编号"),JoinedAgent->PendingJoinedEvent,JoinedEvent);
    if (!TestTrue(TEXT("主人仍在可交谈范围时收到原道谢的新版播放"),StartAnotherJoinedReply(RetrySnapshot,StartedReply))) return false;
    const FGuid CurrentSpeech=JoinedPresentation->GetSpeech().TurnId;
    TestTrue(TEXT("记录与当前回复匹配的实际完成事实"),RecordCompletedSpeech(CurrentSpeech));
    JoinedAgent->FinishReplyPlayback(StartedReply.GetReplyId(),CurrentSpeech);
    TestFalse(TEXT("主人留在可听范围且本轮真实完成，原道谢结清"),JoinedAgent->PendingJoinedEvent.IsValid());

    JoinedAgent->CancelPlan(TEXT("TestOwnerRequestedReply"));
    const FGuid NewJoinedEvent=RecordSubject(TEXT("CompanionJoined"),Bike,EDeliveryNpcEvidenceSource::ActionResult,true);
    Bike->SetActorLocation(BikeFar,false,nullptr,ETeleportType::TeleportPhysics);
    const FGuid OwnerMessage=RecordSubject(TEXT("PlayerSpeech"),Bike,EDeliveryNpcEvidenceSource::Heard,true);
    FDeliveryNpcDecisionRequest PlayerSnapshot;
    if (!TestTrue(TEXT("测试控制器发起的显式请求仍能封存"),NewJoinedEvent.IsValid() &&
        JoinedAgent->BeginListening(Bike,OwnerMessage) && JoinedAgent->BuildDecisionRequest(FGuid::NewGuid(),PlayerSnapshot))) return false;
    TestTrue(TEXT("自动现场限制不会从主人的主动回复里移除原救助事实"),PlayerSnapshot.bPlayerRequest &&
        PlayerSnapshot.PlayerMessageId==OwnerMessage && PlayerSnapshot.Evidence.ContainsByPredicate([NewJoinedEvent](const auto& Fact)
            { return Fact.EventId==NewJoinedEvent; }));

    JoinedAgent->CancelPlan(TEXT("TestCompanionCommandReply"));
    Bike->SetActorLocation(BikeNear,false,nullptr,ETeleportType::TeleportPhysics);
    const auto StageCommandReply=[&](FDeliveryNpcDecisionRequest& Snapshot,FDeliveryNpcSentence& Line,FGuid& ReplyId)
    {
        const FGuid MessageId=RecordSubject(TEXT("PlayerSpeech"),Bike,EDeliveryNpcEvidenceSource::Heard,true);
        if (!MessageId.IsValid() || !JoinedAgent->BeginListening(Bike,MessageId) ||
            !JoinedAgent->BuildDecisionRequest(FGuid::NewGuid(),Snapshot)) return false;
        const auto* Stay=Snapshot.Candidates.FindByPredicate([](const auto& Candidate)
            { return Candidate.Action==EDeliveryNpcAction::Stay; });
        if (!Stay || !Snapshot.bRequesterIsCompanionOwner) return false;
        FDeliveryNpcDecisionProposal Decision;
        Decision.CapabilityVersion=Snapshot.RequestedCapabilityVersion;
        Decision.RequestId=Snapshot.RequestId; Decision.WorldEpoch=Snapshot.WorldEpoch; Decision.NpcId=Snapshot.NpcId;
        Decision.ContextRevision=Snapshot.ContextRevision; Decision.PlanRevision=Snapshot.PlanRevision;
        Decision.CandidateId=Stay->CandidateId; Decision.Action=Stay->Action; Decision.TargetId=Stay->TargetId;
        Decision.Duration=0.f; Decision.EvidenceIds=Stay->EvidenceIds;
        Line=First; Line.Text=TEXT("行，我就在这等你。");
        Decision.Speech=Line.Text; Decision.Emotion=Line.Emotion;
        JoinedAgent->ActiveRequest=Snapshot;
        if (!JoinedAgent->ReceiveStreamDecision(Snapshot.RequestId,Decision)) return false;
        ReplyId=JoinedAgent->GetPublicState().GetReplyId();
        JoinedAgent->ReceiveSentence(Snapshot.RequestId,Line);
        if (!JoinedAgent->FinishModelReply(Snapshot.RequestId,1)) return false;
        JoinedAgent->FinishReply(Snapshot.RequestId,true,1);
        const auto* Reply=JoinedAgent->Replies.Find(Snapshot.RequestId);
        return ReplyId.IsValid() && ReplyId==JoinedAgent->GetPublicState().ActionId && Reply && Reply->bTransferFinished &&
            JoinedPresentation->HasReply(ReplyId) && WaitForEncoding(JoinedPresentation) &&
            JoinedPresentation->EncodedSentences.Contains(0) && !JoinedPresentation->EncodedSentences[0].Audio.IsEmpty();
    };
    const auto CheckWaiting=[&](const TCHAR* Assertion,FGuid ReplyId)
    {
        JoinedPresentation->TickSentences();
        return TestTrue(Assertion,!JoinedAgent->CanStartReplySpeech(ReplyId) && !JoinedPresentation->GetSpeech().bActive &&
            JoinedPresentation->HasReply(ReplyId) && JoinedPresentation->EncodedSentences.Contains(0));
    };

    FDeliveryNpcDecisionRequest RejectedCommand;
    FDeliveryNpcSentence RejectedLine;
    FGuid RejectedReplyId;
    if (!TestTrue(TEXT("失败用例已接收整轮真实编码语音"),StageCommandReply(RejectedCommand,RejectedLine,RejectedReplyId))) return false;
    CheckWaiting(TEXT("命令仅被采纳时保持语音待播"),RejectedReplyId);
    FDeliveryNpcPublicState RejectedAction;
    if (!TestTrue(TEXT("行为树可按原候选启动"),JoinedAgent->StartAction(RejectedAction))) return false;
    CheckWaiting(TEXT("启动动作后仍等待服务器应用同伴安排"),RejectedReplyId);
    if (!TestTrue(TEXT("同一主人改变安排使旧命令修订失效"),Companion->Companion->ApplyCommand(EDeliveryNpcAction::Follow,
        OwnerController->PlayerState,RejectedAction.CompanionCommandRevision))) return false;
    TestFalse(TEXT("旧安排在正式执行入口被拒绝"),Companion->ExecuteAction(RejectedAction));
    // 同时保存另一轮本机待播内容，失败时只清除这次命令的回复
    FDeliveryNpcSpeechState UnrelatedSpeech;
    UnrelatedSpeech.ActionId=FGuid::NewGuid(); UnrelatedSpeech.TurnId=FGuid::NewGuid(); UnrelatedSpeech.bActive=true;
    UnrelatedSpeech.StartTime=JoinedPresentation->ServerTime()+1.; UnrelatedSpeech.Duration=1.f;
    JoinedPresentation->StartStreamSpeech_Implementation(UnrelatedSpeech);
    if (!TestTrue(TEXT("服务端按失败终止旧安排"),JoinedAgent->TryFinishAction(RejectedAction.ActionId,
        EDeliveryNpcActionStatus::Failed,TEXT("StartRejected")))) return false;
    TestTrue(TEXT("按准确回复编号撤下已传输完成的语音"),!JoinedPresentation->HasReply(RejectedReplyId) &&
        JoinedPresentation->CancelledVoiceActions.Contains(RejectedReplyId) &&
        JoinedPresentation->EncodedSentences.IsEmpty() && !JoinedAgent->Replies.Contains(RejectedCommand.RequestId));
    TestTrue(TEXT("取消旧安排保留另一轮的待播内容"),!JoinedPresentation->CancelledVoiceActions.Contains(UnrelatedSpeech.ActionId) &&
        JoinedPresentation->LocalVoices.ContainsByPredicate([&UnrelatedSpeech](const auto& Voice)
            { return Voice.State.TurnId==UnrelatedSpeech.TurnId; }));
    JoinedPresentation->StopStreamSpeech_Implementation(UnrelatedSpeech.ActionId,false);

    FDeliveryNpcDecisionRequest ConfirmedCommand;
    FDeliveryNpcSentence ConfirmedLine;
    FGuid ConfirmedReplyId;
    if (!TestTrue(TEXT("成功用例独立接收下一轮语音"),StageCommandReply(ConfirmedCommand,ConfirmedLine,ConfirmedReplyId))) return false;
    JoinedAgent->ReceiveSentence(RejectedCommand.RequestId,RejectedLine);
    TestTrue(TEXT("旧回复的迟到句不复活或覆盖新队列"),!JoinedPresentation->HasReply(RejectedReplyId) &&
        ConfirmedReplyId!=RejectedReplyId && JoinedPresentation->HasReply(ConfirmedReplyId));
    FDeliveryNpcPublicState ConfirmedAction;
    if (!TestTrue(TEXT("新命令正常启动"),JoinedAgent->StartAction(ConfirmedAction)) ||
        !TestTrue(TEXT("新命令经服务器实装到等候状态"),Companion->ExecuteAction(ConfirmedAction) &&
            Companion->Companion->GetMode()==EDeliveryCompanionMode::Stay)) return false;
    CheckWaiting(TEXT("服务器尚未发出动作成功回执时保留语音队列"),ConfirmedReplyId);
    if (!TestTrue(TEXT("只有本次成功回执完成安排"),JoinedAgent->TryFinishAction(ConfirmedAction.ActionId,
        EDeliveryNpcActionStatus::Succeeded,TEXT("CompanionModeChanged")))) return false;
    JoinedPresentation->TickSentences();
    TestTrue(TEXT("服务端确认成功后播放同一份已收语音"),JoinedAgent->CanStartReplySpeech(ConfirmedReplyId) &&
        JoinedPresentation->GetSpeech().bActive && JoinedPresentation->GetSpeech().bRecordedVoice &&
        JoinedPresentation->GetSpeech().ActionId==ConfirmedReplyId && JoinedPresentation->GetSpeech().Text==ConfirmedLine.Text);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcSpeechSendBudgetTest,"Delivery.AI.SpeechSendBudget",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcSpeechSendBudgetTest::RunTest(const FString&)
{
    for (const int32 Total:{24000,30000})
    {
        const float Duration=8;
        int32 Sent=0;
        // 10 FPS 连续发送八秒语音，检查句末前已经发送全部内容
        for (int32 Frame=0;Frame<=80;++Frame)
        {
            const double Now=Frame*.1-.25;
            const int32 Bytes=DeliveryNpcSpeech::GetSendBytes(Now,Duration,Total,Sent);
            TestTrue(TEXT("单帧压缩语音不超过两千字节"),Bytes>=0 && Bytes<=2048);
            Sent+=Bytes;
            const int32 Expected=int32(FMath::Min(double(Total),FMath::Max(0.0,Now+.4)*Total/Duration));
            TestEqual(TEXT("低帧率下发送进度跟随播放时间"),Sent,Expected);
        }
        TestEqual(TEXT("整句话已发送完毕"),Sent,Total);
        TestEqual(TEXT("长卡顿后仍限制单帧发送长度"),DeliveryNpcSpeech::GetSendBytes(5,Duration,Total,0),2048);
        TestEqual(TEXT("已发送完整语音时不再发送"),DeliveryNpcSpeech::GetSendBytes(10,Duration,Total,Total),0);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcCompleteSpeechTest,"Delivery.AI.CompleteSpeech",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcCompleteSpeechTest::RunTest(const FString&)
{
    auto* Settings=GetMutableDefault<UDeliveryAiSettings>(); const bool Enabled=Settings->bEnabled; Settings->bEnabled=false;
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit);GEngine->DestroyWorldContext(World);World->DestroyWorld(false);Settings->bEnabled=Enabled; };
    auto* Npc=World->SpawnActor<ADeliveryNpcCharacter>(); Npc->SetActorTickEnabled(false); Npc->SetRole(ROLE_SimulatedProxy);
    auto* P=Npc->Presentation.Get();
    FDeliveryNpcSpeechState First;
    First.ActionId=FGuid::NewGuid();First.TurnId=FGuid::NewGuid();First.ReplyToEventId=FGuid::NewGuid();
    First.bActive=true;First.bRecordedVoice=true;First.SampleRate=16000;First.StartTime=-1;First.Duration=2.04f;
    First.Text=TEXT("第一句从头说到尾");First.Emotion=TEXT("Calm");
    FDeliveryNpcSpeechState Second=First;Second.TurnId=FGuid::NewGuid();Second.Sequence=1;Second.Duration=.24f;Second.Text=TEXT("接着第二句");
    TArray<uint8> FirstPcm,SecondPcm;
    FirstPcm.SetNumUninitialized(64000);SecondPcm.SetNumUninitialized(6400);
    for (int32 Index=0;Index<FirstPcm.Num();++Index) FirstPcm[Index]=uint8(Index%251);
    for (int32 Index=0;Index<SecondPcm.Num();++Index) SecondPcm[Index]=uint8(255-Index%239);
    TArray<uint8> FirstEncoded,SecondEncoded,FirstDecoded,SecondDecoded;
    FString Error;
    if (!TestTrue(TEXT("第一句压缩成功"),DeliveryNpcVoice::Encode(FirstPcm,16000,FirstEncoded,Error)) ||
        !TestTrue(TEXT("第二句压缩成功"),DeliveryNpcVoice::Encode(SecondPcm,16000,SecondEncoded,Error))) return false;
    FDeliveryNpcVoiceDecoder FirstDecoder,SecondDecoder;
    int32 FirstDecodedOffset=0,SecondDecodedOffset=0;
    if (!TestTrue(TEXT("生成第一句解码参考"),FirstDecoder.TryDecodeAvailable(FirstEncoded,FirstDecodedOffset,FirstDecoded,Error)) ||
        !TestTrue(TEXT("生成第二句解码参考"),SecondDecoder.TryDecodeAvailable(SecondEncoded,SecondDecodedOffset,SecondDecoded,Error))) return false;
    First.AudioFormatVersion=Second.AudioFormatVersion=DeliveryNpcVoice::FormatVersion;
    First.PcmBytes=FirstPcm.Num();First.EncodedBytes=FirstEncoded.Num();
    Second.PcmBytes=SecondPcm.Num();Second.EncodedBytes=SecondEncoded.Num();
    auto Receive=[P](FGuid Turn,const TArray<uint8>& Encoded,int32 Offset,int32 Count)
    {
        if (Offset>=Encoded.Num()) return;
        TArray<uint8> Part;Part.Append(Encoded.GetData()+Offset,FMath::Min(Count,Encoded.Num()-Offset));
        P->ReceiveAudio_Implementation(Turn,Offset,Encoded.Num(),Part);
    };
    P->StartStreamSpeech_Implementation(First);
    const int32 FirstPrefix=FMath::Min(2048,FirstEncoded.Num());
    Receive(First.TurnId,FirstEncoded,0,FirstPrefix);
    P->PlayRecordedVoice();
    if (!TestNotNull(TEXT("第一段声音进入 UE 音频队列"),P->RecordedWave.Get())) return false;
    TestEqual(TEXT("语音配置允许静音时继续播放"),P->RecordedWave->VirtualizationMode,EVirtualizationMode::PlayWhenSilent);
    TArray<uint8> Heard;
    auto ReadAudio=[&]()
    {
        uint8 Buffer[2048];
        while (P->RecordedWave && P->RecordedWave->GetAvailableAudioByteCount()>0)
        {
            const int32 Count=P->RecordedWave->GeneratePCMData(Buffer,1024);
            Heard.Append(Buffer,Count);
        }
    };
    ReadAudio();
    TestTrue(TEXT("较晚开始播放仍从解码后的句首开始"),Heard.Num()>0 && Heard.Num()<=FirstDecoded.Num() && FMemory::Memcmp(Heard.GetData(),FirstDecoded.GetData(),Heard.Num())==0);
    TestTrue(TEXT("字幕进度跟随已读取的解码样本"),FMath::IsNearlyEqual(P->GetDisplayedSpeechProgress(),float(Heard.Num())/FirstDecoded.Num()));
    // 补发范围与已有内容重叠时，接着添加后面的声音
    Receive(First.TurnId,FirstEncoded,FirstPrefix/2,FirstPrefix);
    // 刻意让网络分段切在编码包中间，完整包到达后才继续解码
    for (int32 Offset=FirstPrefix+FirstPrefix/2;Offset<FirstEncoded.Num();Offset+=257) Receive(First.TurnId,FirstEncoded,Offset,257);
    P->StartStreamSpeech_Implementation(Second);
    for (int32 Offset=0;Offset<SecondEncoded.Num();Offset+=257) Receive(Second.TurnId,SecondEncoded,Offset,257);
    P->Speech=First;P->Speech.bActive=false;P->OnRep_Speech();
    TestTrue(TEXT("服务器结束第一句时本机继续播放剩余内容"),P->GetDisplayedSpeech().bActive && P->GetDisplayedSpeech().TurnId==First.TurnId);
    P->PlayRecordedVoice();ReadAudio();
    TestTrue(TEXT("第一句解码后的声音完整进入播放器"),Heard==FirstDecoded);
    P->LocalVoices[0].DrainedAt=FPlatformTime::Seconds()-.2;
    P->PlayRecordedVoice();P->PlayRecordedVoice();
    TestEqual(TEXT("第一句结束后开始第二句"),P->GetDisplayedSpeech().TurnId,Second.TurnId);
    Heard.Reset();ReadAudio();
    TestTrue(TEXT("第二句解码内容完整且没有混入第一句"),Heard==SecondDecoded);
    P->LocalVoices[0].DrainedAt=FPlatformTime::Seconds()-.2;P->PlayRecordedVoice();
    TestFalse(TEXT("两句话说完后停止字幕"),P->GetDisplayedSpeech().bActive);
    TestTrue(TEXT("正常完成后没有待播句子"),P->LocalVoices.IsEmpty());
    // 在已经开始播放的语音末尾加入无效数据，检查声音损坏不会取消整轮回复
    TArray<uint8> DamagedAudio=FirstEncoded;DamagedAudio.Add(0);
    FDeliveryNpcSpeechState Damaged=First;Damaged.ActionId=FGuid::NewGuid();Damaged.TurnId=FGuid::NewGuid();
    Damaged.EncodedBytes=DamagedAudio.Num();
    FDeliveryNpcSpeechState AfterDamaged=Second;AfterDamaged.ActionId=Damaged.ActionId;AfterDamaged.TurnId=FGuid::NewGuid();
    P->StartStreamSpeech_Implementation(Damaged);Receive(Damaged.TurnId,DamagedAudio,0,FirstPrefix);P->PlayRecordedVoice();
    if (!TestNotNull(TEXT("坏包到达前这句话已经开始播放"),P->RecordedWave.Get())) return false;
    Heard.Reset();ReadAudio();
    const float ProgressBeforeDamage=P->GetDisplayedSpeechProgress();
    P->StartStreamSpeech_Implementation(AfterDamaged);
    for (int32 Offset=0;Offset<SecondEncoded.Num();Offset+=257) Receive(AfterDamaged.TurnId,SecondEncoded,Offset,257);
    AddExpectedError(TEXT("NpcSpeech decode_failed"),EAutomationExpectedErrorFlags::Contains,1);
    for (int32 Offset=FirstPrefix;Offset<DamagedAudio.Num();Offset+=257) Receive(Damaged.TurnId,DamagedAudio,Offset,257);
    TestTrue(TEXT("坏包只停止当前句的声音，完整台词仍显示"),P->GetDisplayedSpeech().bActive &&
        P->GetDisplayedSpeech().TurnId==Damaged.TurnId && P->GetDisplayedSpeech().Text==Damaged.Text && !P->GetDisplayedSpeech().bRecordedVoice);
    TestNull(TEXT("坏句子的音频队列已释放"),P->RecordedWave.Get());
    TestTrue(TEXT("音频回退不会让已经显示的字消失"),P->GetDisplayedSpeechProgress()>=ProgressBeforeDamage);
    TestFalse(TEXT("单句音频损坏不取消服务器确认的整轮回复"),P->CancelledVoiceActions.Contains(Damaged.ActionId));
    if (!TestEqual(TEXT("坏句子和下一句都保留在本机队列"),P->LocalVoices.Num(),2)) return false;
    Receive(Damaged.TurnId,DamagedAudio,0,FirstPrefix);
    TestTrue(TEXT("坏句子的晚到补发不会重新创建声音"),!P->LocalVoices[0].Decoder && P->LocalVoices[0].Pcm.IsEmpty());
    P->LocalVoices[0].TextStartedAt=FPlatformTime::Seconds()-P->LocalVoices[0].State.Duration-.1;
    P->PlayRecordedVoice();P->PlayRecordedVoice();
    TestEqual(TEXT("文字显示结束后继续下一句"),P->GetDisplayedSpeech().TurnId,AfterDamaged.TurnId);
    Heard.Reset();ReadAudio();
    TestTrue(TEXT("后一句完整声音仍进入实际 PCM 队列"),Heard==SecondDecoded);
    P->StopStreamSpeech_Implementation(Damaged.ActionId,false);
    TestTrue(TEXT("发生过音频回退后，主动打断仍停止整轮文字和声音"),P->LocalVoices.IsEmpty() &&
        !P->GetDisplayedSpeech().bActive && P->RecordedWave==nullptr);
    P->StartStreamSpeech_Implementation(Damaged);P->StartStreamSpeech_Implementation(AfterDamaged);
    Receive(AfterDamaged.TurnId,SecondEncoded,0,SecondEncoded.Num());
    TestTrue(TEXT("主动打断后旧句子和晚到声音仍不能恢复播放"),P->LocalVoices.IsEmpty());
    FDeliveryNpcSpeechState Interrupted=First;Interrupted.ActionId=FGuid::NewGuid();Interrupted.TurnId=FGuid::NewGuid();
    P->StartStreamSpeech_Implementation(Interrupted);Receive(Interrupted.TurnId,FirstEncoded,0,FirstPrefix);P->PlayRecordedVoice();
    P->StopStreamSpeech_Implementation(Interrupted.ActionId,false);
    TestFalse(TEXT("打断立即停止本机字幕"),P->GetDisplayedSpeech().bActive);
    TestNull(TEXT("打断释放音频队列"),P->RecordedWave.Get());
    P->StartStreamSpeech_Implementation(Interrupted);Receive(Interrupted.TurnId,FirstEncoded,0,FirstPrefix);
    TestTrue(TEXT("晚到的已取消语音不会再次播放"),P->LocalVoices.IsEmpty());
    FDeliveryNpcSpeechState OldReply=First;OldReply.ActionId=FGuid::NewGuid();OldReply.TurnId=FGuid::NewGuid();
    FDeliveryNpcSpeechState NewReply=First;NewReply.ActionId=FGuid::NewGuid();NewReply.TurnId=FGuid::NewGuid();
    P->StartStreamSpeech_Implementation(OldReply);Receive(OldReply.TurnId,FirstEncoded,0,FirstPrefix);P->PlayRecordedVoice();
    P->StartStreamSpeech_Implementation(NewReply);Receive(NewReply.TurnId,FirstEncoded,0,FirstPrefix);
    P->StopStreamSpeech_Implementation(NewReply.ActionId,true);
    TestTrue(TEXT("插话同时取消前一轮尾音和新一轮待播句子"),P->LocalVoices.IsEmpty() && !P->GetDisplayedSpeech().bActive);
    TestFalse(TEXT("收到服务器停止通知后台词状态立即结束"),P->GetSpeech().bActive);
    P->StartStreamSpeech_Implementation(OldReply);P->StartStreamSpeech_Implementation(NewReply);
    TestTrue(TEXT("两轮取消后的晚到台词都不会重新播放"),P->LocalVoices.IsEmpty());
    TestFalse(TEXT("晚到的开始通知不会改回正在说话"),P->GetSpeech().bActive);
    P->Speech=NewReply;P->OnRep_Speech();
    TestFalse(TEXT("晚到的属性同步保留已取消结果"),P->GetSpeech().bActive);
    FDeliveryNpcSpeechState MixedAudio=First;MixedAudio.ActionId=FGuid::NewGuid();MixedAudio.TurnId=FGuid::NewGuid();
    FDeliveryNpcSpeechState TextOnly=MixedAudio;TextOnly.TurnId=FGuid::NewGuid();TextOnly.Sequence=1;TextOnly.bRecordedVoice=false;TextOnly.Duration=.2f;
    P->StartStreamSpeech_Implementation(MixedAudio);Receive(MixedAudio.TurnId,FirstEncoded,0,FirstPrefix);P->PlayRecordedVoice();
    P->StartStreamSpeech_Implementation(TextOnly);P->Speech=TextOnly;P->OnRep_Speech();
    TestEqual(TEXT("无声音的下一句排在当前语音之后"),P->GetDisplayedSpeech().TurnId,MixedAudio.TurnId);
    for (int32 Offset=FirstPrefix;Offset<FirstEncoded.Num();Offset+=257) Receive(MixedAudio.TurnId,FirstEncoded,Offset,257);
    P->PlayRecordedVoice();Heard.Reset();ReadAudio();
    TestTrue(TEXT("下一句合成失败仍保留上一句完整声音"),Heard==FirstDecoded);
    P->LocalVoices[0].DrainedAt=FPlatformTime::Seconds()-.2;P->PlayRecordedVoice();P->PlayRecordedVoice();
    TestEqual(TEXT("上一句结束后显示无声音的台词"),P->GetDisplayedSpeech().TurnId,TextOnly.TurnId);
    TestFalse(TEXT("文字回退明确标记为无合成声音"),P->GetDisplayedSpeech().bRecordedVoice);
    P->LocalVoices[0].TextStartedAt=FPlatformTime::Seconds()-.3;P->PlayRecordedVoice();
    TestTrue(TEXT("文字句正常结束后清空待播内容"),P->LocalVoices.IsEmpty());
    FDeliveryNpcSpeechState Expired=First;Expired.ActionId=FGuid::NewGuid();Expired.TurnId=FGuid::NewGuid();Expired.StartTime=-10;
    P->AcceptLocalSpeech(Expired,true);
    TestTrue(TEXT("后来进入附近时略过已经结束的句子"),P->LocalVoices.IsEmpty());
    return true;
}
#endif
