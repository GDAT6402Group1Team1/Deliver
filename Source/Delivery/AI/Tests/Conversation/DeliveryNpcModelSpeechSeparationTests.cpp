#include "Misc/AutomationTest.h"
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/Speech/DeliveryNpcVoiceCodec.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcModelSpeechSeparationTest,"Delivery.AI.ModelSpeechSeparation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcModelSpeechSeparationTest::RunTest(const FString&)
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
    auto* Target=World->SpawnActor<ADeliveryNpcCharacter>(FVector(500,0,90),FRotator::ZeroRotator,Spawn);
    auto* Agent=Npc->Agent.Get(); auto* Presentation=Npc->Presentation.Get();
    auto* Gateway=World->GetSubsystem<UDeliveryAiGatewaySubsystem>();
    Npc->SetActorTickEnabled(false); Target->SetActorTickEnabled(false);
    FDeliveryNpcDecisionRequest Base;
    if (!TestTrue(TEXT("从正式入口取得本世界的请求身份"),Agent->BuildDecisionRequest(Gateway->GetWorldEpoch(),Base))) return false;
    const FGuid ActionId=FGuid::NewGuid();
    Agent->PublicState.ActionId=ActionId; Agent->PublicState.Action=EDeliveryNpcAction::Fight;
    Agent->PublicState.Status=EDeliveryNpcActionStatus::Running; Agent->PublicState.Target=Target;
    Agent->PublicState.bStreamSpeech=true;
    int32 PlanRevision=Base.PlanRevision;

    // 本测试检查已开始交战后的请求与语音顺序，拳头命中另由 Fight 测试覆盖
    auto AddRequest=[&](bool bStartSpeech)
    {
        auto Request=Base; Request.RequestId=FGuid::NewGuid(); Request.StreamProtocolVersion=2;
        Request.PlanRevision=++PlanRevision;
        Settings->bEnabled=true;
        const bool Accepted=Gateway->Enqueue(Agent,Request);
        Settings->bEnabled=false;
        TestTrue(TEXT("同 NPC 的模型完成后可以加入下一份请求"),Accepted);
        if (!Accepted) return FGuid();
        Agent->ActiveRequest=Request; Agent->bDecisionPending=true;
        auto& Reply=Agent->Replies.Add(Request.RequestId);
        Reply.ReplyId=FGuid::NewGuid(); Reply.ActionId=ActionId; Reply.Action=EDeliveryNpcAction::Fight;
        Reply.Target=Target; Reply.bHadTarget=true;
        Reply.PlanRevision=Request.PlanRevision;
        Agent->PublicState.ReplyId=Reply.ReplyId;
        Agent->PublicState.PlanRevision=Request.PlanRevision;
        Gateway->Pending[Request.RequestId].bHasProposal=true;
        if (bStartSpeech) Agent->StartReplyPresentation(Reply);
        return Request.RequestId;
    };
    auto Event=[&](FGuid Id,const TCHAR* Type,int32 Number)
    {
        auto Json=MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("type"),Type);
        Json->SetStringField(TEXT("request_id"),Id.ToString(EGuidFormats::DigitsWithHyphensLower));
        Json->SetNumberField(TEXT("sentences"),Number);
        return Json;
    };
    auto Send=[&](FGuid Id,const TSharedRef<FJsonObject>& Json)
    {
        FString Line; FJsonSerializer::Serialize(Json,TJsonWriterFactory<>::Create(&Line));
        Gateway->ReadStream(Id,Line);
    };
    auto Sentence=[&](FGuid Id,int32 Sequence,const TCHAR* Text)
    {
        auto Json=Event(Id,TEXT("sentence"),0);
        Json->SetNumberField(TEXT("sequence"),Sequence); Json->SetStringField(TEXT("text"),Text);
        Json->SetStringField(TEXT("emotion"),TEXT("Angry")); Json->SetNumberField(TEXT("sample_rate"),24000);
        Json->SetStringField(TEXT("pcm"),TEXT("")); Send(Id,Json);
    };
    auto FinishHttp=[&](FGuid Id,int32 Count)
    {
        Send(Id,Event(Id,TEXT("done"),Count));
        if (auto* Entry=Gateway->Pending.Find(Id))
        {
            Entry->bHttpFinished=true; Entry->bHttpSucceeded=true;
            Gateway->FinishStreamRequest(Id);
        }
    };

    const FGuid First=AddRequest(true);
    if (!First.IsValid()) return false;
    const FGuid FirstReply=Agent->Replies[First].ReplyId;
    Send(First,Event(First,TEXT("model_done"),2));
    TestFalse(TEXT("模型完成后不再等待模型"),Agent->IsDecisionPending());
    TestTrue(TEXT("声音尚未结束时仍保存原网络请求"),Gateway->Pending.Contains(First) && Gateway->Pending[First].bModelFinished);
    Sentence(First,0,TEXT("我没说你怂啊，别血口喷人！"));
    Sentence(First,1,TEXT("这句还没有说出口，换回复后不该记住。"));
    Presentation->TickSentences();
    TestTrue(TEXT("model_done 之后到达的声音仍能开始播放"),Presentation->Speech.bActive);
    const FGuid FirstTurn=Presentation->Speech.TurnId;

    const FGuid Second=AddRequest(true);
    if (!Second.IsValid()) return false;
    const FGuid SecondReply=Agent->Replies[Second].ReplyId;
    TestEqual(TEXT("同动作新回复只进入待播位置"),Presentation->PendingReply.ReplyId,SecondReply);
    TestEqual(TEXT("新模型请求不会截断当前句"),Presentation->Speech.TurnId,FirstTurn);
    FinishHttp(First,2);
    TestTrue(TEXT("旧 HTTP 结束后新模型仍在等待"),Agent->IsDecisionPending());
    TestEqual(TEXT("旧 HTTP 完成不能清掉新请求"),Agent->ActiveRequest.RequestId,Second);
    Send(Second,Event(Second,TEXT("model_done"),1));
    Sentence(Second,0,TEXT("这轮待播内容稍后会被替换。"));

    const FGuid Third=AddRequest(true);
    if (!Third.IsValid()) return false;
    const FGuid ThirdReply=Agent->Replies[Third].ReplyId;
    TestEqual(TEXT("待播位置只留下最新回复"),Presentation->PendingReply.ReplyId,ThirdReply);
    TestFalse(TEXT("替换待播回复会取消对应网络请求"),Gateway->Pending.Contains(Second));
    TestFalse(TEXT("替换待播回复会移除对应记录"),Agent->Replies.Contains(Second));
    TestEqual(TEXT("取消旧待播任务不清掉新模型请求"),Agent->ActiveRequest.RequestId,Third);
    Agent->FinishReply(Second,false);
    TestEqual(TEXT("被替换任务晚到的失败也不影响新请求"),Agent->ActiveRequest.RequestId,Third);
    Sentence(Third,0,TEXT("没说就没说嘛，你这是什么态度！"));
    Send(Third,Event(Third,TEXT("model_done"),1));
    FinishHttp(Third,1);

    Presentation->Speech.StartTime=Presentation->ServerTime()-Presentation->Speech.Duration-1;
    Presentation->TickSentences();
    TestEqual(TEXT("当前完整句结束后再切换新回复"),Presentation->ReplyAction,ThirdReply);
    TestFalse(TEXT("切换后待播位置已空"),Presentation->PendingReply.ReplyId.IsValid());
    TestTrue(TEXT("已经说完的原句仍写入记忆"),Agent->Evidence.ContainsByPredicate([](const auto& Item)
        { return Item.Kind==TEXT("SelfSpeech") && Item.Description.Contains(TEXT("别血口喷人")); }));
    TestFalse(TEXT("原回复中尚未播放的句子没有写入记忆"),Agent->Evidence.ContainsByPredicate([](const auto& Item)
        { return Item.Description.Contains(TEXT("这句还没有说出口")) || Item.Description.Contains(TEXT("这轮待播内容")); }));

    // 可靠通知按同一 Actor 的顺序到达；晚几帧的客户端仍能把当前完整句播完
    Presentation->LocalVoices.Reset();
    FDeliveryNpcLocalVoice Local; Local.State.ActionId=FirstReply; Local.State.TurnId=FirstTurn; Local.State.bActive=true;
    Presentation->LocalVoices.Add(Local); Presentation->DisplayedSpeech=Local.State;
    Presentation->FinishReplyAfterSentence_Implementation(FirstReply);
    TestEqual(TEXT("普通续接保留客户端当前句"),Presentation->LocalVoices.Num(),1);
    TestTrue(TEXT("普通续接不会立即关闭客户端字幕"),Presentation->DisplayedSpeech.bActive);
    Presentation->StopStreamSpeech_Implementation(ThirdReply,true);
    TestTrue(TEXT("全局结束的停止通知会清掉客户端旧尾音"),Presentation->LocalVoices.IsEmpty());
    TestFalse(TEXT("立即停止同时清掉字幕"),Presentation->DisplayedSpeech.bActive);

    Agent->CancelPlan(TEXT("NextProtocolCase"));
    Agent->PublicState.ActionId=ActionId; Agent->PublicState.Action=EDeliveryNpcAction::Fight;
    Agent->PublicState.Status=EDeliveryNpcActionStatus::Running; Agent->PublicState.Target=Target;
    const FGuid Silent=AddRequest(false);
    Send(Silent,Event(Silent,TEXT("model_done"),0));
    TestTrue(TEXT("零台词完成不会新建播放任务"),!Presentation->ReplyAction.IsValid() && !Presentation->PendingReply.ReplyId.IsValid());
    TestTrue(TEXT("零台词完成仍可用于真实动作回应"),Presentation->IsReplyWithoutSpeech(Agent->PublicState.GetReplyId()));
    Send(Silent,Event(Silent,TEXT("model_done"),0));
    TestFalse(TEXT("重复 model_done 被拒绝而不会再次完成模型请求"),Gateway->Pending.Contains(Silent));

    // 真实 done/HTTP 结束链必须清理环境零台词，不能等一个永远不会发生的播放回调
    for (const auto SilentAction:{EDeliveryNpcAction::Fight,EDeliveryNpcAction::Flee,EDeliveryNpcAction::Wait})
    {
        Agent->PublicState.Action=SilentAction;
        Agent->PublicState.ServerStartTime=4; Agent->PublicState.Duration=6;
        Agent->ActionDeadline=World->GetTimeSeconds()+8;
        const FGuid Ambient=AddRequest(false);
        const auto BeforeAmbient=Agent->GetPublicState();
        const double Deadline=Agent->ActionDeadline;
        TestEqual(TEXT("环境回复沿用兼容的 v4 请求"),Agent->ActiveRequest.RequestedCapabilityVersion,4);
        Send(Ambient,Event(Ambient,TEXT("model_done"),0));
        TestTrue(TEXT("只有模型完成时仍等待传输结束"),Agent->Replies.Contains(Ambient));
        FinishHttp(Ambient,0);
        TestFalse(TEXT("环境零句传输结束后移除回复记录"),Agent->Replies.Contains(Ambient));
        TestFalse(TEXT("环境零句传输结束后移除网络请求"),Gateway->Pending.Contains(Ambient));
        Agent->FinishReply(Ambient,true,0);
        Sentence(Ambient,0,TEXT("终态之后迟到的句子不能重新播放"));
        TestFalse(TEXT("重复结束与迟到句子不会复活环境回复"),Agent->Replies.Contains(Ambient) || Presentation->HasPendingSpeech());
        const auto AfterAmbient=Agent->GetPublicState();
        TestTrue(TEXT("清理环境回复不停止或重启原活动"),AfterAmbient.ActionId==BeforeAmbient.ActionId &&
            AfterAmbient.PlanRevision==BeforeAmbient.PlanRevision && AfterAmbient.Action==BeforeAmbient.Action &&
            AfterAmbient.Status==BeforeAmbient.Status && AfterAmbient.Target==BeforeAmbient.Target &&
            AfterAmbient.ServerStartTime==BeforeAmbient.ServerStartTime && AfterAmbient.Duration==BeforeAmbient.Duration &&
            Agent->ActionDeadline==Deadline);
    }
    TestTrue(TEXT("连续环境零句不会累积回复记录"),Agent->Replies.IsEmpty());
    Agent->PublicState.Action=EDeliveryNpcAction::Fight;
    const FGuid Speaking=AddRequest(true);
    Send(Speaking,Event(Speaking,TEXT("model_done"),1));
    Sentence(Speaking,0,TEXT("这句已经开始，环境零台词不能打断。"));
    Presentation->TickSentences();
    const FGuid SpeakingTurn=Presentation->Speech.TurnId;
    TestTrue(TEXT("环境零句前有实际正在播放的句子"),Presentation->Speech.bActive);
    const FGuid SilentAlongside=AddRequest(false);
    Send(SilentAlongside,Event(SilentAlongside,TEXT("model_done"),0));
    FinishHttp(SilentAlongside,0);
    TestTrue(TEXT("零句清理保留另一回复的播放和归属"),Presentation->Speech.bActive &&
        Presentation->Speech.TurnId==SpeakingTurn && Agent->Replies.Contains(Speaking));
    TestFalse(TEXT("仅移除本次环境零句记录"),Agent->Replies.Contains(SilentAlongside));
    Agent->CancelPlan(TEXT("NextSilentPlayerCase"));
    Agent->PublicState.ActionId=ActionId; Agent->PublicState.Action=EDeliveryNpcAction::Wait;
    Agent->PublicState.Status=EDeliveryNpcActionStatus::Running;
    {
        auto* ReceiptOwner=World->SpawnActor<APlayerController>();
        ReceiptOwner->PlayerState=World->SpawnActor<APlayerState>();
        auto* ReceiptPawn=World->SpawnActor<APawn>(); ReceiptOwner->Possess(ReceiptPawn);
        const FGuid PlayerSilent=AddRequest(false),MessageId=FGuid::NewGuid();
        auto& PlayerReply=Agent->Replies[PlayerSilent];
        PlayerReply.RequesterPC=ReceiptOwner; PlayerReply.RequesterPlayerState=ReceiptOwner->PlayerState;
        PlayerReply.MessageId=MessageId;
        Agent->ActiveRequest.bPlayerRequest=true; Agent->ActiveRequest.PlayerMessageId=MessageId;
        Agent->ActiveRequest.RequesterPC=ReceiptOwner; Agent->ActiveRequest.RequesterPlayerState=ReceiptOwner->PlayerState;
        Agent->bPlayerRequest=true; Agent->PlayerReplyMessage=MessageId; Agent->PlayerReplyController=ReceiptOwner;
        Agent->PublicState.ReplyToEventId=MessageId; Agent->PublicState.SpeechTarget=ReceiptPawn;
        Send(PlayerSilent,Event(PlayerSilent,TEXT("model_done"),0));
        FinishHttp(PlayerSilent,0);
        TestTrue(TEXT("玩家零句传输结束后仍保留真实动作回执"),Agent->Replies.Contains(PlayerSilent) &&
            Agent->GetReplyMessage(ReceiptOwner)==MessageId && Agent->GetPublicState().Status==EDeliveryNpcActionStatus::Running);
        TestTrue(TEXT("玩家零句等待的动作仍可完成"),Agent->TryFinishAction(ActionId,EDeliveryNpcActionStatus::Succeeded,NAME_None));
        TestFalse(TEXT("玩家动作完成后才清理零句记录"),Agent->Replies.Contains(PlayerSilent) || Agent->IsPlayerReplyActive(ReceiptOwner));
    }
    Agent->PublicState.Action=EDeliveryNpcAction::Fight;
    Agent->PublicState.Status=EDeliveryNpcActionStatus::Running; Agent->PublicState.Target=Target;

    const FGuid LateFirst=AddRequest(false);
    Send(LateFirst,Event(LateFirst,TEXT("model_done"),1));
    const FGuid Newer=AddRequest(true);
    const FGuid NewerReply=Agent->Replies[Newer].ReplyId;
    Sentence(LateFirst,0,TEXT("很晚才到的旧回复第一句。"));
    TestFalse(TEXT("较新台词准备后，迟到的旧首句被取消"),Gateway->Pending.Contains(LateFirst));
    TestEqual(TEXT("迟到旧首句不会倒过来替换新台词"),Presentation->ReplyAction,NewerReply);
    TestEqual(TEXT("取消迟到首句不结束当前模型请求"),Agent->ActiveRequest.RequestId,Newer);
    Send(Newer,Event(Newer,TEXT("model_done"),1));
    Agent->CancelPlan(TEXT("NextInterruptCase"));
    Agent->PublicState.ActionId=ActionId; Agent->PublicState.Action=EDeliveryNpcAction::Fight;
    Agent->PublicState.Status=EDeliveryNpcActionStatus::Running; Agent->PublicState.Target=Target;

    auto* SpeakerController=World->SpawnActor<APlayerController>();
    SpeakerController->PlayerState=World->SpawnActor<APlayerState>();
    auto* Speaker=World->SpawnActor<APawn>();
    SpeakerController->Possess(Speaker);
    const FGuid Cancelled=AddRequest(true);
    // 传输测试直接构造已批准回复，同时保存实际请求者，不用模型的说话对象代替归属
    Agent->Replies[Cancelled].RequesterPC=SpeakerController;
    Agent->Replies[Cancelled].RequesterPlayerState=SpeakerController->PlayerState;
    Agent->Replies[Cancelled].MessageId=FGuid::NewGuid();
    Send(Cancelled,Event(Cancelled,TEXT("model_done"),1));
    TestTrue(TEXT("同一真实玩家的新一轮可以开始"),Agent->BeginListening(Speaker,FGuid::NewGuid()));
    TestTrue(TEXT("新玩家发言清掉旧语音记录"),Agent->Replies.IsEmpty());
    TestFalse(TEXT("新玩家发言取消 model_done 后仍在生成的声音"),Gateway->Pending.Contains(Cancelled));
    TestFalse(TEXT("新玩家发言同时清掉播放与待播状态"),Presentation->HasPendingSpeech());
    TestEqual(TEXT("插话保留当前活动编号"),Agent->GetPublicState().ActionId,ActionId);
    TestEqual(TEXT("插话保留活动运行状态"),Agent->GetPublicState().Status,EDeliveryNpcActionStatus::Running);

    Agent->CancelPlan(TEXT("NextTargetCase"));
    Agent->PublicState.ActionId=ActionId; Agent->PublicState.Action=EDeliveryNpcAction::Fight;
    Agent->PublicState.Status=EDeliveryNpcActionStatus::Running; Agent->PublicState.Target=Target;
    const FGuid LostTarget=AddRequest(true);
    Send(LostTarget,Event(LostTarget,TEXT("model_done"),1));
    Target->Destroy();
    Presentation->TickSentences();
    TestTrue(TEXT("活动目标销毁不取消独立回复"),Presentation->HasPendingSpeech());
    TestTrue(TEXT("活动目标销毁保留仍在生成声音的 HTTP"),Gateway->Pending.Contains(LostTarget));
    Sentence(LostTarget,0,TEXT("人已经走了，我还是把这句话说完。"));
    FinishHttp(LostTarget,1);
    Presentation->TickSentences();
    TestTrue(TEXT("目标消失后本轮回复仍能开始"),Presentation->Speech.bActive);
    Presentation->Speech.StartTime=Presentation->ServerTime()-Presentation->Speech.Duration-1;
    Presentation->TickSentences();
    TestFalse(TEXT("独立回复实际播放结束后清理"),Presentation->HasPendingSpeech());
    TestFalse(TEXT("完整结束后移除独立回复记录"),Agent->Replies.Contains(LostTarget));

    // 模拟玩家中途走近，只收到半句后主机已切换回复，再通过正式补发接口收齐声音
    auto* ViewNpc=World->SpawnActor<ADeliveryNpcCharacter>(FVector(1000,0,90),FRotator::ZeroRotator,Spawn);
    ViewNpc->SetActorTickEnabled(false); ViewNpc->SetRole(ROLE_SimulatedProxy);
    auto* View=ViewNpc->Presentation.Get();
    TArray<uint8> Pcm,Encoded,Part,Decoded;
    Pcm.SetNumUninitialized(24000);
    for (int32 Index=0;Index<Pcm.Num();++Index) Pcm[Index]=uint8(Index%251);
    FString Error;
    FDeliveryNpcVoiceStreamEncoder Encoder;
    if (!TestTrue(TEXT("补发检查使用正式分段编码器"),Encoder.Start(24000,Encoded,Error)) ||
        !TestTrue(TEXT("为补发检查编码声音"),Encoder.Append(Pcm,Part,Error))) return false;
    Encoded.Append(Part);
    if (!TestTrue(TEXT("生成完整句末数据"),Encoder.Finish(Part,Error))) return false;
    Encoded.Append(Part);
    FDeliveryNpcVoiceDecoder Reference;
    int32 DecodedOffset=0;
    if (!TestTrue(TEXT("生成完整声音的解码参考"),Reference.TryDecodeAvailable(Encoded,DecodedOffset,Decoded,Error))) return false;
    FDeliveryNpcSpeechState Tail;
    Tail.ActionId=FGuid::NewGuid(); Tail.TurnId=FGuid::NewGuid(); Tail.bActive=true; Tail.bRecordedVoice=true;
    Tail.Text=TEXT("你先把话说完，我在这儿听着呢。"); Tail.Emotion=TEXT("Calm");
    Tail.SampleRate=24000; Tail.AudioFormatVersion=DeliveryNpcVoice::StreamingFormatVersion;
    View->StartStreamSpeech_Implementation(Tail);
    const int32 Prefix=FMath::Min(128,Encoded.Num()/2);
    Part.Reset(); Part.Append(Encoded.GetData(),Prefix);
    View->ReceiveAudio_Implementation(Tail.TurnId,0,0,Part);
    FDeliveryNpcSpeechState Skipped=Tail; Skipped.TurnId=FGuid::NewGuid(); Skipped.Sequence=1;
    View->StartStreamSpeech_Implementation(Skipped);

    Presentation->ReplyAction=Tail.ActionId; Presentation->Speech=Tail; Presentation->Speech.bActive=false;
    auto& Saved=Presentation->ServerVoices.Add(Tail.TurnId);
    Saved.Encoded=Encoded; Saved.Pcm=Pcm; Saved.ExpiresAt=FPlatformTime::Seconds()+5;
    const double Expiry=Saved.ExpiresAt;
    const FGuid Unfinished=FGuid::NewGuid();
    Presentation->ServerVoices.Add(Unfinished).Encoded=Encoded;
    Presentation->ServerVoices[Unfinished].bComplete=false;
    for (int32 Index=0;Index<4;++Index)
    {
        auto& Older=Presentation->ServerVoices.Add(FGuid::NewGuid());
        Older.Encoded=Encoded; Older.ExpiresAt=Expiry-4+Index;
    }
    Presentation->CancelSpeech(true);
    View->FinishReplyAfterSentence_Implementation(Tail.ActionId);
    TestEqual(TEXT("普通续接最多保留三句已完成声音"),Presentation->ServerVoices.Num(),3);
    TestFalse(TEXT("尚未完成的旧声音不保留"),Presentation->ServerVoices.Contains(Unfinished));
    if (!TestTrue(TEXT("刚完成的当前句仍可补取"),Presentation->ServerVoices.Contains(Tail.TurnId))) return false;
    TestEqual(TEXT("普通续接不延长原来的五秒缓存期限"),Presentation->ServerVoices[Tail.TurnId].ExpiresAt,Expiry);
    if (!TestEqual(TEXT("客户端只保留当前句，不继续旧回复的下一句"),View->LocalVoices.Num(),1)) return false;
    const double ReceiveUntil=View->LocalVoices[0].ReceiveUntil;
    View->FinishReplyAfterSentence_Implementation(Tail.ActionId);
    TestEqual(TEXT("重复句末通知不会延长补发时间"),View->LocalVoices[0].ReceiveUntil,ReceiveUntil);
    int32 Offset=Prefix,Total=0;
    while (Offset<Encoded.Num())
    {
        if (!TestTrue(TEXT("普通续接后仍可从服务器取得缺少的数据"),
            Presentation->GetSpeechAudioChunk(Tail.TurnId,Offset,2048,Part,Total))) return false;
        View->ReceiveAudio_Implementation(Tail.TurnId,Offset,Total,Part);
        Offset+=Part.Num();
    }
    TestTrue(TEXT("补齐后的实际解码声音与完整句一致"),View->LocalVoices[0].Pcm==Decoded);
    TestTrue(TEXT("补发句末数据能够确认完整声音"),View->LocalVoices[0].State.bAudioComplete && View->LocalVoices[0].Decoder->IsComplete());
    View->StartStreamSpeech_Implementation(Skipped);
    TestEqual(TEXT("收到补发也不会复活其他旧句"),View->LocalVoices.Num(),1);
    View->FinishLocalSentence();
    FDeliveryNpcSpeechState Expired=Tail; Expired.ActionId=FGuid::NewGuid(); Expired.TurnId=FGuid::NewGuid();
    View->StartStreamSpeech_Implementation(Expired);
    Part.Reset(); Part.Append(Encoded.GetData(),Prefix);
    View->ReceiveAudio_Implementation(Expired.TurnId,0,0,Part);
    View->FinishReplyAfterSentence_Implementation(Expired.ActionId);
    if (!TestEqual(TEXT("过期检查仍从真实的半句接收状态开始"),View->LocalVoices.Num(),1)) return false;
    View->LocalVoices[0].ReceiveUntil=FPlatformTime::Seconds()-1;
    Part.Reset(); Part.Append(Encoded.GetData()+Prefix,FMath::Min(2048,Encoded.Num()-Prefix));
    View->ReceiveAudio_Implementation(Expired.TurnId,Prefix,Encoded.Num(),Part);
    View->FinishStreamAudio_Implementation(Expired.TurnId,Pcm.Num(),Encoded.Num(),true);
    TestEqual(TEXT("客户端五秒补发时间结束后不再追加声音"),View->LocalVoices[0].Encoded.Num(),Prefix);
    TestFalse(TEXT("过期的句末通知也被拒绝"),View->LocalVoices[0].State.bAudioComplete);
    Presentation->ServerVoices[Tail.TurnId].ExpiresAt=FPlatformTime::Seconds()-1;
    TestFalse(TEXT("服务器不补发已过期的声音"),Presentation->GetSpeechAudioChunk(Tail.TurnId,0,2048,Part,Total));
    Presentation->TrimVoiceCache();
    TestFalse(TEXT("过期声音从服务器缓存移除"),Presentation->ServerVoices.Contains(Tail.TurnId));
    Presentation->CancelSpeech(); View->StopStreamSpeech_Implementation(Tail.ActionId,true);
    TestTrue(TEXT("玩家插话或倒地的立即取消清空全部补发与播放"),Presentation->ServerVoices.IsEmpty() && View->LocalVoices.IsEmpty());
    View->StartStreamSpeech_Implementation(Tail);
    TestTrue(TEXT("立即取消后，保留过的旧句也不能重新开始"),View->LocalVoices.IsEmpty());
    return true;
}
#endif
