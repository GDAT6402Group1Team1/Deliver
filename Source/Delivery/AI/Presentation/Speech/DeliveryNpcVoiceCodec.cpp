#include "AI/Presentation/Speech/DeliveryNpcVoiceCodec.h"

#include "Interfaces/VoiceCodec.h"
#include "VoiceModule.h"

// ── 数据格式 ──
// 压缩后的一句话是一段字节：先是 20 字节的格式头，后面跟着一个个数据包
// 格式头依次是：标记 DNV1（4 字节）、格式版本（2）、声道数 1（2）、采样率（4）、原始声音字节数（4）、解压后总字节数（4）
// 每个数据包依次是：压缩后的长度（2）、这包解压后实际有几字节声音（2）、包的序号（2），然后是 Opus 压缩数据
// 一包装 4 帧，每帧 20 毫秒，所以一包是 80 毫秒的声音；最后一包不满 80 毫秒时，压缩前用无声数据补满整帧，解压后再裁掉
// 边收边压的格式（StreamingFormatVersion）开始时还不知道这句多长，格式头里两个长度写 0
// 结尾另加一个压缩长度为 0 的结束包，后面跟 8 字节：原始声音字节数和解压后总字节数
namespace
{
    /// 格式头开头的标记，按小端字节顺序读出来是 DNV1
    constexpr uint32 Magic = 0x31564E44; // DNV1

    /// 格式头的字节数
    constexpr int32 HeaderBytes = 20;

    /// 每个数据包前面那三项（压缩长度、实际长度、序号）的字节数
    constexpr int32 PacketHeaderBytes = 6;

    /// 一包装几帧；每帧 20 毫秒
    constexpr int32 FramesPerPacket = 4;

    /// 一包压缩数据最多多少字节，超过说明压缩出错
    constexpr int32 MaxPacketBytes = 2048;

    /// 压缩的码率，每秒 24000 位，也就是每秒 3 KB 左右
    constexpr int32 Bitrate = 24000;

    /// @brief 只支持每秒 16000 或 24000 个采样点
    bool IsSampleRateSupported(int32 SampleRate)
    {
        return SampleRate == 16000 || SampleRate == 24000;
    }

    /// @brief 按小端顺序（低字节在前）写 2 字节整数
    void Write16(TArray<uint8>& Data, uint16 Value)
    {
        Data.Add(uint8(Value));
        Data.Add(uint8(Value >> 8));
    }

    /// @brief 按小端顺序写 4 字节整数
    void Write32(TArray<uint8>& Data, uint32 Value)
    {
        Write16(Data, uint16(Value));
        Write16(Data, uint16(Value >> 16));
    }

    /// @brief 按小端顺序读 2 字节整数
    uint16 Read16(const uint8* Data)
    {
        return uint16(Data[0]) | (uint16(Data[1]) << 8);
    }

    /// @brief 按小端顺序读 4 字节整数
    uint32 Read32(const uint8* Data)
    {
        return uint32(Read16(Data)) | (uint32(Read16(Data + 2)) << 16);
    }
}

/// @brief 原始声音加上句尾 0.04 秒无声数据后的字节数；参数不合法时返回 0
/// 0.04 秒的字节数 = 每秒采样数 × 2 字节 × 0.04，也就是 SampleRate * 2 / 25
int32 DeliveryNpcVoice::GetDecodedPcmBytes(int32 SourcePcmBytes, int32 SampleRate)
{
    if (!IsSampleRateSupported(SampleRate) || SourcePcmBytes <= 0 || SourcePcmBytes > GetMaxPcmBytes(SampleRate) || SourcePcmBytes % 2)
    {
        return 0;
    }
    return SourcePcmBytes + SampleRate * 2 / 25;
}

// ── 一次压完 ──
// 格式版本 1：整句声音已经收齐时，Encode 一次写出格式头和全部数据包

/// @brief 把一整句原始声音压缩成上面的格式；失败时 OutEncoded 为空，OutError 写原因
bool DeliveryNpcVoice::Encode(const TArray<uint8>& Pcm, int32 SampleRate, TArray<uint8>& OutEncoded, FString& OutError)
{
    OutEncoded.Reset();
    OutError.Reset();
    const int32 TotalPcmBytes = GetDecodedPcmBytes(Pcm.Num(), SampleRate);
    if (TotalPcmBytes == 0)
    {
        OutError = TEXT("Invalid NPC voice PCM size or sample rate");
        return false;
    }

    // 用 UE 语音模块的 Opus 编码器：固定码率、复杂度 3（压缩速度和音质的折中），不打开麦克风
    const TSharedPtr<IVoiceEncoder> Encoder = FVoiceModule::Get().CreateVoiceEncoder(SampleRate, 1, EAudioEncodeHint::VoiceEncode_Voice);
    if (!Encoder || !Encoder->SetBitrate(Bitrate) || !Encoder->SetVBR(false) || !Encoder->SetComplexity(3))
    {
        OutError = TEXT("UE voice encoder is unavailable");
        return false;
    }

    Write32(OutEncoded, Magic);
    Write16(OutEncoded, FormatVersion);
    Write16(OutEncoded, 1);
    Write32(OutEncoded, SampleRate);
    Write32(OutEncoded, Pcm.Num());
    Write32(OutEncoded, TotalPcmBytes);

    // 一帧 20 毫秒的字节数 = 每秒采样数 × 2 字节 ÷ 50
    const int32 FrameBytes = SampleRate * 2 / 50;
    const int32 PacketPcmBytes = FrameBytes * FramesPerPacket;
    TArray<uint8> Input;
    TArray<uint8> Packet;
    Packet.SetNumUninitialized(MaxPacketBytes);
    int32 PacketIndex = 0;
    for (int32 Offset = 0; Offset < TotalPcmBytes; ++PacketIndex)
    {
        const int32 ActualBytes = FMath::Min(PacketPcmBytes, TotalPcmBytes - Offset);
        const int32 PaddedBytes = FMath::DivideAndRoundUp(ActualBytes, FrameBytes) * FrameBytes;
        Input.SetNumZeroed(PaddedBytes);
        FMemory::Memzero(Input.GetData(), Input.Num());
        const int32 CopyBytes = FMath::Clamp(Pcm.Num() - Offset, 0, ActualBytes);
        if (CopyBytes > 0)
        {
            FMemory::Memcpy(Input.GetData(), Pcm.GetData() + Offset, CopyBytes);
        }

        uint32 PacketBytes = Packet.Num();
        const int32 RemainingBytes = Encoder->Encode(Input.GetData(), Input.Num(), Packet.GetData(), PacketBytes);
        if (RemainingBytes != 0 || PacketBytes == 0 || PacketBytes > MaxPacketBytes ||
            OutEncoded.Num() + PacketHeaderBytes + int32(PacketBytes) > MaxEncodedBytes)
        {
            OutEncoded.Reset();
            OutError = TEXT("UE voice encoder returned an invalid packet");
            return false;
        }

        // 写这一包的三项，再写压缩数据；ActualBytes 是这包真正的声音长度，补满整帧的无声部分解压后裁掉
        Write16(OutEncoded, uint16(PacketBytes));
        Write16(OutEncoded, uint16(ActualBytes));
        Write16(OutEncoded, uint16(PacketIndex));
        OutEncoded.Append(Packet.GetData(), PacketBytes);
        Offset += ActualBytes;
    }
    return true;
}

// ── 边收边压 ──
// 服务器每收到一段后端合成的声音就调用 Append，凑够一包（80 毫秒）就压缩并输出，可以马上发给客户端
// 整句收完调用 Finish：句尾补 0.04 秒无声数据，压完剩下的声音，再写结束包；任何一步出错都调用 Fail，之后这句不能再用

/// @brief 记下这句已经失败，丢掉编码器和没压的声音，清空输出并写原因；返回 false
bool FDeliveryNpcVoiceStreamEncoder::Fail(const TCHAR* Reason, TArray<uint8>& OutEncoded, FString& OutError)
{
    bFailed = true;
    Encoder.Reset();
    PendingPcm.Reset();
    OutEncoded.Reset();
    OutError = Reason;
    return false;
}

/// @brief 开始新的一句：建好 Opus 编码器，输出格式头；采样率不支持或已经开始过时调用 Fail
bool FDeliveryNpcVoiceStreamEncoder::Start(int32 InSampleRate, TArray<uint8>& OutEncoded, FString& OutError)
{
    OutEncoded.Reset(); OutError.Reset();
    if (bStarted || bFailed || !IsSampleRateSupported(InSampleRate))
        return Fail(TEXT("Invalid NPC voice stream start"), OutEncoded, OutError);
    Encoder = FVoiceModule::Get().CreateVoiceEncoder(InSampleRate, 1, EAudioEncodeHint::VoiceEncode_Voice);
    if (!Encoder || !Encoder->SetBitrate(Bitrate) || !Encoder->SetVBR(false) || !Encoder->SetComplexity(3))
        return Fail(TEXT("UE voice encoder is unavailable"), OutEncoded, OutError);
    SampleRate = InSampleRate;
    bStarted = true;
    Write32(OutEncoded, Magic);
    Write16(OutEncoded, DeliveryNpcVoice::StreamingFormatVersion);
    Write16(OutEncoded, 1);
    Write32(OutEncoded, SampleRate);

    // 两个长度现在还不知道，先写 0；Finish 在结束包里写真正的长度
    Write32(OutEncoded, 0);
    Write32(OutEncoded, 0);
    EncodedBytes = OutEncoded.Num();
    return true;
}

/// @brief 收到一段原始声音：放进 PendingPcm，再调用 Flush(false) 压出所有已经凑满的整包
/// 声音长度为奇数（不是整数个采样点）或者总长超过 12 秒时调用 Fail
bool FDeliveryNpcVoiceStreamEncoder::Append(const TArray<uint8>& Pcm, TArray<uint8>& OutEncoded, FString& OutError)
{
    OutEncoded.Reset(); OutError.Reset();
    if (!bStarted || bFinished || bFailed || Pcm.IsEmpty() || Pcm.Num() % 2 ||
        Pcm.Num() > DeliveryNpcVoice::GetMaxPcmBytes(SampleRate) - SourcePcmBytes)
        return Fail(TEXT("Invalid NPC voice stream PCM"), OutEncoded, OutError);
    SourcePcmBytes += Pcm.Num();
    PendingPcm.Append(Pcm);
    return Flush(false, OutEncoded, OutError);
}

/// @brief 把 PendingPcm 里凑满 80 毫秒的部分逐包压缩输出；bFinal 为 true 时不满一包的剩余部分也压掉
/// 压完的部分从 PendingPcm 前面删掉，没凑满的留到下次
bool FDeliveryNpcVoiceStreamEncoder::Flush(bool bFinal, TArray<uint8>& OutEncoded, FString& OutError)
{
    const int32 FrameBytes = SampleRate * 2 / 50;
    const int32 PacketPcmBytes = FrameBytes * FramesPerPacket;
    int32 Offset = 0;
    while (PendingPcm.Num() - Offset >= PacketPcmBytes || (bFinal && Offset < PendingPcm.Num()))
    {
        const int32 ActualBytes = FMath::Min(PacketPcmBytes, PendingPcm.Num() - Offset);
        TArray<uint8> Input;
        Input.SetNumZeroed(FMath::DivideAndRoundUp(ActualBytes, FrameBytes) * FrameBytes);
        FMemory::Memcpy(Input.GetData(), PendingPcm.GetData() + Offset, ActualBytes);
        TArray<uint8> Packet;
        Packet.SetNumUninitialized(MaxPacketBytes);
        uint32 PacketBytes = Packet.Num();
        const int32 Remaining = Encoder->Encode(Input.GetData(), Input.Num(), Packet.GetData(), PacketBytes);
        if (Remaining != 0 || PacketBytes == 0 || PacketBytes > MaxPacketBytes ||
            EncodedBytes + PacketHeaderBytes + int32(PacketBytes) > DeliveryNpcVoice::MaxEncodedBytes)
            return Fail(TEXT("UE voice stream encoder returned an invalid packet"), OutEncoded, OutError);
        Write16(OutEncoded, uint16(PacketBytes));
        Write16(OutEncoded, uint16(ActualBytes));
        Write16(OutEncoded, uint16(PacketIndex++));
        OutEncoded.Append(Packet.GetData(), PacketBytes);
        EncodedBytes += PacketHeaderBytes + int32(PacketBytes);
        Offset += ActualBytes;
    }
    PendingPcm.RemoveAt(0, Offset, EAllowShrinking::No);
    return true;
}

/// @brief 这句收完了：句尾补 0.04 秒无声数据，调用 Flush(true) 压完剩下的声音，再写结束包和两个长度
/// 之后丢掉编码器；没开始、已经结束、已经失败或一段声音都没收到时调用 Fail
bool FDeliveryNpcVoiceStreamEncoder::Finish(TArray<uint8>& OutEncoded, FString& OutError)
{
    OutEncoded.Reset(); OutError.Reset();
    if (!bStarted || bFinished || bFailed || SourcePcmBytes == 0)
        return Fail(TEXT("Invalid NPC voice stream finish"), OutEncoded, OutError);
    const int32 TotalPcmBytes = DeliveryNpcVoice::GetDecodedPcmBytes(SourcePcmBytes, SampleRate);

    // 无声数据只在整句的末尾补一次；Append 收到的每段声音后面都不补，否则句子中间会多出停顿
    PendingPcm.AddZeroed(TotalPcmBytes - SourcePcmBytes);
    if (!Flush(true, OutEncoded, OutError)) return false;
    if (EncodedBytes + PacketHeaderBytes + 8 > DeliveryNpcVoice::MaxEncodedBytes)
        return Fail(TEXT("NPC voice stream exceeds its size limit"), OutEncoded, OutError);
    Write16(OutEncoded, 0);
    Write16(OutEncoded, 0);
    Write16(OutEncoded, uint16(PacketIndex));
    Write32(OutEncoded, SourcePcmBytes);
    Write32(OutEncoded, TotalPcmBytes);
    EncodedBytes += PacketHeaderBytes + 8;
    bFinished = true;
    Encoder.Reset();
    return true;
}

/// @brief 丢掉编码器和没压的声音，回到没开始的状态，可以重复调用
void FDeliveryNpcVoiceStreamEncoder::Reset()
{
    Encoder.Reset(); PendingPcm.Reset();
    SampleRate = SourcePcmBytes = EncodedBytes = PacketIndex = 0;
    bStarted = bFinished = bFailed = false;
}

// ── 解压 ──
// 客户端每收到一段数据，就把这句从头到现在收到的全部字节交给 TryDecodeAvailable
// 它从上次解到的位置 ReadOffset 接着往下读，凑齐一整包就解压一包，返回新解出的声音；不完整的包留到下次
// 服务器说这句发完了，调用 FinishInput 确认已经解到结尾；格式头、序号、长度任何一处不对都调用 Fail

/// @brief 记下这句已经失败，丢掉解码器，清空输出并写原因；返回 false
bool FDeliveryNpcVoiceDecoder::Fail(const TCHAR* Reason, TArray<uint8>& OutPcm, FString& OutError)
{
    bFailed = true;
    Decoder.Reset();
    OutPcm.Reset();
    OutError = Reason;
    return false;
}

/// @brief 从 ReadOffset 起解出目前能解的完整数据包，追加到 OutPcm
/// @param Encoded 这句从开头到现在收到的全部字节
/// @param InOutOffset 调用方记的已读位置，必须等于 ReadOffset；解完后更新
/// @return 数据还不够一包时也返回 true；格式错误时返回 false
bool FDeliveryNpcVoiceDecoder::TryDecodeAvailable(const TArray<uint8>& Encoded, int32& InOutOffset, TArray<uint8>& OutPcm, FString& OutError)
{
    OutPcm.Reset();
    OutError.Reset();
    if (bFailed || InOutOffset != ReadOffset || ReadOffset < 0 || ReadOffset > Encoded.Num() ||
        Encoded.Num() > DeliveryNpcVoice::MaxEncodedBytes)
    {
        return Fail(TEXT("NPC voice data is out of order or exceeds its size limit"), OutPcm, OutError);
    }

    // 第一次调用时读格式头：标记、声道、采样率都要对，版本 1 的两个长度要对得上，边收边压版本的两个长度要是 0
    // 读完建好 Opus 解码器，ReadOffset 移到格式头后面
    if (!Decoder)
    {
        if (Encoded.Num() < HeaderBytes)
        {
            return true;
        }
        const uint8* Header = Encoded.GetData();
        const uint32 Rate = Read32(Header + 8);
        const uint32 SourceBytes = Read32(Header + 12);
        const uint32 TotalBytes = Read32(Header + 16);
        Version = Read16(Header + 4);
        const bool bCompleteHeader = Version == DeliveryNpcVoice::FormatVersion && TotalBytes > 0 &&
            SourceBytes <= DeliveryNpcVoice::MaxPcmBytes && TotalBytes <= DeliveryNpcVoice::MaxDecodedPcmBytes &&
            int32(TotalBytes) == DeliveryNpcVoice::GetDecodedPcmBytes(int32(SourceBytes), int32(Rate));
        const bool bStreamingHeader = Version == DeliveryNpcVoice::StreamingFormatVersion && SourceBytes == 0 && TotalBytes == 0;
        if (Read32(Header) != Magic || Read16(Header + 6) != 1 || Rate > MAX_int32 ||
            !IsSampleRateSupported(int32(Rate)) || (!bCompleteHeader && !bStreamingHeader))
        {
            return Fail(TEXT("Invalid NPC voice header"), OutPcm, OutError);
        }
        Decoder = FVoiceModule::Get().CreateVoiceDecoder(Rate, 1);
        if (!Decoder)
        {
            return Fail(TEXT("UE voice decoder is unavailable"), OutPcm, OutError);
        }
        SampleRate = Rate;
        SourcePcmBytes = SourceBytes;
        TotalPcmBytes = TotalBytes;
        InOutOffset = ReadOffset = HeaderBytes;
    }

    const int32 FrameBytes = SampleRate * 2 / 50;
    const int32 PacketPcmBytes = FrameBytes * FramesPerPacket;
    while (ReadOffset < Encoded.Num() && !IsComplete())
    {
        if (Encoded.Num() - ReadOffset < PacketHeaderBytes)
        {
            return true;
        }
        const uint8* Header = Encoded.GetData() + ReadOffset;
        const int32 PacketBytes = Read16(Header);
        const int32 ActualBytes = Read16(Header + 2);

        // 压缩长度为 0 的是边收边压格式的结束包：核对序号，读出两个长度，和已经解出的字节数比对，然后停止
        if (Version == DeliveryNpcVoice::StreamingFormatVersion && PacketBytes == 0)
        {
            if (ActualBytes != 0 || Read16(Header + 4) != PacketIndex)
                return Fail(TEXT("Invalid NPC voice stream end order"), OutPcm, OutError);
            if (Encoded.Num() - ReadOffset < PacketHeaderBytes + 8) return true;
            const uint32 SourceBytes = Read32(Header + PacketHeaderBytes);
            const uint32 TotalBytes = Read32(Header + PacketHeaderBytes + 4);
            if (SourceBytes == 0 || SourceBytes > DeliveryNpcVoice::MaxPcmBytes ||
                TotalBytes > DeliveryNpcVoice::MaxDecodedPcmBytes || TotalBytes != uint32(DecodedPcmBytes) ||
                int32(TotalBytes) != DeliveryNpcVoice::GetDecodedPcmBytes(int32(SourceBytes), SampleRate))
                return Fail(TEXT("Invalid NPC voice stream final length"), OutPcm, OutError);
            SourcePcmBytes = SourceBytes; TotalPcmBytes = TotalBytes;
            InOutOffset = ReadOffset = ReadOffset + PacketHeaderBytes + 8;
            break;
        }

        // 检查这包的实际长度：版本 1 里除最后一包外都是整整 80 毫秒；边收边压格式里，只有最后一包可以不满，之前出现过不满的包就是错的
        const bool bValidLength = Version == DeliveryNpcVoice::FormatVersion
            ? ActualBytes == FMath::Min(PacketPcmBytes, TotalPcmBytes - DecodedPcmBytes)
            : !bSawPartialPacket && ActualBytes > 0 && ActualBytes <= PacketPcmBytes && ActualBytes % 2 == 0 &&
              ActualBytes <= DeliveryNpcVoice::GetDecodedPcmBytes(DeliveryNpcVoice::GetMaxPcmBytes(SampleRate), SampleRate) - DecodedPcmBytes;
        if (PacketBytes < 4 || PacketBytes > MaxPacketBytes || Read16(Header + 4) != PacketIndex ||
            !bValidLength)
        {
            return Fail(TEXT("Invalid NPC voice packet size or sequence"), OutPcm, OutError);
        }

        // 这包的压缩数据还没收全，等下次
        if (Encoded.Num() - ReadOffset - PacketHeaderBytes < PacketBytes)
        {
            return true;
        }

        // UE 的 Opus 数据第一个字节是这包有几帧，要和实际长度算出的帧数一致
        const uint8* Packet = Header + PacketHeaderBytes;
        const int32 FrameCount = FMath::DivideAndRoundUp(ActualBytes, FrameBytes);
        if (Packet[0] != FrameCount || PacketBytes < 2 + FrameCount * 2)
        {
            return Fail(TEXT("Invalid UE voice frame count"), OutPcm, OutError);
        }

        // UE 的解码器要求输出缓冲至少比实际帧数多留 6 帧的空间，解完再确认返回的长度正好是这些帧
        TArray<uint8> Decoded;
        Decoded.SetNumUninitialized((FramesPerPacket + 6) * FrameBytes);
        uint32 DecodedBytes = Decoded.Num();
        Decoder->Decode(Packet, PacketBytes, Decoded.GetData(), DecodedBytes);
        if (DecodedBytes != uint32(FrameCount * FrameBytes))
        {
            return Fail(TEXT("UE voice decoder returned an invalid PCM length"), OutPcm, OutError);
        }

        // 只取 ActualBytes 字节，补满整帧的无声部分丢掉
        OutPcm.Append(Decoded.GetData(), ActualBytes);
        bSawPartialPacket = ActualBytes < PacketPcmBytes;
        DecodedPcmBytes += ActualBytes;
        InOutOffset = ReadOffset = ReadOffset + PacketHeaderBytes + PacketBytes;
        ++PacketIndex;
    }

    // 已经解完整句，后面却还有数据，说明数据有错
    if (IsComplete() && ReadOffset != Encoded.Num())
    {
        return Fail(TEXT("Unexpected data after the last NPC voice packet"), OutPcm, OutError);
    }
    return true;
}

/// @brief 服务器说这句发完了时调用：已经解完整句返回 true，否则调用 Fail，说明中途有数据丢了
bool FDeliveryNpcVoiceDecoder::FinishInput(FString& OutError)
{
    OutError.Reset();
    if (IsComplete()) return true;
    TArray<uint8> Ignored;
    return Fail(TEXT("NPC voice input ended before the final packet"), Ignored, OutError);
}

/// @brief 丢掉解码器，清空所有位置和长度，下一句从第一个字节开始
void FDeliveryNpcVoiceDecoder::Reset()
{
    Decoder.Reset();
    SampleRate = SourcePcmBytes = TotalPcmBytes = DecodedPcmBytes = ReadOffset = PacketIndex = 0;
    Version = 0;
    bFailed = bSawPartialPacket = false;
}
