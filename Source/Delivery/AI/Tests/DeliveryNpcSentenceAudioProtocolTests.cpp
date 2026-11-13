#include "Misc/AutomationTest.h"
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Network/DeliveryAiGatewaySubsystem.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/Base64.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcSentenceAudioProtocolTest, "Delivery.AI.SentenceAudioProtocol",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcSentenceAudioProtocolTest::RunTest(const FString&)
{
    auto* Settings = GetMutableDefault<UDeliveryAiSettings>();
    const bool Enabled = Settings->bEnabled, Replies = Settings->bStreamReplies, Audio = Settings->bStreamSentenceAudio;
    Settings->bEnabled = false;
    Settings->bStreamReplies = true;
    Settings->bStreamSentenceAudio = true;
    const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    World->GetWorldSettings()->NotifyBeginPlay();
    ON_SCOPE_EXIT
    {
        World->EndPlay(EEndPlayReason::Quit);
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
        Settings->bEnabled = Enabled;
        Settings->bStreamReplies = Replies;
        Settings->bStreamSentenceAudio = Audio;
    };
    auto* Pawn = World->SpawnActor<APawn>();
    auto* Root = NewObject<USceneComponent>(Pawn);
    Pawn->SetRootComponent(Root);
    Root->RegisterComponent();
    auto* Agent = NewObject<UDeliveryNpcAgentComponent>(Pawn);
    Pawn->AddInstanceComponent(Agent);
    Agent->RegisterComponent();
    Agent->SetComponentTickEnabled(false);
    auto* Gateway = World->GetSubsystem<UDeliveryAiGatewaySubsystem>();

    FDeliveryNpcDecisionRequest Plain;
    TSharedPtr<FJsonObject> Encoded;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(DeliveryNpc::EncodeRequest(Plain)), Encoded);
    if (!TestTrue(TEXT("默认请求可编码且不启用句内分段"), Encoded.IsValid() && !Encoded->HasField(TEXT("speech_transport")))) return false;
    Plain.bStreamSentenceAudio = true;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(DeliveryNpc::EncodeRequest(Plain)), Encoded);
    TestEqual(TEXT("显式启用后才请求分段协议"), Encoded->GetStringField(TEXT("speech_transport")), FString(TEXT("pcm_chunks_v1")));
    Settings->bStreamReplies = false;
    Agent->BuildDecisionRequest(Gateway->GetWorldEpoch(), Plain);
    TestFalse(TEXT("关闭流式回复时不会单独请求音频块"), Plain.bStreamSentenceAudio);
    Settings->bStreamReplies = true;

    using FPending = UDeliveryAiGatewaySubsystem::FPending;
    using EState = UDeliveryAiGatewaySubsystem::ESentenceReception;
    // 这里检查网络协议，已通过的动作建议由夹具设置；实际编码与播放在 AudioStreaming 测试中检查
    auto StartRequest = [&]()
    {
        Agent->CancelDecision();
        Agent->PublicState = {};
        Settings->bEnabled = true;
        const bool Accepted = Agent->RequestDecision();
        Settings->bEnabled = false;
        TestTrue(TEXT("协议夹具创建真实待处理请求"), Accepted);
        const auto Id = Agent->ActiveRequest.RequestId;
        if (auto* Pending = Gateway->Pending.Find(Id)) Pending->bHasProposal = true;
        Agent->ReplyRequest = Id;
        Agent->ReplyAction = FGuid::NewGuid();
        Agent->PublicState.ActionId = Agent->ReplyAction;
        Agent->PublicState.Status = EDeliveryNpcActionStatus::Accepted;
        return Id;
    };
    auto Event = [](FGuid Id, const TCHAR* Type)
    {
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("request_id"), Id.ToString(EGuidFormats::DigitsWithHyphensLower));
        Json->SetStringField(TEXT("type"), Type);
        Json->SetNumberField(TEXT("sequence"), 0);
        Json->SetStringField(TEXT("text"), TEXT("你听我说完。"));
        Json->SetStringField(TEXT("emotion"), TEXT("Calm"));
        Json->SetNumberField(TEXT("sample_rate"), 24000);
        Json->SetNumberField(TEXT("audio_stream_version"), 1);
        Json->SetNumberField(TEXT("chunk_index"), 0);
        Json->SetNumberField(TEXT("offset_bytes"), 0);
        Json->SetStringField(TEXT("pcm"), TEXT("AAA="));
        Json->SetStringField(TEXT("status"), TEXT("completed"));
        Json->SetNumberField(TEXT("chunk_count"), 1);
        Json->SetNumberField(TEXT("total_pcm_bytes"), 2);
        Json->SetNumberField(TEXT("sentences"), 1);
        return Json;
    };
    auto Send = [&](FGuid Id, const TSharedRef<FJsonObject>& Json)
    {
        FString Line;
        FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Line));
        Gateway->ReadStream(Id, Line);
    };
    auto Reject = [&](const TCHAR* Label, const TCHAR* Type, TFunctionRef<void(FJsonObject&, FPending&)> Change)
    {
        const auto Id = StartRequest();
        auto* Pending = Gateway->Pending.Find(Id);
        if (!Pending) return;
        auto Json = Event(Id, Type);
        Change(*Json, *Pending);
        Send(Id, Json);
        TestFalse(Label, Gateway->Pending.Contains(Id));
        TestFalse(TEXT("拒绝数据后结束本次等待"), Agent->IsDecisionPending());
    };
    Reject(TEXT("未经协商不接收分段"), TEXT("speech_begin"), [](auto&, auto& Pending) { Pending.Request.bStreamSentenceAudio = false; });
    Reject(TEXT("不把小数序号截断为整数"), TEXT("speech_begin"), [](auto& Json, auto&) { Json.SetNumberField(TEXT("sequence"), .5); });
    Reject(TEXT("每轮最多三句话"), TEXT("speech_begin"), [](auto& Json, auto&) { Json.SetNumberField(TEXT("sequence"), 3); });
    Reject(TEXT("未知音频版本被拒绝"), TEXT("speech_begin"), [](auto& Json, auto&) { Json.SetNumberField(TEXT("audio_stream_version"), 2); });
    Reject(TEXT("分段采样率必须一致"), TEXT("speech_begin"), [](auto& Json, auto&) { Json.SetNumberField(TEXT("sample_rate"), 16000); });
    Reject(TEXT("同一句不能从完整音频改成分段"), TEXT("speech_begin"), [](auto&, auto& Pending) { Pending.Sentences[0].State = EState::Whole; });
    Reject(TEXT("同一句不能从分段改成完整音频"), TEXT("sentence"), [](auto&, auto& Pending) { Pending.Sentences[0].State = EState::Streaming; });
    Reject(TEXT("收到开头之前不能追加音频"), TEXT("audio_chunk"), [](auto&, auto&) {});
    Reject(TEXT("重复音频块不会播放两次"), TEXT("audio_chunk"), [](auto&, auto& Pending)
    { Pending.Sentences[0].State = EState::Streaming; Pending.Sentences[0].NextChunkIndex = 1; Pending.Sentences[0].PcmBytes = 2; });
    Reject(TEXT("音频偏移必须接在上一块之后"), TEXT("audio_chunk"), [](auto& Json, auto& Pending)
    { Pending.Sentences[0].State = EState::Streaming; Json.SetNumberField(TEXT("offset_bytes"), 2); });
    Reject(TEXT("一块不能超过八帧音频"), TEXT("audio_chunk"), [](auto& Json, auto& Pending)
    {
        Pending.Sentences[0].State = EState::Streaming;
        TArray<uint8> Pcm; Pcm.SetNumZeroed(30722);
        Json.SetStringField(TEXT("pcm"), FBase64::Encode(Pcm));
    });
    Reject(TEXT("累计音频不能超过八秒"), TEXT("audio_chunk"), [](auto& Json, auto& Pending)
    {
        Pending.Sentences[0].State = EState::Streaming; Pending.Sentences[0].PcmBytes = 384000;
        Json.SetNumberField(TEXT("offset_bytes"), 384000);
    });
    Reject(TEXT("单句不能接受第三十三块"), TEXT("audio_chunk"), [](auto& Json, auto& Pending)
    {
        Pending.Sentences[0].State = EState::Streaming; Pending.Sentences[0].NextChunkIndex = 32;
        Json.SetNumberField(TEXT("chunk_index"), 32);
    });
    Reject(TEXT("结束通知必须符合已收到的长度"), TEXT("audio_end"), [](auto&, auto& Pending)
    { Pending.Sentences[0].State = EState::Streaming; Pending.Sentences[0].NextChunkIndex = 1; Pending.Sentences[0].PcmBytes = 4; });
    Reject(TEXT("音频尚未结束时不能完成整轮"), TEXT("done"), [](auto&, auto& Pending) { Pending.Sentences[0].State = EState::Streaming; });
    Reject(TEXT("完成通知不能跳过缺失的句子"), TEXT("done"), [](auto& Json, auto& Pending)
    { Json.SetNumberField(TEXT("sentences"), 2); Pending.Sentences[1].State = EState::Complete; });
    Reject(TEXT("超过截止时间的已缓冲内容也不转交"), TEXT("sentence"), [](auto&, auto& Pending) { Pending.Deadline = 0; });

    auto Id = StartRequest();
    Send(Id, Event(FGuid::NewGuid(), TEXT("audio_chunk")));
    TestTrue(TEXT("其他请求的迟到数据不会结束当前回复"), Gateway->Pending.Contains(Id));
    auto Whole = Event(Id, TEXT("sentence"));
    Whole->SetStringField(TEXT("pcm"), TEXT(""));
    Send(Id, Whole);
    TestTrue(TEXT("协商分段后仍接受旧后端的完整句子"), Gateway->Pending.Contains(Id) &&
        Gateway->Pending[Id].Sentences[0].State == EState::Whole);
    Send(Id, Event(Id, TEXT("done")));
    TestTrue(TEXT("完整句子可以按原协议结束"), Gateway->Pending.Contains(Id) && Gateway->Pending[Id].bReplyFinished);
    Gateway->Pending[Id].bHttpFinished = true;
    Gateway->Pending[Id].bHttpSucceeded = true;
    Gateway->FinishStreamRequest(Id);
    TestFalse(TEXT("HTTP 完成后释放请求"), Agent->IsDecisionPending());

    // 旧 Windows 语音采用 16 kHz，沿用原来的字节上限；新分段才固定为 24 kHz、八秒
    Id = StartRequest();
    Whole = Event(Id, TEXT("sentence"));
    TArray<uint8> LegacyPcm;
    LegacyPcm.SetNumZeroed(320000);
    Whole->SetNumberField(TEXT("sample_rate"), 16000);
    Whole->SetStringField(TEXT("pcm"), FBase64::Encode(LegacyPcm));
    Send(Id, Whole);
    TestTrue(TEXT("16 kHz 的十秒旧整句不会被新增的八秒规则拒绝"), Gateway->Pending.Contains(Id) &&
        Gateway->Pending[Id].Sentences[0].State == EState::Whole && Gateway->Pending[Id].Sentences[0].PcmBytes == LegacyPcm.Num());
    Send(Id, Event(Id, TEXT("done")));
    TestTrue(TEXT("较长的旧整句仍可以正常结束"), Gateway->Pending.Contains(Id) && Gateway->Pending[Id].bReplyFinished);

    Id = StartRequest();
    Gateway->Pending[Id].Sentences[0].State = EState::Failed;
    Gateway->Pending[Id].Sentences[1].State = EState::Complete;
    auto Done = Event(Id, TEXT("done"));
    Done->SetNumberField(TEXT("sentences"), 2);
    Send(Id, Done);
    TestTrue(TEXT("某句合成失败不阻止后续句子完成"), Gateway->Pending.Contains(Id) && Gateway->Pending[Id].bReplyFinished);
    Send(Id, Event(Id, TEXT("sentence")));
    TestFalse(TEXT("结束后的追加数据被拒绝"), Gateway->Pending.Contains(Id));
    return true;
}
#endif
