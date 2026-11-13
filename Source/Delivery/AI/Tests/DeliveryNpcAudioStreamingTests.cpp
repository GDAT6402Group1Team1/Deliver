#include "Misc/AutomationTest.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Presentation/DeliveryNpcBodyAnimInstance.h"
#include "AI/Presentation/DeliveryNpcVoiceCodec.h"
#include "AI/BehaviorTree/BTTask_DeliveryExecuteNpcAction.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Async/TaskGraphInterfaces.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/DataTable.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Base64.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundWaveProcedural.h"

namespace
{
    TArray<uint8> MakeNpcStreamTestPcm(int32 Samples)
    {
        TArray<uint8> Pcm;
        Pcm.SetNumUninitialized(Samples * 2);
        for (int32 Index = 0; Index < Samples; ++Index)
        {
            const int16 Sample = int16(9000 * FMath::Sin(2 * PI * 317 * Index / 24000.0));
            Pcm[Index * 2] = uint8(Sample);
            Pcm[Index * 2 + 1] = uint8(uint16(Sample) >> 8);
        }
        return Pcm;
    }

    FString NpcStreamTestJson(const TSharedRef<FJsonObject>& Json)
    {
        FString Text;
        FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text));
        return Text;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcAudioStreamingTest, "Delivery.AI.AudioStreaming",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcAudioStreamingTest::RunTest(const FString&)
{
    auto* Settings = GetMutableDefault<UDeliveryAiSettings>();
    const bool Enabled = Settings->bEnabled;
    const bool StreamReplies = Settings->bStreamReplies;
    const bool StreamAudio = Settings->bStreamSentenceAudio;
    Settings->bEnabled = false;
    Settings->bStreamReplies = true;
    Settings->bStreamSentenceAudio = true;
    TArray<UWorld*> Worlds;
    ON_SCOPE_EXIT
    {
        for (UWorld* World : Worlds)
        {
            World->EndPlay(EEndPlayReason::Quit);
            GEngine->DestroyWorldContext(World);
            World->DestroyWorld(false);
        }
        Settings->bEnabled = Enabled;
        Settings->bStreamReplies = StreamReplies;
        Settings->bStreamSentenceAudio = StreamAudio;
    };
    auto MakeWorld = [&]()
    {
        const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
        UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
        Worlds.Add(World);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
        World->GetWorldSettings()->NotifyBeginPlay();
        return World;
    };
    UWorld* ServerWorld = MakeWorld();
    UWorld* ClientWorld = MakeWorld();
    auto* ServerNpc = ServerWorld->SpawnActor<ADeliveryNpcCharacter>();
    auto* ClientNpc = ClientWorld->SpawnActor<ADeliveryNpcCharacter>();
    auto* ServerPlayer = ServerWorld->SpawnActor<AActor>();
    auto* ClientPlayer = ClientWorld->SpawnActor<AActor>();
    ServerNpc->SetActorTickEnabled(false);
    ClientNpc->SetActorTickEnabled(false);
    ServerNpc->Agent->SetComponentTickEnabled(false);
    ClientNpc->Agent->SetComponentTickEnabled(false);
    ClientNpc->SetRole(ROLE_SimulatedProxy);
    auto* Agent = ServerNpc->Agent.Get();
    auto* Host = ServerNpc->Presentation.Get();
    auto* Client = ClientNpc->Presentation.Get();
    // 这里故意用较短缓冲制造缺音，检查读空、补足和续播；不修改正式人物配置
    Host->VoiceSet=NewObject<UDeliveryNpcVoiceSet>(ServerNpc);
    Client->VoiceSet=NewObject<UDeliveryNpcVoiceSet>(ClientNpc);
    Host->VoiceSet->StreamingStartBufferSeconds=.64f;
    Client->VoiceSet->StreamingStartBufferSeconds=.64f;
    // 使用正式姿势选择代码读取两端各自的播放器，不人为设置动画阶段
    auto* HostBody = NewObject<UDeliveryNpcBodyAnimInstance>(ServerNpc->GetMesh());
    auto* ClientBody = NewObject<UDeliveryNpcBodyAnimInstance>(ClientNpc->GetMesh());
    HostBody->NativeInitializeAnimation(); ClientBody->NativeInitializeAnimation();
    using EBodyPose = UDeliveryNpcBodyAnimInstance::EPose;
    auto CheckBodyPose = [&](EBodyPose Expected, const TCHAR* Message)
    {
        HostBody->NativeUpdateAnimation(.02f); ClientBody->NativeUpdateAnimation(.02f);
        TestEqual(FString::Printf(TEXT("主机：%s"), Message), HostBody->PoseIndex, static_cast<int32>(Expected));
        TestEqual(FString::Printf(TEXT("客户端：%s"), Message), ClientBody->PoseIndex, static_cast<int32>(Expected));
    };
    auto* Gateway = ServerWorld->GetSubsystem<UDeliveryAiGatewaySubsystem>();
    if (!TestNotNull(TEXT("服务器世界具有正式请求入口"), Gateway)) return false;
    TestTrue(TEXT("主机和模拟客户端使用独立世界与播放组件"), Host != Client && Host->GetWorld() != Client->GetWorld());

    FDeliveryNpcDecisionRequest Request;
    FGuid MessageId;
    TMap<FGuid, int32> SentBytes;
    TSet<FGuid> StartedTurns, EndedTurns, DisplayedTurns;
    auto Event = [&](const TCHAR* Type)
    {
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("type"), Type);
        Json->SetStringField(TEXT("request_id"), Request.RequestId.ToString(EGuidFormats::DigitsWithHyphensLower));
        return Json;
    };
    auto Feed = [&](const TSharedRef<FJsonObject>& Json)
    {
        Gateway->ReadStream(Request.RequestId, NpcStreamTestJson(Json));
    };
    auto BeginReply = [&](bool bIncremental)
    {
        Agent->CancelPlan(TEXT("TestNextReply"));
        Client->StopStreamSpeech_Implementation(FGuid(), true);
        Settings->bStreamSentenceAudio = bIncremental;
        MessageId = FGuid::NewGuid();
        FDeliveryNpcObservation Message;
        Message.EventId = MessageId; Message.Kind = TEXT("PlayerSpeech");
        Message.Description = TEXT("你刚才说什么？"); Message.Subject = ServerPlayer;
        Agent->ObserveEvent(Message);
        Agent->BeginListening(ServerPlayer, MessageId);
        Settings->bEnabled = true;
        const bool bQueued = Agent->RequestDecision();
        Settings->bEnabled = false;
        if (!bQueued) return false;
        Request = Agent->ActiveRequest;
        // 直接向正式 NDJSON 解析入口送数据，不向正在运行的 AI 后台发请求
        Gateway->Queue.Remove(Request.RequestId);
        auto Proposal = MakeShared<FJsonObject>();
        Proposal->SetNumberField(TEXT("protocol_version"), FDeliveryNpcDecisionRequest::ProtocolVersion);
        Proposal->SetNumberField(TEXT("capability_version"), FDeliveryNpcDecisionRequest::CapabilityVersion);
        Proposal->SetStringField(TEXT("request_id"), Request.RequestId.ToString(EGuidFormats::DigitsWithHyphensLower));
        Proposal->SetStringField(TEXT("npc_id"), Request.NpcId.ToString(EGuidFormats::DigitsWithHyphensLower));
        Proposal->SetStringField(TEXT("world_epoch"), Request.WorldEpoch.ToString(EGuidFormats::DigitsWithHyphensLower));
        Proposal->SetNumberField(TEXT("context_revision"), Request.ContextRevision);
        Proposal->SetNumberField(TEXT("plan_revision"), Request.PlanRevision);
        Proposal->SetStringField(TEXT("target_id"), TEXT(""));
        Proposal->SetStringField(TEXT("action"), TEXT("Wait"));
        Proposal->SetNumberField(TEXT("duration"), 4);
        Proposal->SetStringField(TEXT("speech"), TEXT("听我说完。"));
        Proposal->SetStringField(TEXT("emotion"), TEXT("Calm"));
        Proposal->SetArrayField(TEXT("evidence_ids"), TArray<TSharedPtr<FJsonValue>>());
        auto Line = Event(TEXT("proposal")); Line->SetObjectField(TEXT("proposal"), Proposal); Feed(Line);
        FDeliveryNpcPublicState Action;
        if (!Agent->StartAction(Action)) return false;
        ClientNpc->Agent->PublicState = Action;
        ClientNpc->Agent->PublicState.SpeechTarget = ClientPlayer;
        ClientNpc->Agent->bDecisionPending = true;
        Client->ReplyAction = Action.ActionId;
        return true;
    };
    auto BeginSentence = [&](int32 Sequence, const FString& Text)
    {
        auto Line = Event(TEXT("speech_begin"));
        Line->SetNumberField(TEXT("sequence"), Sequence); Line->SetStringField(TEXT("text"), Text);
        Line->SetStringField(TEXT("emotion"), TEXT("Calm")); Line->SetNumberField(TEXT("sample_rate"), 24000);
        Line->SetNumberField(TEXT("audio_stream_version"), 1); Feed(Line);
    };
    auto Chunk = [&](int32 Sequence, int32 Index, int32 Offset, const TArray<uint8>& Pcm, int32 Count)
    {
        auto Line = Event(TEXT("audio_chunk"));
        Line->SetNumberField(TEXT("sequence"), Sequence); Line->SetNumberField(TEXT("chunk_index"), Index);
        Line->SetNumberField(TEXT("offset_bytes"), Offset);
        Line->SetStringField(TEXT("pcm"), FBase64::Encode(Pcm.GetData() + Offset, Count)); Feed(Line);
    };
    auto EndSentence = [&](int32 Sequence, int32 Count, int32 Bytes, bool bSucceeded = true)
    {
        auto Line = Event(TEXT("audio_end"));
        Line->SetNumberField(TEXT("sequence"), Sequence); Line->SetNumberField(TEXT("chunk_count"), Count);
        Line->SetNumberField(TEXT("total_pcm_bytes"), Bytes);
        Line->SetStringField(TEXT("status"), bSucceeded ? TEXT("completed") : TEXT("failed"));
        if (!bSucceeded) Line->SetStringField(TEXT("audio_error"), TEXT("TestSynthesisFailure"));
        Feed(Line);
    };
    auto FinishHttp = [&](int32 SentenceCount)
    {
        auto Line = Event(TEXT("done")); Line->SetNumberField(TEXT("sentences"), SentenceCount); Feed(Line);
        auto* Pending = Gateway->Pending.Find(Request.RequestId);
        if (!Pending) return false;
        Pending->bHttpFinished = Pending->bHttpSucceeded = true;
        Gateway->FinishStreamRequest(Request.RequestId);
        ClientNpc->Agent->bDecisionPending = Agent->IsDecisionPending();
        return !Agent->IsDecisionPending();
    };
    // 从服务器保留的实际压缩数据接收；两端不共享音频数组、解码器或播放器
    auto CopyNetworkData = [&]()
    {
        auto State = Host->GetSpeech();
        if (!State.bActive) return;
        // 只越过开播时间，不越过短句结尾；无声卡世界仍按服务器时间判断句末
        Host->Speech.StartTime = State.StartTime = Host->ServerTime() - .001;
        for (auto& Local : Host->LocalVoices) if (Local.State.TurnId == State.TurnId) Local.State.StartTime = State.StartTime;
        if (!StartedTurns.Contains(State.TurnId))
        {
            Client->StartStreamSpeech_Implementation(State);
            StartedTurns.Add(State.TurnId);
        }
        int32& Offset = SentBytes.FindOrAdd(State.TurnId);
        TArray<uint8> Data; int32 Total = 0;
        while (Host->GetSpeechAudioChunk(State.TurnId, Offset, 257, Data, Total))
        {
            Client->ReceiveAudio_Implementation(State.TurnId, Offset, Total, Data);
            Offset += Data.Num();
        }
        const auto* Saved = Host->ServerVoices.Find(State.TurnId);
        if (State.AudioFormatVersion == DeliveryNpcVoice::StreamingFormatVersion && State.bAudioComplete &&
            (!State.bRecordedVoice || (Saved && Saved->bComplete)) && !EndedTurns.Contains(State.TurnId))
        {
            Client->FinishStreamAudio_Implementation(State.TurnId, State.PcmBytes, State.EncodedBytes, State.bRecordedVoice);
            EndedTurns.Add(State.TurnId);
        }
        Client->PlayRecordedVoice();
        const auto Display = Client->GetDisplayedSpeech();
        if (Display.bActive) DisplayedTurns.Add(Display.TurnId);
    };
    auto Pump = [&]()
    {
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        Host->TickSentences();
        CopyNetworkData();
        Host->PlayRecordedVoice();
    };
    auto WaitUntil = [&](TFunctionRef<bool()> Ready)
    {
        const double Deadline = FPlatformTime::Seconds() + 5;
        do
        {
            Pump();
            if (Ready()) return true;
            FPlatformProcess::Sleep(.001f);
        } while (FPlatformTime::Seconds() < Deadline);
        return false;
    };
    auto HavePcm = [&](int32 Bytes)
    {
        return !Host->LocalVoices.IsEmpty() && !Client->LocalVoices.IsEmpty() &&
            Host->LocalVoices[0].Pcm.Num() >= Bytes && Client->LocalVoices[0].Pcm.Num() >= Bytes;
    };
    auto ReadAudio = [](UDeliveryNpcPresentationComponent* Presentation, TArray<uint8>& Heard)
    {
        uint8 Buffer[2048];
        while (Presentation->RecordedWave && Presentation->RecordedWave->GetAvailableAudioByteCount() > 0)
        {
            const int32 Count = Presentation->RecordedWave->GeneratePCMData(Buffer, 1024);
            if (Count <= 0) break;
            Heard.Append(Buffer, Count);
        }
    };
    auto FinishLocal = [&](UDeliveryNpcPresentationComponent* Presentation)
    {
        if (!Presentation->LocalVoices.IsEmpty()) Presentation->LocalVoices[0].DrainedAt = FPlatformTime::Seconds() - .2;
        Presentation->PlayRecordedVoice();
    };

    if (!TestTrue(TEXT("真实 Agent 请求经过 Gateway 接收动作建议"), BeginReply(true))) return false;
    const FString FirstText = TEXT("别着急，我刚才说的是车。"), SecondText = TEXT("听清了吧？");
    const auto Pcm = MakeNpcStreamTestPcm(38400); // 1.6 秒，超过开始与续播缓冲
    TArray<uint8> HostHeard, ClientHeard;
    BeginSentence(0, FirstText);
    Chunk(0, 0, 0, Pcm, 7680);
    if (!TestTrue(TEXT("第一小段经过正式接收、编码和客户端解码"), WaitUntil([&]() { return HavePcm(7680); }))) return false;
    TestNull(TEXT("主机不足初始缓冲时没有开播"), Host->RecordedWave.Get());
    TestNull(TEXT("客户端不足初始缓冲时没有开播"), Client->RecordedWave.Get());
    CheckBodyPose(EBodyPose::Idle, TEXT("首句声音未开始时不播放说话动作"));
    TestTrue(TEXT("首句声音未开始时两端都闭嘴"), Host->GetVoiceAmplitude()==0 && Client->GetVoiceAmplitude()==0);
    Chunk(0, 1, 7680, Pcm, 26880);
    if (!TestTrue(TEXT("句末尚未到达，两端已有可读声音"), WaitUntil([&]() { return HavePcm(34560) && Host->RecordedWave && Client->RecordedWave; }))) return false;
    const auto FirstState = Host->GetSpeech();
    USoundWaveProcedural* HostFirstWave = Host->RecordedWave.Get();
    USoundWaveProcedural* ClientFirstWave = Client->RecordedWave.Get();
    TestFalse(TEXT("开始播放时后端尚未结束本句"), FirstState.bAudioComplete);
    TestTrue(TEXT("首次播放时模型请求仍存在"), Agent->IsDecisionPending() && Gateway->Pending.Contains(Request.RequestId));
    CheckBodyPose(EBodyPose::Talk, TEXT("实际开始播放后进入说话动作"));
    // 模拟句末通知先到客户端，后续声音仍按原顺序逐段传输
    FDeliveryNpcVoiceStreamEncoder EarlyEndEncoder;
    TArray<uint8> EarlyEndEncoded, EarlyEndPart; FString EarlyEndError;
    if (!TestTrue(TEXT("准备提前句末通知的实际连续编码总长"),
        EarlyEndEncoder.Start(24000,EarlyEndEncoded,EarlyEndError) &&
        EarlyEndEncoder.Append(Pcm,EarlyEndPart,EarlyEndError))) return false;
    EarlyEndEncoded.Append(EarlyEndPart);
    if (!TestTrue(TEXT("提前句末通知包含实际尾音编码"),EarlyEndEncoder.Finish(EarlyEndPart,EarlyEndError))) return false;
    EarlyEndEncoded.Append(EarlyEndPart);
    Client->FinishStreamAudio_Implementation(FirstState.TurnId,Pcm.Num(),EarlyEndEncoded.Num(),true);
    TestTrue(TEXT("客户端已知总长时仍有声音尚未收到"),Client->LocalVoices[0].State.bAudioComplete &&
        Client->LocalVoices[0].Pcm.Num()<Client->LocalVoices[0].TotalBytes);
    ReadAudio(Host, HostHeard); ReadAudio(Client, ClientHeard);
    TestTrue(TEXT("主机在句未完成时实际读出 PCM"), HostHeard.Num() > 0 && HostHeard.Num() < Pcm.Num());
    TestTrue(TEXT("模拟客户端在句未完成时实际读出解码 PCM"), ClientHeard.Num() > 0 && ClientHeard.Num() < Pcm.Num());
    Client->StartStreamSpeech_Implementation(FirstState);
    Client->Speech = FirstState; Client->OnRep_Speech();
    TestEqual(TEXT("重复开始与复制通知仍只有一条待播句子"), Client->LocalVoices.Num(), 1);
    TestEqual(TEXT("音频追加前后字幕保持原句编号"), Client->GetDisplayedSpeech().TurnId, FirstState.TurnId);
    const float CaptionBeforePause = Client->GetDisplayedSpeechProgress();
    Pump();
    TestTrue(TEXT("暂时读空音频不会结束句子或清字幕"), Client->GetDisplayedSpeech().bActive &&
        Client->GetDisplayedSpeech().TurnId == FirstState.TurnId && !Client->FinishedVoiceTurns.Contains(FirstState.TurnId));
    TestTrue(TEXT("两端读空时等待续播缓冲"), Host->LocalVoices[0].bRebuffering && Client->LocalVoices[0].bRebuffering);
    TestTrue(TEXT("句末通知先到也不能把客户端缺音当作播完"),Client->LocalVoices[0].State.bAudioComplete &&
        Client->GetConsumedVoiceBytes()<Client->LocalVoices[0].TotalBytes && Client->IsWaitingForMoreAudio());
    TestTrue(TEXT("两端都能区分暂时缺音与整句结束"), Host->IsWaitingForMoreAudio() && Client->IsWaitingForMoreAudio());
    CheckBodyPose(EBodyPose::Idle, TEXT("音频读空时停止说话动作"));
    TestTrue(TEXT("暂时缺音时两端都闭嘴"), Host->GetVoiceAmplitude()==0 && Client->GetVoiceAmplitude()==0);
    Chunk(0, 2, 34560, Pcm, 7680);
    if (!TestTrue(TEXT("收到低于续播阈值的下一小段"), WaitUntil([&]() { return HavePcm(42240); }))) return false;
    TestTrue(TEXT("不足续播缓冲时继续保留当前句"), Client->LocalVoices[0].bRebuffering);
    Chunk(0, 3, 42240, Pcm, 23040);
    if (!TestTrue(TEXT("缓冲恢复后接着原句播放"), WaitUntil([&]() { return HavePcm(65280) && !Client->LocalVoices[0].bRebuffering; }))) return false;
    TestTrue(TEXT("续播仍使用原句的两个音频队列"), Host->RecordedWave == HostFirstWave && Client->RecordedWave == ClientFirstWave);
    TestTrue(TEXT("后续声音到达后结束临时等待"), !Host->IsWaitingForMoreAudio() && !Client->IsWaitingForMoreAudio());
    CheckBodyPose(EBodyPose::Talk, TEXT("续播时恢复原句的说话动作"));
    TestEqual(TEXT("身体动作恢复时仍显示原句字幕"), Client->GetDisplayedSpeech().TurnId, FirstState.TurnId);
    ReadAudio(Host, HostHeard); ReadAudio(Client, ClientHeard);
    Chunk(0, 4, 65280, Pcm, 11520);
    if (!TestTrue(TEXT("接收最后一段 PCM，仍不把它当句末"), WaitUntil([&]() { return HavePcm(Pcm.Num()); }))) return false;
    TestFalse(TEXT("没有 audio_end 时本句不能完成"), Host->GetSpeech().bAudioComplete);
    TestTrue(TEXT("续播期间字幕进度不倒退"), Client->GetDisplayedSpeechProgress() >= CaptionBeforePause);
    Host->Speech.StartTime = -Host->Speech.Duration - 1;
    Host->TickSentences();
    TestTrue(TEXT("超过预估台词时长也不能在句末通知前结束"), Host->GetSpeech().bActive &&
        Host->GetSpeech().TurnId == FirstState.TurnId && !Host->HasFinishedSentence());
    Host->Speech.StartTime = Host->ServerTime() - .001;
    EndSentence(0, 5, Pcm.Num());
    if (!TestTrue(TEXT("明确句末后两端获得尾音和最终长度"), WaitUntil([&]()
        { return Host->GetSpeech().bAudioComplete && !Client->LocalVoices.IsEmpty() && Client->LocalVoices[0].State.bAudioComplete; }))) return false;
    TestTrue(TEXT("收到最终长度后字幕进度不倒退"), Client->GetDisplayedSpeechProgress() >= CaptionBeforePause);
    // 客户端暂留第一句尾音，让下一句先在队列里等候
    ReadAudio(Host, HostHeard);
    TArray<uint8> HostExpected = Pcm;
    HostExpected.AddZeroed(DeliveryNpcVoice::GetDecodedPcmBytes(Pcm.Num(), 24000) - Pcm.Num());
    TArray<uint8> ReferenceEncoded, ReferencePcm; FString Error;
    FDeliveryNpcVoiceDecoder ReferenceDecoder; int32 ReferenceOffset = 0;
    if (!TestTrue(TEXT("生成原始 PCM 的 UE 编码参考"), DeliveryNpcVoice::Encode(Pcm, 24000, ReferenceEncoded, Error)) ||
        !TestTrue(TEXT("生成远端解码参考"), ReferenceDecoder.TryDecodeAvailable(ReferenceEncoded, ReferenceOffset, ReferencePcm, Error))) return false;
    TestTrue(TEXT("主机从头到尾读取原始声音，末尾仅补一次"), HostHeard == HostExpected);

    const auto ShortPcm = MakeNpcStreamTestPcm(1920);
    BeginSentence(1, SecondText); Chunk(1, 0, 0, ShortPcm, ShortPcm.Num()); EndSentence(1, 1, ShortPcm.Num());
    if (!TestTrue(TEXT("实际 done 与 HTTP 完成结束模型请求"), FinishHttp(2))) return false;
    FinishLocal(Host);
    Host->Speech.StartTime = -Host->Speech.Duration - 1;
    if (!TestTrue(TEXT("客户端仍播前一句时，后一句声音可以提前收齐"), WaitUntil([&]()
        { return Host->GetSpeech().Sequence == 1 && Host->RecordedWave && Client->LocalVoices.Num() == 2 &&
            Client->LocalVoices[1].State.bAudioComplete && Client->LocalVoices[1].Pcm.Num() ==
                DeliveryNpcVoice::GetDecodedPcmBytes(ShortPcm.Num(), 24000); }))) return false;
    Client->LocalVoices[1].LastDataAt = FPlatformTime::Seconds() - 6;
    TestEqual(TEXT("已收齐的下一句仍在等待前一句说完"), Client->GetDisplayedSpeech().TurnId, FirstState.TurnId);
    ReadAudio(Client, ClientHeard);
    TestTrue(TEXT("客户端实际读取完整声音，没有重复句首或丢尾音"), ClientHeard == ReferencePcm);
    Client->PlayRecordedVoice();
    TestTrue(TEXT("全部播完后的短暂收尾不再视为缺音"),!Client->IsWaitingForMoreAudio() &&
        Client->LocalVoices[0].DrainedAt>0 && Client->GetDisplayedSpeech().bActive);
    FinishLocal(Client);
    Pump();
    if (!TestNotNull(TEXT("排队超过五秒的完整短句仍可实际开播"), Client->RecordedWave.Get())) return false;
    TestEqual(TEXT("下一句使用自己的字幕与顺序"), Client->GetDisplayedSpeech().Text, SecondText);
    TestEqual(TEXT("五块音频加下一句只产生两个字幕编号"), DisplayedTurns.Num(), 2);
    TArray<uint8> ShortHostHeard, ShortClientHeard;
    ReadAudio(Host, ShortHostHeard); ReadAudio(Client, ShortClientHeard);
    TestEqual(TEXT("完整短句不因低于缓冲阈值丢失"), ShortHostHeard.Num(), DeliveryNpcVoice::GetDecodedPcmBytes(ShortPcm.Num(), 24000));
    TestEqual(TEXT("客户端也完整播放短句与尾音"), ShortClientHeard.Num(), ShortHostHeard.Num());
    FinishLocal(Host); FinishLocal(Client);
    Host->Speech.StartTime = -Host->Speech.Duration - 1;
    Pump();
    TestFalse(TEXT("两句完成后结束本轮播放"), Host->HasPendingSpeech());
    TestEqual(TEXT("整轮只保存一次完整发言"), Agent->Evidence.FilterByPredicate([&](const auto& Item)
        { return Item.Kind == TEXT("SelfSpeech") && Item.ReplyToEventId == MessageId && Item.Description == FirstText + SecondText; }).Num(), 1);

    // 让下一块仍在后台编码时打断，随后补送旧请求和旧句子的通知
    if (!TestTrue(TEXT("开始取消检查用的新回复"), BeginReply(true))) return false;
    BeginSentence(0, FirstText); Chunk(0, 0, 0, Pcm, 7680); Chunk(0, 1, 7680, Pcm, 26880);
    if (!TestTrue(TEXT("取消前两端确实正在播放"), WaitUntil([&]() { return HavePcm(34560) && Host->RecordedWave && Client->RecordedWave; }))) return false;
    const FGuid CancelledRequest = Request.RequestId, CancelledAction = Agent->GetPublicState().ActionId;
    const auto CancelledState = Host->GetSpeech();
    Chunk(0, 2, 34560, Pcm, 23040);
    Agent->CancelPlan(TEXT("PlayerInterrupted"));
    Client->StopStreamSpeech_Implementation(CancelledAction, true);
    if (!TestTrue(TEXT("取消后已经启动的后台编码能够退出"), WaitUntil([&]() { return Host->PendingEncodeTasks == 0; }))) return false;
    Chunk(0, 3, 57600, Pcm, 7680);
    EndSentence(0, 4, 65280);
    Client->StartStreamSpeech_Implementation(CancelledState);
    TArray<uint8> LateAudio; LateAudio.Append(ReferenceEncoded.GetData(), FMath::Min(257, ReferenceEncoded.Num()));
    Client->ReceiveAudio_Implementation(CancelledState.TurnId, 0, 0, LateAudio);
    Client->FinishStreamAudio_Implementation(CancelledState.TurnId, Pcm.Num(), ReferenceEncoded.Num(), true);
    TestTrue(TEXT("取消移除请求及两端全部旧声音"), !Gateway->Pending.Contains(CancelledRequest) &&
        Host->LocalVoices.IsEmpty() && Client->LocalVoices.IsEmpty() && Host->GeneratingSentences.IsEmpty());
    TestTrue(TEXT("迟到编码与句末通知不能恢复字幕"), !Host->GetDisplayedSpeech().bActive && !Client->GetDisplayedSpeech().bActive);
    TestNull(TEXT("取消后主机释放程序音频"), Host->RecordedWave.Get());
    TestNull(TEXT("取消后客户端释放程序音频"), Client->RecordedWave.Get());
    CheckBodyPose(EBodyPose::Idle, TEXT("取消后不继续播放说话动作"));
    TestTrue(TEXT("取消后没有遗留的缺音状态或张嘴动作"), !Host->IsWaitingForMoreAudio() &&
        !Client->IsWaitingForMoreAudio() && Host->GetVoiceAmplitude()==0 && Client->GetVoiceAmplitude()==0);

    // 后台合成失败与玩家取消不同，失败句保留文字，后一句继续走正常生成和播放
    if (!TestTrue(TEXT("开始检查单句合成失败"), BeginReply(true))) return false;
    BeginSentence(0, FirstText); Chunk(0, 0, 0, Pcm, 7680); Chunk(0, 1, 7680, Pcm, 26880);
    if (!TestTrue(TEXT("失败通知前音频已经进入两端播放器"), WaitUntil([&]()
        { return HavePcm(34560) && Host->RecordedWave && Client->RecordedWave; }))) return false;
    const FGuid FailedTurn = Host->GetSpeech().TurnId;
    EndSentence(0, 2, 34560, false);
    if (!TestTrue(TEXT("失败结束消息经过 Gateway 后两端保留本句文字"), WaitUntil([&]()
        { return Host->GetDisplayedSpeech().bActive && !Host->GetDisplayedSpeech().bRecordedVoice &&
            Client->GetDisplayedSpeech().bActive && !Client->GetDisplayedSpeech().bRecordedVoice; }))) return false;
    TestEqual(TEXT("声音失败没有重建字幕编号"), Client->GetDisplayedSpeech().TurnId, FailedTurn);
    TestEqual(TEXT("声音失败没有丢失完整文字"), Client->GetDisplayedSpeech().Text, FirstText);
    TestFalse(TEXT("单句合成失败没有取消本轮动作"), Client->CancelledVoiceActions.Contains(Agent->GetPublicState().ActionId));
    BeginSentence(1, SecondText); Chunk(1, 0, 0, ShortPcm, ShortPcm.Num()); EndSentence(1, 1, ShortPcm.Num());
    if (!TestTrue(TEXT("含失败句的回复仍按实际句数正常结束"), FinishHttp(2))) return false;
    Host->LocalVoices[0].TextStartedAt = FPlatformTime::Seconds() - Host->LocalVoices[0].State.Duration - .1;
    Client->LocalVoices[0].TextStartedAt = FPlatformTime::Seconds() - Client->LocalVoices[0].State.Duration - .1;
    Host->PlayRecordedVoice(); Client->PlayRecordedVoice();
    Host->Speech.StartTime = -Host->Speech.Duration - 1;
    if (!TestTrue(TEXT("文字回退结束后后一句仍实际开播"), WaitUntil([&]()
        { return Host->GetSpeech().Sequence == 1 && Host->RecordedWave && Client->RecordedWave; }))) return false;
    TArray<uint8> AfterFailureHost, AfterFailureClient;
    ReadAudio(Host, AfterFailureHost); ReadAudio(Client, AfterFailureClient);
    TestEqual(TEXT("失败句没有污染后一句的主机音频"), AfterFailureHost.Num(), ShortHostHeard.Num());
    TestEqual(TEXT("失败句没有污染后一句的客户端音频"), AfterFailureClient.Num(), ShortClientHeard.Num());

    // 旧后端关闭句内分段时仍可沿同一个 Gateway 与播放器播放完整 sentence
    if (!TestTrue(TEXT("新开关关闭后仍能请求旧整句回复"), BeginReply(false))) return false;
    auto Whole = Event(TEXT("sentence"));
    Whole->SetNumberField(TEXT("sequence"), 0); Whole->SetStringField(TEXT("text"), SecondText);
    Whole->SetStringField(TEXT("emotion"), TEXT("Calm")); Whole->SetNumberField(TEXT("sample_rate"), 24000);
    Whole->SetStringField(TEXT("pcm"), FBase64::Encode(ShortPcm)); Feed(Whole);
    if (!TestTrue(TEXT("旧整句完成通知正常处理"), FinishHttp(1)) ||
        !TestTrue(TEXT("旧整句通过两端实际播放器"), WaitUntil([&]() { return Host->RecordedWave && Client->RecordedWave; }))) return false;
    TArray<uint8> LegacyHost, LegacyClient;
    ReadAudio(Host, LegacyHost); ReadAudio(Client, LegacyClient);
    TestEqual(TEXT("旧声音仍使用原格式"), Host->GetSpeech().AudioFormatVersion, DeliveryNpcVoice::FormatVersion);
    TestEqual(TEXT("旧整句完整进入主机音频队列"), LegacyHost.Num(), ShortHostHeard.Num());
    TestEqual(TEXT("旧整句完整进入客户端音频队列"), LegacyClient.Num(), ShortClientHeard.Num());
    Agent->CancelPlan(TEXT("TestFinished"));
    Client->StopStreamSpeech_Implementation(FGuid(), true);
    return true;
}
// 只有明确指定本机地址才访问真实模型；普通自动化继续使用上面的确定性数据
class FDeliveryNpcLiveAudioCommand final : public IAutomationLatentCommand
{
public:
    FDeliveryNpcLiveAudioCommand(FAutomationTestBase* InTest,bool bInCancelMode) : Test(InTest),bCancelMode(bInCancelMode) {}
    virtual ~FDeliveryNpcLiveAudioCommand() override { Cleanup(); }

    virtual bool Update() override
    {
        if (!bInitialized)
        {
            if (!Initialize()) return Finish(false);
            bInitialized=true;
        }
        const double Now=FPlatformTime::Seconds();
        if (Now-StartedAt>45) { Test->AddError(TEXT("真实语音检查超过45秒，停止本次请求")); return Finish(false); }
        const float Dt=static_cast<float>(FMath::Clamp(Now-LastTickAt,.001,.05));
        const double AudioDt=FMath::Max(0.0,Now-LastTickAt);
        LastTickAt=Now;
        // latent 命令逐帧返回，引擎继续处理真实 HTTP 回调和后台编码完成通知
        ServerWorld->Tick(LEVELTICK_All,Dt);
        ClientWorld->Tick(LEVELTICK_All,Dt);
        auto* Agent=ServerNpc->Agent.Get();
        auto* Gateway=ServerWorld->GetSubsystem<UDeliveryAiGatewaySubsystem>();
        for (const auto& Pair:Gateway->Pending)
        {
            ObservedRequests.Add(Pair.Key);
            if (Pair.Value.Http.IsValid()) Mark(TEXT("http_dispatched"));
            if (Pair.Value.bHttpFinished) Mark(TEXT("http_completed"));
        }
        if (CancelledAt>=0)
        {
            // 取消后仍逐帧推进真实回调，检查旧字幕或音频是否重新出现
            bStayedCancelled&=Gateway->GetPendingCount()==0 && !Agent->IsDecisionPending() &&
                !Host->GetDisplayedSpeech().bActive && !Client->GetDisplayedSpeech().bActive &&
                Host->LocalVoices.IsEmpty() && Client->LocalVoices.IsEmpty() && Host->GeneratingSentences.IsEmpty() &&
                !Host->RecordedWave && !Client->RecordedWave;
            if (Now-CancelledAt>=.5 && Host->PendingEncodeTasks==0 && Client->PendingEncodeTasks==0)
            {
                bool bOk=Test->TestEqual(TEXT("真实打断检查仍只提交一次模型请求"),ObservedRequests.Num(),1);
                bOk=Test->TestTrue(TEXT("打断后持续半秒没有恢复旧声音、字幕或请求"),bStayedCancelled) && bOk;
                bOk=Test->TestTrue(TEXT("打断前已开始的UE编码回调全部结束"),Host->PendingEncodeTasks==0 && Client->PendingEncodeTasks==0) && bOk;
                Mark(TEXT("cancel_callbacks_drained"));
                return Finish(bOk);
            }
            if (Now-CancelledAt>5)
            { Test->AddError(TEXT("打断后五秒内UE编码回调仍未结束")); return Finish(false); }
            return false;
        }
        const auto PublicState=Agent->GetPublicState();
        if (PublicState.ActionId.IsValid()) Mark(TEXT("proposal_received"));
        if (PublicState.Status==EDeliveryNpcActionStatus::Accepted && !bStartedTree)
        {
            // 复用正式执行节点，让动作正常开始和结束，不手改成功状态延长语音
            auto* Controller=Cast<AAIController>(ServerNpc->GetController());
            auto* Tree=NewObject<UBehaviorTree>(Controller);
            Tree->RootNode=NewObject<UBTComposite_Sequence>(Tree);
            FBTCompositeChild Child;
            Child.ChildTask=NewObject<UBTTask_DeliveryExecuteNpcAction>(Tree);
            Tree->RootNode->Children.Add(Child);
            if (!Controller || !Controller->RunBehaviorTree(Tree))
            { Test->AddError(TEXT("真实模型返回动作后未能启动正式执行节点")); return Finish(false); }
            bStartedTree=true;
        }
        ClientNpc->Agent->PublicState=PublicState;
        ClientNpc->Agent->PublicState.Target=PublicState.Target==ServerPlayer?ClientPlayer:nullptr;
        ClientNpc->Agent->PublicState.SpeechTarget=PublicState.SpeechTarget==ServerPlayer?ClientPlayer:nullptr;
        ClientNpc->Agent->bDecisionPending=Agent->IsDecisionPending();
        ClientNpc->Agent->ListeningSpeaker=Agent->GetListeningSpeaker()==ServerPlayer?ClientPlayer:nullptr;
        ClientNpc->Agent->ListeningStartedAt=Agent->GetListeningStartTime();
        Client->ReplyAction=Host->ReplyAction;
        if (!Host->GeneratingSentences.IsEmpty()) Mark(TEXT("speech_begin_received"));
        for (const auto& Pair:Host->GeneratingSentences)
        {
            if (Pair.Value.ReceivedPcmBytes>0) Mark(TEXT("pcm_received"));
            if (Pair.Value.bInputEnded) Mark(TEXT("audio_end_received"));
        }
        CopyAudio();
        Host->PlayRecordedVoice();
        Client->PlayRecordedVoice();
        Consume(Host,true,AudioDt);
        Consume(Client,false,AudioDt);
        if (bCancelMode)
        {
            const auto HostDisplay=Host->GetDisplayedSpeech();
            const auto ClientDisplay=Client->GetDisplayedSpeech();
            const auto* First=Turns.Find(HostDisplay.TurnId);
            if (First && First->State.Sequence==0 && First->bHostNonzero && First->bClientNonzero && !First->bInputEnded &&
                HostDisplay.bActive && ClientDisplay.bActive && HostDisplay.TurnId==ClientDisplay.TurnId &&
                Agent->IsDecisionPending() && Gateway->Pending.Contains(RequestId))
            {
                CancelledAt=FPlatformTime::Seconds();
                EncodeTasksAtCancel=Host->PendingEncodeTasks;
                Mark(TEXT("cancel_requested_before_audio_end"));
                Agent->CancelPlan(TEXT("LiveAudioTestInterrupted"));
                Client->StopStreamSpeech_Implementation(HostDisplay.ActionId,true);
                return false;
            }
        }
        if (!Agent->IsDecisionPending() && Gateway->GetPendingCount()==0) Mark(TEXT("request_finished"));
        if (!Agent->IsDecisionPending() && Gateway->GetPendingCount()==0 && !Host->HasPendingSpeech() &&
            Client->LocalVoices.IsEmpty() && !Client->GetDisplayedSpeech().bActive)
        {
            if (bCancelMode)
            {
                bCancelNotCovered=Host->ExpectedSentences>0 && Host->NextSentence==Host->ExpectedSentences;
                Test->AddError(bCancelNotCovered?
                    TEXT("本次回复未出现两端开播且首句仍在生成的时段，真实流式打断未覆盖；没有追加请求或修改文案"):
                    TEXT("本次真实回复未正常完成，无法检查流式打断；保留失败结果，不追加模型请求"));
                return Finish(false);
            }
            bool bOk=Test->TestEqual(TEXT("真实检查只提交一次模型请求"),ObservedRequests.Num(),1);
            bOk=Test->TestTrue(TEXT("真实后端返回了可播放的句内分段语音"),!Turns.IsEmpty() && bSawStreaming) && bOk;
            bOk=Test->TestTrue(TEXT("真实done声明的全部句子已经完成，连接失败未被误当成功"),Host->ExpectedSentences>0 &&
                Host->NextSentence==Host->ExpectedSentences && Turns.Num()==Host->ExpectedSentences) && bOk;
            TSet<int32> Sequences;
            for (const auto& Pair:Turns)
            {
                const auto& Turn=Pair.Value;
                Sequences.Add(Turn.State.Sequence);
                TArray<uint8> ExpectedHost=Turn.SourcePcm;
                ExpectedHost.AddZeroed(DeliveryNpcVoice::GetDecodedPcmBytes(Turn.SourcePcm.Num(),24000)-Turn.SourcePcm.Num());
                FDeliveryNpcVoiceDecoder Decoder;
                int32 Offset=0;
                TArray<uint8> ExpectedClient;
                FString Error;
                bOk=Test->TestTrue(TEXT("真实句末声明完整，声音和字幕使用同一句编号"),Turn.bEnded &&
                    Turn.State.ReplyToEventId==MessageId && Turn.bHostNonzero && Turn.bClientNonzero) && bOk;
                bOk=Test->TestTrue(TEXT("主机程序音频完整读取原始声音和一次尾部补零"),Turn.HostPcm==ExpectedHost) && bOk;
                const bool bDecoded=Decoder.TryDecodeAvailable(Turn.Encoded,Offset,ExpectedClient,Error);
                bOk=Test->TestTrue(TEXT("接收端程序音频完整读取真实Opus解码结果"),bDecoded && Turn.ClientPcm==ExpectedClient) && bOk;
            }
            bool bCompleteSequence=Sequences.Num()==Host->ExpectedSentences;
            for (int32 Index=0;Index<Host->ExpectedSentences;++Index) bCompleteSequence&=Sequences.Contains(Index);
            bOk=Test->TestTrue(TEXT("实际播放句子从零开始且没有重复或缺号"),bCompleteSequence) && bOk;
            return Finish(bOk);
        }
        return false;
    }

private:
    struct FTurn
    {
        FDeliveryNpcSpeechState State;
        TArray<uint8> Encoded,SourcePcm,HostPcm,ClientPcm;
        int32 SentBytes=0;
        bool bEnded=false,bInputEnded=false,bHostNonzero=false,bClientNonzero=false;
        bool bConsumedBeforeEnd=false;
        bool bHostConsumedBeforeEnd=false,bClientConsumedBeforeEnd=false;
        double HostGapAt=-1,ClientGapAt=-1,HostGapSeconds=0,ClientGapSeconds=0;
    };

    bool Initialize()
    {
        ReportPath=FPaths::ProjectSavedDir()/TEXT("AIResearch/LiveAudioStreaming")/
            FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%S"))/TEXT("Report.json");
        auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
        OldEnabled=Settings->bEnabled; OldReplies=Settings->bStreamReplies; OldAudio=Settings->bStreamSentenceAudio;
        OldEndpoint=Settings->DecisionEndpoint; OldTimeout=Settings->RequestTimeout;
        bChangedSettings=true;
        Settings->bEnabled=false; Settings->bStreamReplies=true; Settings->bStreamSentenceAudio=true;
        Settings->DecisionEndpoint=TEXT("http://127.0.0.1:8769/v1/decisions"); Settings->RequestTimeout=30;
        auto MakeWorld=[&]()
        {
            const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
                .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(true);
            auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
            Worlds.Add(World);
            GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
            World->InitializeActorsForPlay(FURL()); World->GetWorldSettings()->NotifyBeginPlay();
            return World;
        };
        ServerWorld=MakeWorld(); ClientWorld=MakeWorld();
        ServerNpc=ServerWorld->SpawnActor<ADeliveryNpcCharacter>();
        ClientNpc=ClientWorld->SpawnActor<ADeliveryNpcCharacter>();
        auto* Profiles=LoadObject<UDataTable>(nullptr,TEXT("/Game/NPC/Residents/Config/DT_NpcProfiles.DT_NpcProfiles"));
        if (!Test->TestNotNull(TEXT("真实检查使用正式人物配置表"),Profiles)) return false;
        for (auto* Npc:{ServerNpc,ClientNpc})
        {
            Npc->SetActorTickEnabled(false);
            Npc->GetCharacterMovement()->DisableMovement();
            Npc->Profile.DataTable=Profiles; Npc->Profile.RowName=TEXT("xiumei");
            if (!Test->TestTrue(TEXT("加载翠花正式姓名、人设与B音色"),Npc->ApplyProfile() && Npc->Presentation->TtsVoice==TEXT("female"))) return false;
        }
        ClientNpc->SetRole(ROLE_SimulatedProxy);
        ClientNpc->Agent->SetComponentTickEnabled(false);
        if (!ServerNpc->GetController()) ServerNpc->SpawnDefaultController();
        Host=ServerNpc->Presentation.Get(); Client=ClientNpc->Presentation.Get();
        auto* Pawn=ServerWorld->SpawnActor<APawn>();
        auto* Root=NewObject<USceneComponent>(Pawn); Pawn->SetRootComponent(Root); Root->RegisterComponent();
        Pawn->SetActorLocation(FVector(220,0,0));
        auto* Controller=ServerWorld->SpawnActor<APlayerController>();
        Controller->PlayerState=ServerWorld->SpawnActor<APlayerState>();
        Controller->PlayerState->SetIsABot(false); Controller->Possess(Pawn);
        ServerPlayer=Pawn; ClientPlayer=ClientWorld->SpawnActor<AActor>();
        StartedAt=LastTickAt=FPlatformTime::Seconds();
        MessageId=FGuid::NewGuid();
        Settings->bEnabled=true;
        const bool bHeard=ServerNpc->HearSpeech(Pawn,TEXT("翠花，早上好！今天过得怎么样？"),MessageId,0);
        // 请求已入队后关闭新增请求；现有 Gateway 仍负责完成这一次 HTTP
        Settings->bEnabled=false;
        const auto& Submitted=ServerNpc->Agent->ActiveRequest;
        RequestId=Submitted.RequestId; RequestWorldEpoch=Submitted.WorldEpoch; RequestNpcId=Submitted.NpcId;
        if (!Test->TestTrue(TEXT("固定问候只建立一个真实待处理请求"),bHeard && RequestId.IsValid() &&
            ServerWorld->GetSubsystem<UDeliveryAiGatewaySubsystem>()->GetPendingCount()==1)) return false;
        ObservedRequests.Add(RequestId);
        Mark(TEXT("submitted"));
        return true;
    }

    void Mark(FName Stage)
    {
        if (Stages.Contains(Stage)) return;
        const double Ms=(FPlatformTime::Seconds()-StartedAt)*1000;
        Stages.Add(Stage,Ms);
        Test->AddInfo(FString::Printf(TEXT("LiveAudio stage=%s elapsed_ms=%.1f request=%s"),*Stage.ToString(),Ms,*RequestId.ToString()));
    }

    void CopyAudio()
    {
        const auto State=Host->GetSpeech();
        if (State.bActive)
        {
            if (!Turns.Contains(State.TurnId))
            {
                Turns.Add(State.TurnId);
                Client->StartStreamSpeech_Implementation(State);
            }
            auto& Current=Turns.FindChecked(State.TurnId);
            Current.State=State;
            bSawStreaming|=State.AudioFormatVersion==DeliveryNpcVoice::StreamingFormatVersion;
            if (const auto* Generating=Host->GeneratingSentences.Find(State.Sequence)) Current.bInputEnded|=Generating->bInputEnded;
            Current.bInputEnded|=State.bAudioComplete;
        }
        // 继续接收已经切走的上一句，避免同帧切句时漏掉它保留在服务器上的尾音
        for (auto& Pair:Turns)
        {
            auto& Turn=Pair.Value;
            TArray<uint8> Data; int32 Total=0;
            while (Host->GetSpeechAudioChunk(Pair.Key,Turn.SentBytes,2048,Data,Total))
            {
                Client->ReceiveAudio_Implementation(Pair.Key,Turn.SentBytes,Total,Data);
                Turn.SentBytes+=Data.Num(); Turn.Encoded.Append(Data);
            }
            const auto* Saved=Host->ServerVoices.Find(Pair.Key);
            if (Saved && Saved->bComplete && !Turn.bEnded)
            {
                Turn.SourcePcm=Saved->Pcm;
                Client->FinishStreamAudio_Implementation(Pair.Key,Saved->Pcm.Num(),Saved->Encoded.Num(),true);
                Turn.bEnded=true; Turn.bInputEnded=true;
                Mark(TEXT("audio_encoding_finished"));
            }
            else if (Pair.Key==State.TurnId && State.bAudioComplete && !State.bRecordedVoice && !Turn.bEnded)
            {
                Client->FinishStreamAudio_Implementation(Pair.Key,State.PcmBytes,State.EncodedBytes,false);
                Turn.bEnded=true; Turn.bInputEnded=true;
            }
        }
    }

    void Consume(UDeliveryNpcPresentationComponent* Presentation,bool bHost,double Dt)
    {
        double& Credit=bHost?HostCredit:ClientCredit;
        const auto Display=Presentation->GetDisplayedSpeech();
        auto* Turn=Turns.Find(Display.TurnId);
        if (!Presentation->RecordedWave || !Display.bActive || !Turn) { Credit=0; return; }
        const double Now=FPlatformTime::Seconds();
        double& GapAt=bHost?Turn->HostGapAt:Turn->ClientGapAt;
        double& GapSeconds=bHost?Turn->HostGapSeconds:Turn->ClientGapSeconds;
        if (Presentation->IsWaitingForMoreAudio())
        {
            if (GapAt<0) GapAt=Now;
            Credit=0;
            return;
        }
        else if (GapAt>=0) { GapSeconds+=Now-GapAt; GapAt=-1; }
        Credit+=Dt;
        // UE 每次至少接收 512 个样本的读取请求，末尾实际返回多少由声音对象决定
        constexpr int32 SamplesPerRead=512;
        constexpr double ReadInterval=double(SamplesPerRead)/24000.0;
        while (Credit>=ReadInterval)
        {
            Credit-=ReadInterval;
            if (Presentation->RecordedWave->GetAvailableAudioByteCount()<=0) { Credit=0; break; }
            uint8 Bytes[SamplesPerRead*2];
            const int32 Count=Presentation->RecordedWave->GeneratePCMData(Bytes,SamplesPerRead);
            if (Count<=0) break;
            (bHost?Turn->HostPcm:Turn->ClientPcm).Append(Bytes,Count);
            bool bNonzero=false;
            for (int32 Index=0;Index<Count;++Index) bNonzero|=Bytes[Index]!=0;
            if (bNonzero)
            {
                (bHost?Turn->bHostNonzero:Turn->bClientNonzero)=true;
                Mark(bHost?TEXT("host_first_nonzero_pcm"):TEXT("receiver_first_nonzero_pcm"));
                Turn->bConsumedBeforeEnd|=!Turn->bInputEnded;
                (bHost?Turn->bHostConsumedBeforeEnd:Turn->bClientConsumedBeforeEnd)|=!Turn->bInputEnded;
            }
        }
    }

    bool Finish(bool bSucceeded)
    {
        auto Report=MakeShared<FJsonObject>();
        Report->SetStringField(TEXT("status"),bCancelNotCovered?TEXT("not_covered"):(bSucceeded?TEXT("passed"):TEXT("failed")));
        Report->SetStringField(TEXT("mode"),bCancelMode?TEXT("cancel_during_first_sentence"):TEXT("complete_reply"));
        if (bCancelMode)
        {
            Report->SetBoolField(TEXT("cancelled_before_first_audio_end"),CancelledAt>=0);
            Report->SetNumberField(TEXT("post_cancel_observed_ms"),CancelledAt>=0?(FPlatformTime::Seconds()-CancelledAt)*1000:0);
            Report->SetNumberField(TEXT("ue_encode_tasks_at_cancel"),EncodeTasksAtCancel);
            Report->SetBoolField(TEXT("ue_state_stayed_cancelled"),CancelledAt>=0 && bStayedCancelled);
            Report->SetBoolField(TEXT("backend_worker_termination_verified"),false);
            Report->SetStringField(TEXT("cancel_coverage"),TEXT("只检查UE请求取消、编码回调与两端本机状态；后端是否真正结束需另按request_id核对后台日志"));
        }
        Report->SetStringField(TEXT("request_id"),RequestId.ToString(EGuidFormats::DigitsWithHyphensLower));
        Report->SetStringField(TEXT("world_epoch"),RequestWorldEpoch.ToString(EGuidFormats::DigitsWithHyphensLower));
        Report->SetStringField(TEXT("npc_id"),RequestNpcId.ToString(EGuidFormats::DigitsWithHyphensLower));
        Report->SetStringField(TEXT("message_id"),MessageId.ToString());
        Report->SetStringField(TEXT("endpoint"),TEXT("http://127.0.0.1:8769/v1/decisions"));
        Report->SetStringField(TEXT("voice"),TEXT("female"));
        const auto* Voice=Host && Host->VoiceSet?Host->VoiceSet.Get():GetDefault<UDeliveryNpcVoiceSet>();
        Report->SetNumberField(TEXT("streaming_start_buffer_seconds"),Voice->StreamingStartBufferSeconds);
        Report->SetNumberField(TEXT("streaming_resume_buffer_seconds"),Voice->StreamingResumeBufferSeconds);
        Report->SetNumberField(TEXT("request_count"),ObservedRequests.Num());
        auto Timing=MakeShared<FJsonObject>();
        for (const auto& Pair:Stages) Timing->SetNumberField(Pair.Key.ToString(),Pair.Value);
        Report->SetObjectField(TEXT("observed_stage_ms"),Timing);
        TArray<TSharedPtr<FJsonValue>> Rows;
        bool bEarly=false;
        for (const auto& Pair:Turns)
        {
            const auto& Turn=Pair.Value;
            auto Row=MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("turn_id"),Pair.Key.ToString()); Row->SetStringField(TEXT("text"),Turn.State.Text);
            Row->SetNumberField(TEXT("sequence"),Turn.State.Sequence); Row->SetNumberField(TEXT("format_version"),Turn.State.AudioFormatVersion);
            Row->SetNumberField(TEXT("source_pcm_bytes"),Turn.SourcePcm.Num()); Row->SetNumberField(TEXT("encoded_bytes"),Turn.Encoded.Num());
            Row->SetNumberField(TEXT("host_consumed_bytes"),Turn.HostPcm.Num()); Row->SetNumberField(TEXT("receiver_consumed_bytes"),Turn.ClientPcm.Num());
            Row->SetNumberField(TEXT("host_rebuffer_ms"),Turn.HostGapSeconds*1000);
            Row->SetNumberField(TEXT("receiver_rebuffer_ms"),Turn.ClientGapSeconds*1000);
            Row->SetBoolField(TEXT("pcm_consumed_before_audio_end_received"),Turn.bConsumedBeforeEnd);
            Row->SetBoolField(TEXT("host_pcm_before_audio_end_received"),Turn.bHostConsumedBeforeEnd);
            Row->SetBoolField(TEXT("receiver_pcm_before_audio_end_received"),Turn.bClientConsumedBeforeEnd);
            Rows.Add(MakeShared<FJsonValueObject>(Row)); bEarly|=Turn.bConsumedBeforeEnd;
        }
        Report->SetArrayField(TEXT("sentences"),Rows);
        Report->SetBoolField(TEXT("early_consumption_observed"),bEarly);
        Report->SetStringField(TEXT("coverage"),TEXT("真实HTTP/模型/语音，经正式Agent、执行节点、编码、解码和程序音频读取；接收端是独立世界手工传递，可一帧送完现有数据，未模拟实际每帧2KB发送和网络延迟；首PCM时间只反映本机数据处理，没有声卡输出或画面验收"));
        if (!ReportPath.IsEmpty())
        {
            FString Json; FJsonSerializer::Serialize(Report,TJsonWriterFactory<>::Create(&Json));
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath),true);
            if (!FFileHelper::SaveStringToFile(Json,*ReportPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
                Test->AddError(TEXT("真实语音检查报告保存失败"));
            else Test->AddInfo(TEXT("LiveAudio report=")+FPaths::ConvertRelativePathToFull(ReportPath));
        }
        Test->AddInfo(bEarly?TEXT("本样本在收到句末前已读出非零PCM，未测声卡实际发声时间"):
            TEXT("本样本未证实句末前已读出非零PCM，不据此声称降低了实际播放延迟"));
        Cleanup();
        return true;
    }

    void Cleanup()
    {
        if (ServerNpc) ServerNpc->Agent->CancelPlan(TEXT("LiveAudioTestEnd"));
        if (Client) Client->StopStreamSpeech_Implementation(FGuid(),true);
        for (auto* World:Worlds)
        { World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
        Worlds.Reset(); ServerNpc=ClientNpc=nullptr; Host=Client=nullptr;
        if (bChangedSettings)
        {
            auto* Settings=GetMutableDefault<UDeliveryAiSettings>();
            Settings->bEnabled=OldEnabled; Settings->bStreamReplies=OldReplies; Settings->bStreamSentenceAudio=OldAudio;
            Settings->DecisionEndpoint=OldEndpoint; Settings->RequestTimeout=OldTimeout; bChangedSettings=false;
        }
    }

    FAutomationTestBase* Test;
    TArray<UWorld*> Worlds;
    UWorld* ServerWorld=nullptr; UWorld* ClientWorld=nullptr;
    ADeliveryNpcCharacter* ServerNpc=nullptr; ADeliveryNpcCharacter* ClientNpc=nullptr;
    AActor* ServerPlayer=nullptr; AActor* ClientPlayer=nullptr;
    UDeliveryNpcPresentationComponent* Host=nullptr; UDeliveryNpcPresentationComponent* Client=nullptr;
    TMap<FGuid,FTurn> Turns;
    TMap<FName,double> Stages;
    TSet<FGuid> ObservedRequests;
    FGuid RequestId,MessageId,RequestWorldEpoch,RequestNpcId;
    FString OldEndpoint,ReportPath;
    float OldTimeout=0;
    bool OldEnabled=false,OldReplies=false,OldAudio=false,bChangedSettings=false;
    bool bInitialized=false,bStartedTree=false,bSawStreaming=false;
    bool bCancelMode=false,bCancelNotCovered=false,bStayedCancelled=true;
    int32 EncodeTasksAtCancel=0;
    double CancelledAt=-1;
    double StartedAt=0,LastTickAt=0,HostCredit=0,ClientCredit=0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcLiveAudioTest,"Delivery.AI.LiveAudioStreaming",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcLiveAudioTest::RunTest(const FString&)
{
    FString Endpoint;
    if (!FParse::Value(FCommandLine::Get(),TEXT("DeliveryAILiveAudioEndpoint="),Endpoint))
    {
        AddInfo(TEXT("未执行真实模型检查：需要显式指定 -DeliveryAILiveAudioEndpoint=http://127.0.0.1:8769/v1/decisions；本条结果不算真实链路已验证"));
        return true;
    }
    if (Endpoint!=TEXT("http://127.0.0.1:8769/v1/decisions"))
    { AddError(TEXT("真实语音检查仅允许指定本机8769端口，不会请求其他服务")); return false; }
    const bool bCancel=FParse::Param(FCommandLine::Get(),TEXT("DeliveryAILiveAudioCancel"));
    ADD_LATENT_AUTOMATION_COMMAND(FDeliveryNpcLiveAudioCommand(this,bCancel));
    return true;
}
#endif
