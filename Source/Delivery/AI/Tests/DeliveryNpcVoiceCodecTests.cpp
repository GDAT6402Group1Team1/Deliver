#include "Misc/AutomationTest.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "AI/Presentation/DeliveryNpcVoiceCodec.h"
#include "Async/Async.h"
#include "Audio.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    struct FBackgroundVoiceEncoding
    {
        TArray<uint8> Encoded;
        FString Error;
        double EncodeMs = 0;
        bool bSucceeded = false;
        bool bRanOnWorker = false;
    };

    TArray<uint8> MakeVoiceSignal(int32 SampleRate)
    {
        const int32 Samples = SampleRate + 73;
        TArray<uint8> Pcm;
        Pcm.SetNumUninitialized(Samples * 2);
        for (int32 Index = 0; Index < Samples; ++Index)
        {
            const double Time = double(Index) / SampleRate;
            const int16 Sample = int16(11000 * FMath::Sin(2 * PI * (220 * Time + 80 * Time * Time)) +
                3000 * FMath::Sin(2 * PI * 673 * Time));
            Pcm[Index * 2] = uint8(Sample);
            Pcm[Index * 2 + 1] = uint8(uint16(Sample) >> 8);
        }
        return Pcm;
    }

    double MeanEnergy(const TArray<uint8>& Pcm, int32 StartSample, int32 Samples)
    {
        double Energy = 0;
        for (int32 Index = StartSample; Index < StartSample + Samples; ++Index)
        {
            const int16 Sample = int16(uint16(Pcm[Index * 2]) | (uint16(Pcm[Index * 2 + 1]) << 8));
            Energy += double(Sample) * Sample;
        }
        return Energy / FMath::Max(1, Samples);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcVoiceCodecRoundTripTest, "Delivery.AI.VoiceCodec.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcVoiceCodecRoundTripTest::RunTest(const FString&)
{
    for (const int32 Rate : {16000, 24000})
    {
        const TArray<uint8> Pcm = MakeVoiceSignal(Rate);
        TArray<uint8> Encoded;
        FString Error;
        if (!TestTrue(TEXT("使用 UE Voice 编码语音"), DeliveryNpcVoice::Encode(Pcm, Rate, Encoded, Error)))
        {
            AddError(Error);
            return false;
        }
        TestTrue(TEXT("压缩后小于原 PCM 的三成"), Encoded.Num() < Pcm.Num() * .3);
        FDeliveryNpcVoiceDecoder Decoder;
        TArray<uint8> Received;
        TArray<uint8> Decoded;
        int32 Offset = 0;
        bool bDecodedBeforeComplete = false;
        // 网络数据按任意字节长度到达，完整的包先解码，余下字节留到下次
        while (Received.Num() < Encoded.Num())
        {
            const int32 Size = FMath::Min(37, Encoded.Num() - Received.Num());
            Received.Append(Encoded.GetData() + Received.Num(), Size);
            TArray<uint8> Added;
            if (!TestTrue(TEXT("分批接收仍可顺序解码"), Decoder.TryDecodeAvailable(Received, Offset, Added, Error)))
            {
                AddError(Error);
                return false;
            }
            bDecodedBeforeComplete |= !Added.IsEmpty() && Received.Num() < Encoded.Num();
            Decoded.Append(Added);
            const int32 PreviousOffset = Offset;
            TestTrue(TEXT("重复检查已收到数据仍有效"), Decoder.TryDecodeAvailable(Received, Offset, Added, Error));
            TestTrue(TEXT("已经解码的声音不会重复输出"), Added.IsEmpty());
            TestEqual(TEXT("重复检查不改变读取位置"), Offset, PreviousOffset);
        }
        TestTrue(TEXT("最后一个包到达前已经有声音可播放"), bDecodedBeforeComplete);
        TestTrue(TEXT("全部包解码完成"), Decoder.IsComplete());
        TestEqual(TEXT("保留输入采样率"), Decoder.GetSampleRate(), Rate);
        TestEqual(TEXT("保存原始声音长度"), Decoder.GetSourcePcmBytes(), Pcm.Num());
        TestEqual(TEXT("播放长度包括 40 ms 尾部静音"), Decoded.Num(), DeliveryNpcVoice::GetDecodedPcmBytes(Pcm.Num(), Rate));
        TestEqual(TEXT("读取位置停在容器末尾"), Offset, Encoded.Num());

        // 原始声音在最后一个采样仍有能量，解码后超过原长度的部分应保留延后的尾音
        const double SourceTail = MeanEnergy(Pcm, Pcm.Num() / 2 - Rate / 100, Rate / 100);
        const double DelayedTail = MeanEnergy(Decoded, Pcm.Num() / 2, Rate / 100);
        TestTrue(TEXT("编码器延后的尾音没有被原 PCM 长度裁掉"), DelayedTail > SourceTail * .05);
        AddInfo(FString::Printf(TEXT("NpcVoiceCodec rate=%d pcm=%d wire=%d decoded=%d tail_energy_ratio=%.3f"),
            Rate, Pcm.Num(), Encoded.Num(), Decoded.Num(), DelayedTail / SourceTail));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcVoiceCodecBoundariesTest, "Delivery.AI.VoiceCodec.Boundaries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcVoiceCodecBoundariesTest::RunTest(const FString&)
{
    FString Error;
    TArray<uint8> Encoded;
    TArray<uint8> Pcm = MakeVoiceSignal(16000);
    if (!TestTrue(TEXT("准备实际编码数据"), DeliveryNpcVoice::Encode(Pcm, 16000, Encoded, Error))) return false;
    const TArray<uint8> Valid = Encoded;
    for (const int32 BadByte : {0, 4, 6, 8, 12, 16, 20, 22, 24, 26})
    {
        TArray<uint8> Invalid = Valid;
        Invalid[BadByte] ^= 0xff;
        FDeliveryNpcVoiceDecoder Decoder;
        TArray<uint8> Added;
        int32 Offset = 0;
        TestFalse(FString::Printf(TEXT("拒绝损坏的头或包字段 %d"), BadByte), Decoder.TryDecodeAvailable(Invalid, Offset, Added, Error));
        TestTrue(TEXT("失败时不返回这次解出的部分声音"), Added.IsEmpty());
    }

    FDeliveryNpcVoiceDecoder Decoder;
    TArray<uint8> Received;
    TArray<uint8> Added;
    Received.Append(Valid.GetData(), 19);
    int32 Offset = 0;
    TestTrue(TEXT("不完整的头等待后续字节"), Decoder.TryDecodeAvailable(Received, Offset, Added, Error));
    TestEqual(TEXT("头未收全时不公布采样率"), Decoder.GetSampleRate(), 0);
    TestEqual(TEXT("头未收全时不推进位置"), Offset, 0);
    Received.Append(Valid.GetData() + Received.Num(), Valid.Num() - Received.Num() - 1);
    TestTrue(TEXT("缺少最后一个字节时先解码前面的包"), Decoder.TryDecodeAvailable(Received, Offset, Added, Error));
    TestFalse(TEXT("未收全时不标为播放完成"), Decoder.IsComplete());
    const int32 PreviousOffset = Offset;
    Offset = 0;
    TestFalse(TEXT("拒绝把解码位置倒退并重播旧包"), Decoder.TryDecodeAvailable(Received, Offset, Added, Error));
    Offset = PreviousOffset;
    TestFalse(TEXT("损坏后必须重新开始这一句"), Decoder.TryDecodeAvailable(Valid, Offset, Added, Error));
    Decoder.Reset();
    Offset = 0;
    TestTrue(TEXT("取消后重新创建解码状态可播放新句"), Decoder.TryDecodeAvailable(Valid, Offset, Added, Error));
    TestTrue(TEXT("新句完整解码"), Decoder.IsComplete());
    TArray<uint8> Trailing = Valid;
    Trailing.Add(0);
    TestFalse(TEXT("最后一个包之后的多余数据被拒绝"), Decoder.TryDecodeAvailable(Trailing, Offset, Added, Error));

    Pcm.Add(0);
    TestFalse(TEXT("拒绝半个 16 bit 采样"), DeliveryNpcVoice::Encode(Pcm, 16000, Encoded, Error));
    Pcm.Pop();
    TestFalse(TEXT("拒绝未约定的采样率"), DeliveryNpcVoice::Encode(Pcm, 22050, Encoded, Error));
    Pcm.SetNumZeroed(DeliveryNpcVoice::MaxPcmBytes + 2);
    TestFalse(TEXT("拒绝超过上限的原始声音"), DeliveryNpcVoice::Encode(Pcm, 16000, Encoded, Error));
    Encoded.SetNumZeroed(DeliveryNpcVoice::MaxEncodedBytes + 1);
    Decoder.Reset();
    Offset = 0;
    TestFalse(TEXT("接收数据也检查总长度上限"), Decoder.TryDecodeAvailable(Encoded, Offset, Added, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcVoiceCodecStreamingTest, "Delivery.AI.VoiceCodec.Streaming",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcVoiceCodecStreamingTest::RunTest(const FString&)
{
    for (const int32 Rate : {16000, 24000})
    {
        const auto Pcm = MakeVoiceSignal(Rate);
        FString Error;
        TArray<uint8> FullEncoded, FullDecoded;
        FDeliveryNpcVoiceDecoder FullDecoder;
        int32 FullOffset = 0;
        if (!TestTrue(TEXT("准备整句编码作为对照"), DeliveryNpcVoice::Encode(Pcm, Rate, FullEncoded, Error)) ||
            !TestTrue(TEXT("解码整句对照"), FullDecoder.TryDecodeAvailable(FullEncoded, FullOffset, FullDecoded, Error))) return false;

        FDeliveryNpcVoiceStreamEncoder Encoder;
        FDeliveryNpcVoiceDecoder Decoder;
        TArray<uint8> Wire, Received, Decoded, Part;
        int32 Offset = 0;
        bool bPlayedBeforeFinish = false;
        auto ReceiveAvailable = [&]()
        {
            while (Received.Num() < Wire.Num())
            {
                const int32 Size = FMath::Min(37, Wire.Num() - Received.Num());
                Received.Append(Wire.GetData() + Received.Num(), Size);
                TArray<uint8> Added;
                if (!Decoder.TryDecodeAvailable(Received, Offset, Added, Error)) { AddError(Error); return false; }
                Decoded.Append(Added);
            }
            return true;
        };
        if (!TestTrue(TEXT("总长未知时先发格式头"), Encoder.Start(Rate, Part, Error))) return false;
        Wire.Append(Part);
        if (!ReceiveAvailable()) return false;
        TestEqual(TEXT("开始时没有虚构声音总长"), Decoder.GetTotalPcmBytes(), 0);
        // 模拟模型回调大小不同，编码器应保留不足一包的数据
        for (int32 InputOffset = 0, Step = 0; InputOffset < Pcm.Num(); ++Step)
        {
            const int32 Sizes[] = {34, 318, 15360, 960};
            const int32 Size = FMath::Min(Sizes[Step % 4], Pcm.Num() - InputOffset);
            TArray<uint8> Chunk; Chunk.Append(Pcm.GetData() + InputOffset, Size);
            if (!TestTrue(TEXT("逐次追加原始音频"), Encoder.Append(Chunk, Part, Error))) return false;
            Wire.Append(Part); InputOffset += Size;
            if (!ReceiveAvailable()) return false;
            bPlayedBeforeFinish |= !Decoded.IsEmpty();
            TestFalse(TEXT("缓冲暂时读完不代表一句结束"), Decoder.IsComplete());
        }
        TestTrue(TEXT("模型未完成时已能解出可播放的声音"), bPlayedBeforeFinish);
        if (!TestTrue(TEXT("一句结束后补尾音和最终长度"), Encoder.Finish(Part, Error))) return false;
        Wire.Append(Part);
        if (!ReceiveAvailable()) return false;
        TestTrue(TEXT("完整结束通知确认收齐"), Decoder.FinishInput(Error));
        TestEqual(TEXT("尾部只补一次 40 ms"), Decoded.Num(), DeliveryNpcVoice::GetDecodedPcmBytes(Pcm.Num(), Rate));
        TestTrue(TEXT("不同模型块大小不改变 UE 编码后的声音"), Decoded == FullDecoded);
        TestEqual(TEXT("最终原始长度与输入相同"), Decoder.GetSourcePcmBytes(), Pcm.Num());
        const int32 PreviousOffset = Offset;
        TestTrue(TEXT("再次检查结束不重复解码"), Decoder.TryDecodeAvailable(Received, Offset, Part, Error));
        TestTrue(TEXT("没有重复尾音"), Part.IsEmpty() && PreviousOffset == Offset);
        TestFalse(TEXT("结束后拒绝继续追加原始声音"), Encoder.Append(Pcm, Part, Error));
        TestFalse(TEXT("结束通知不会发送第二遍"), Encoder.Finish(Part, Error));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcVoiceCodecStreamingBoundariesTest, "Delivery.AI.VoiceCodec.StreamingBoundaries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcVoiceCodecStreamingBoundariesTest::RunTest(const FString&)
{
    FString Error;
    TArray<uint8> Header, Body, End;
    FDeliveryNpcVoiceStreamEncoder Encoder;
    auto Pcm = MakeVoiceSignal(24000);
    if (!TestTrue(TEXT("开始分段声音"), Encoder.Start(24000, Header, Error)) ||
        !TestTrue(TEXT("编码分段声音"), Encoder.Append(Pcm, Body, Error)) ||
        !TestTrue(TEXT("结束分段声音"), Encoder.Finish(End, Error))) return false;
    TArray<uint8> Valid = Header; Valid.Append(Body); Valid.Append(End);
    const int32 EndOffset = Valid.Num() - 14;
    for (const int32 BadByte : {4, 12, 16, 24, EndOffset + 2, EndOffset + 4, EndOffset + 6, EndOffset + 10})
    {
        auto Invalid = Valid; Invalid[BadByte] ^= 0xff;
        FDeliveryNpcVoiceDecoder Decoder; TArray<uint8> Added; int32 Offset = 0;
        TestFalse(FString::Printf(TEXT("拒绝损坏的分段字段 %d"), BadByte), Decoder.TryDecodeAvailable(Invalid, Offset, Added, Error));
        TestTrue(TEXT("本次失败不返回半段声音"), Added.IsEmpty());
    }
    FDeliveryNpcVoiceDecoder Decoder;
    TArray<uint8> Added, Truncated = Valid; Truncated.SetNum(EndOffset);
    int32 Offset = 0;
    TestTrue(TEXT("缺少结束通知时仍能解码前面的声音"), Decoder.TryDecodeAvailable(Truncated, Offset, Added, Error));
    TestFalse(TEXT("断线不能把部分声音标成完整句"), Decoder.FinishInput(Error));
    TestFalse(TEXT("断线后的迟到数据不能继续播放"), Decoder.TryDecodeAvailable(Valid, Offset, Added, Error));
    Decoder.Reset(); Offset = 0;
    TestTrue(TEXT("取消后重建可处理下一句"), Decoder.TryDecodeAvailable(Valid, Offset, Added, Error));
    auto Extra = Valid; Extra.Add(0);
    TestFalse(TEXT("结束后多余数据被拒绝"), Decoder.TryDecodeAvailable(Extra, Offset, Added, Error));

    Encoder.Reset(); Encoder.Reset();
    TestTrue(TEXT("重复清理后可重新开始"), Encoder.Start(24000, Header, Error));
    TArray<uint8> OneSample; OneSample.Add(0); OneSample.Add(0);
    TestTrue(TEXT("比一帧还短的声音暂存在编码器"), Encoder.Append(OneSample, Body, Error) && Body.IsEmpty());
    TestTrue(TEXT("短句结束时发送尾音"), Encoder.Finish(End, Error));
    Valid = Header; Valid.Append(End); Decoder.Reset(); Offset = 0;
    TestTrue(TEXT("短句正常解码"), Decoder.TryDecodeAvailable(Valid, Offset, Added, Error) && Decoder.FinishInput(Error));
    TestEqual(TEXT("短句也只补一次尾音"), Added.Num(), DeliveryNpcVoice::GetDecodedPcmBytes(2, 24000));
    Encoder.Reset(); Encoder.Start(24000, Header, Error);
    Pcm.SetNumZeroed(DeliveryNpcVoice::MaxPcmBytes + 2);
    TestFalse(TEXT("累计原始声音受长度上限约束"), Encoder.Append(Pcm, Body, Error));
    TestTrue(TEXT("超过上限不发送部分包"), Body.IsEmpty());
    Encoder.Reset(); Encoder.Start(24000, Header, Error);
    TestFalse(TEXT("没有声音的结束不是成功"), Encoder.Finish(End, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcVoiceCodecRecordedSpeechTest, "Delivery.AI.VoiceCodec.RecordedSpeech",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryNpcVoiceCodecRecordedSpeechTest::RunTest(const FString&)
{
    const FString Folder = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AIResearch"));
    TArray<TSharedPtr<FJsonValue>> Results;
    for (const FString Voice : {FString(TEXT("male")), FString(TEXT("female"))})
    {
        const FString InputPath = FPaths::Combine(Folder, TEXT("npc-voice-automatic-") + Voice + TEXT(".wav"));
        TArray<uint8> Wave;
        if (!FFileHelper::LoadFileToArray(Wave, *InputPath))
        {
            AddWarning(TEXT("本机语音样本不存在，本次未测该声音：") + InputPath);
            continue;
        }
        FWaveModInfo Info;
        if (!TestTrue(TEXT("读取已保存的实际 TTS 文件"), Info.ReadWaveInfo(Wave.GetData(), Wave.Num()))) return false;
        if (!TestEqual(TEXT("TTS 使用单声道"), int32(*Info.pChannels), 1) ||
            !TestEqual(TEXT("TTS 使用 16 bit PCM"), int32(*Info.pBitsPerSample), 16)) return false;
        const int32 Rate = *Info.pSamplesPerSec;
        TArray<uint8> Pcm;
        Pcm.Append(Info.SampleDataStart, Info.SampleDataSize);
        TArray<uint8> Encoded;
        FString Error;
        const double EncodeStarted = FPlatformTime::Seconds();
        if (!TestTrue(TEXT("编码实际 TTS 声音"), DeliveryNpcVoice::Encode(Pcm, Rate, Encoded, Error)))
        {
            AddError(Error);
            return false;
        }
        const double EncodeMs = (FPlatformTime::Seconds() - EncodeStarted) * 1000;

        // 同步编码已经在游戏线程准备语音模块，后台任务只保存自己的音频数据
        const double SubmitStarted = FPlatformTime::Seconds();
        auto Background = Async(EAsyncExecution::ThreadPool, [Samples = Pcm, Rate]()
        {
            FBackgroundVoiceEncoding Result;
            Result.bRanOnWorker = !IsInGameThread();
            const double Started = FPlatformTime::Seconds();
            Result.bSucceeded = DeliveryNpcVoice::Encode(Samples, Rate, Result.Encoded, Result.Error);
            Result.EncodeMs = (FPlatformTime::Seconds() - Started) * 1000;
            return Result;
        });
        const double SubmitMs = (FPlatformTime::Seconds() - SubmitStarted) * 1000;
        // 测试最多等待五秒；任务持有数据副本，超时返回后不会访问测试对象或局部引用
        if (!TestTrue(TEXT("后台编码在五秒内完成"), Background.WaitFor(FTimespan::FromSeconds(5)))) return false;
        const double CompletionMs = (FPlatformTime::Seconds() - SubmitStarted) * 1000;
        const FBackgroundVoiceEncoding BackgroundResult = Background.Get();
        if (!TestTrue(TEXT("后台编码实际 TTS 声音"), BackgroundResult.bSucceeded))
        {
            AddError(BackgroundResult.Error);
            return false;
        }
        TestTrue(TEXT("编码任务在线程池执行"), BackgroundResult.bRanOnWorker);
        const bool bMatchingEncoding = BackgroundResult.Encoded == Encoded;
        TestTrue(TEXT("同一声音在线程池与游戏线程生成相同编码数据"), bMatchingEncoding);

        FDeliveryNpcVoiceDecoder Decoder;
        TArray<uint8> Decoded;
        int32 Offset = 0;
        const double DecodeStarted = FPlatformTime::Seconds();
        if (!TestTrue(TEXT("解码实际 TTS 声音"), Decoder.TryDecodeAvailable(Encoded, Offset, Decoded, Error)))
        {
            AddError(Error);
            return false;
        }
        const double DecodeMs = (FPlatformTime::Seconds() - DecodeStarted) * 1000;
        TestTrue(TEXT("实际语音全部解码"), Decoder.IsComplete());
        TestEqual(TEXT("实际语音包含尾音补偿长度"), Decoded.Num(), DeliveryNpcVoice::GetDecodedPcmBytes(Pcm.Num(), Rate));
        TestTrue(TEXT("实际语音压缩后明显小于 PCM"), Encoded.Num() < Pcm.Num() * .3);
        TArray<uint8> DecodedWave;
        SerializeWaveFile(DecodedWave, Decoded.GetData(), Decoded.Num(), 1, Rate);
        const FString OutputPath = FPaths::Combine(Folder, TEXT("npc-voice-opus-") + Voice + TEXT(".wav"));
        TestTrue(TEXT("保存解码声音供实际听取"), FFileHelper::SaveArrayToFile(DecodedWave, *OutputPath));

        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("voice"), Voice);
        Row->SetStringField(TEXT("input"), InputPath);
        Row->SetStringField(TEXT("decoded_wav"), OutputPath);
        Row->SetNumberField(TEXT("sample_rate"), Rate);
        Row->SetNumberField(TEXT("pcm_bytes"), Pcm.Num());
        Row->SetNumberField(TEXT("encoded_bytes"), Encoded.Num());
        Row->SetNumberField(TEXT("decoded_bytes"), Decoded.Num());
        Row->SetNumberField(TEXT("source_seconds"), double(Pcm.Num()) / (Rate * 2));
        Row->SetNumberField(TEXT("playback_seconds"), double(Decoded.Num()) / (Rate * 2));
        Row->SetNumberField(TEXT("encode_ms"), EncodeMs);
        Row->SetNumberField(TEXT("decode_ms"), DecodeMs);
        Row->SetNumberField(TEXT("threadpool_submit_ms"), SubmitMs);
        Row->SetNumberField(TEXT("threadpool_encode_ms"), BackgroundResult.EncodeMs);
        Row->SetNumberField(TEXT("threadpool_completion_ms"), CompletionMs);
        Row->SetBoolField(TEXT("threadpool_ran_on_worker"), BackgroundResult.bRanOnWorker);
        Row->SetBoolField(TEXT("threadpool_matches_sync"), bMatchingEncoding);
        Results.Add(MakeShared<FJsonValueObject>(Row));
        AddInfo(FString::Printf(TEXT("NpcVoiceCodec voice=%s rate=%d pcm=%d wire=%d decoded=%d encode_ms=%.3f decode_ms=%.3f"),
            *Voice, Rate, Pcm.Num(), Encoded.Num(), Decoded.Num(), EncodeMs, DecodeMs));
        AddInfo(FString::Printf(TEXT("NpcVoiceCodecThreadPool voice=%s submit_ms=%.3f encode_ms=%.3f completion_ms=%.3f matches_sync=%d"),
            *Voice, SubmitMs, BackgroundResult.EncodeMs, CompletionMs, bMatchingEncoding));
    }
    if (!Results.IsEmpty())
    {
        auto Report = MakeShared<FJsonObject>();
        Report->SetStringField(TEXT("codec"), TEXT("UE Voice Opus, 24 kbps CBR, mono"));
        Report->SetStringField(TEXT("measurement"), TEXT("每份语音各做一次同步和线程池编码；submit 包含复制音频与提交任务，completion 包含测试等待。该数据不代表实际地图的帧率变化。Opus 有损，输出增加 40 ms 尾音补偿"));
        Report->SetArrayField(TEXT("samples"), Results);
        FString Json;
        FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
        TestTrue(TEXT("保存本次压缩大小和耗时"), FFileHelper::SaveStringToFile(Json,
            *FPaths::Combine(Folder, TEXT("npc-voice-codec-results.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    }
    return true;
}
#endif
