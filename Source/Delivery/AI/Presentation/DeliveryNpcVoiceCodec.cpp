#include "DeliveryNpcVoiceCodec.h"

#include "Interfaces/VoiceCodec.h"
#include "VoiceModule.h"

namespace
{
    constexpr uint32 Magic = 0x31564E44; // DNV1
    constexpr int32 HeaderBytes = 20;
    constexpr int32 PacketHeaderBytes = 6;
    constexpr int32 FramesPerPacket = 4;
    constexpr int32 MaxPacketBytes = 2048;
    constexpr int32 Bitrate = 24000;

    bool IsSampleRateSupported(int32 SampleRate)
    {
        return SampleRate == 16000 || SampleRate == 24000;
    }

    void Write16(TArray<uint8>& Data, uint16 Value)
    {
        Data.Add(uint8(Value));
        Data.Add(uint8(Value >> 8));
    }

    void Write32(TArray<uint8>& Data, uint32 Value)
    {
        Write16(Data, uint16(Value));
        Write16(Data, uint16(Value >> 16));
    }

    uint16 Read16(const uint8* Data)
    {
        return uint16(Data[0]) | (uint16(Data[1]) << 8);
    }

    uint32 Read32(const uint8* Data)
    {
        return uint32(Read16(Data)) | (uint32(Read16(Data + 2)) << 16);
    }
}

int32 DeliveryNpcVoice::GetDecodedPcmBytes(int32 SourcePcmBytes, int32 SampleRate)
{
    if (!IsSampleRateSupported(SampleRate) || SourcePcmBytes <= 0 || SourcePcmBytes > MaxPcmBytes || SourcePcmBytes % 2)
    {
        return 0;
    }
    return SourcePcmBytes + SampleRate * 2 / 25;
}

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

    // 只使用 UE 的编码器，不创建麦克风采集对象
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

        // 每包记录压缩长度、实际播放长度和顺序，最后一包补齐的帧在解码后裁去
        Write16(OutEncoded, uint16(PacketBytes));
        Write16(OutEncoded, uint16(ActualBytes));
        Write16(OutEncoded, uint16(PacketIndex));
        OutEncoded.Append(Packet.GetData(), PacketBytes);
        Offset += ActualBytes;
    }
    return true;
}

bool FDeliveryNpcVoiceStreamEncoder::Fail(const TCHAR* Reason, TArray<uint8>& OutEncoded, FString& OutError)
{
    bFailed = true;
    Encoder.Reset();
    PendingPcm.Reset();
    OutEncoded.Reset();
    OutError = Reason;
    return false;
}

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
    // 总长由 Finish 的结束通知给出，开始时用 0 表示尚未确定
    Write32(OutEncoded, 0);
    Write32(OutEncoded, 0);
    EncodedBytes = OutEncoded.Num();
    return true;
}

bool FDeliveryNpcVoiceStreamEncoder::Append(const TArray<uint8>& Pcm, TArray<uint8>& OutEncoded, FString& OutError)
{
    OutEncoded.Reset(); OutError.Reset();
    if (!bStarted || bFinished || bFailed || Pcm.IsEmpty() || Pcm.Num() % 2 ||
        Pcm.Num() > DeliveryNpcVoice::MaxPcmBytes - SourcePcmBytes)
        return Fail(TEXT("Invalid NPC voice stream PCM"), OutEncoded, OutError);
    SourcePcmBytes += Pcm.Num();
    PendingPcm.Append(Pcm);
    return Flush(false, OutEncoded, OutError);
}

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

bool FDeliveryNpcVoiceStreamEncoder::Finish(TArray<uint8>& OutEncoded, FString& OutError)
{
    OutEncoded.Reset(); OutError.Reset();
    if (!bStarted || bFinished || bFailed || SourcePcmBytes == 0)
        return Fail(TEXT("Invalid NPC voice stream finish"), OutEncoded, OutError);
    const int32 TotalPcmBytes = DeliveryNpcVoice::GetDecodedPcmBytes(SourcePcmBytes, SampleRate);
    // 每句话最后补 40 ms，不能在每个模型音频块后重复补静音
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

void FDeliveryNpcVoiceStreamEncoder::Reset()
{
    Encoder.Reset(); PendingPcm.Reset();
    SampleRate = SourcePcmBytes = EncodedBytes = PacketIndex = 0;
    bStarted = bFinished = bFailed = false;
}

bool FDeliveryNpcVoiceDecoder::Fail(const TCHAR* Reason, TArray<uint8>& OutPcm, FString& OutError)
{
    bFailed = true;
    Decoder.Reset();
    OutPcm.Reset();
    OutError = Reason;
    return false;
}

bool FDeliveryNpcVoiceDecoder::TryDecodeAvailable(const TArray<uint8>& Encoded, int32& InOutOffset, TArray<uint8>& OutPcm, FString& OutError)
{
    OutPcm.Reset();
    OutError.Reset();
    if (bFailed || InOutOffset != ReadOffset || ReadOffset < 0 || ReadOffset > Encoded.Num() ||
        Encoded.Num() > DeliveryNpcVoice::MaxEncodedBytes)
    {
        return Fail(TEXT("NPC voice data is out of order or exceeds its size limit"), OutPcm, OutError);
    }

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
        const bool bValidLength = Version == DeliveryNpcVoice::FormatVersion
            ? ActualBytes == FMath::Min(PacketPcmBytes, TotalPcmBytes - DecodedPcmBytes)
            : !bSawPartialPacket && ActualBytes > 0 && ActualBytes <= PacketPcmBytes && ActualBytes % 2 == 0 &&
              ActualBytes <= DeliveryNpcVoice::MaxDecodedPcmBytes - DecodedPcmBytes;
        if (PacketBytes < 4 || PacketBytes > MaxPacketBytes || Read16(Header + 4) != PacketIndex ||
            !bValidLength)
        {
            return Fail(TEXT("Invalid NPC voice packet size or sequence"), OutPcm, OutError);
        }
        if (Encoded.Num() - ReadOffset - PacketHeaderBytes < PacketBytes)
        {
            return true;
        }

        const uint8* Packet = Header + PacketHeaderBytes;
        const int32 FrameCount = FMath::DivideAndRoundUp(ActualBytes, FrameBytes);
        if (Packet[0] != FrameCount || PacketBytes < 2 + FrameCount * 2)
        {
            return Fail(TEXT("Invalid UE voice frame count"), OutPcm, OutError);
        }
        // UE 解码器在每帧开始前要求留出六帧空间，再检查实际返回的长度
        TArray<uint8> Decoded;
        Decoded.SetNumUninitialized((FramesPerPacket + 6) * FrameBytes);
        uint32 DecodedBytes = Decoded.Num();
        Decoder->Decode(Packet, PacketBytes, Decoded.GetData(), DecodedBytes);
        if (DecodedBytes != uint32(FrameCount * FrameBytes))
        {
            return Fail(TEXT("UE voice decoder returned an invalid PCM length"), OutPcm, OutError);
        }
        OutPcm.Append(Decoded.GetData(), ActualBytes);
        bSawPartialPacket = ActualBytes < PacketPcmBytes;
        DecodedPcmBytes += ActualBytes;
        InOutOffset = ReadOffset = ReadOffset + PacketHeaderBytes + PacketBytes;
        ++PacketIndex;
    }
    if (IsComplete() && ReadOffset != Encoded.Num())
    {
        return Fail(TEXT("Unexpected data after the last NPC voice packet"), OutPcm, OutError);
    }
    return true;
}

bool FDeliveryNpcVoiceDecoder::FinishInput(FString& OutError)
{
    OutError.Reset();
    if (IsComplete()) return true;
    TArray<uint8> Ignored;
    return Fail(TEXT("NPC voice input ended before the final packet"), Ignored, OutError);
}

void FDeliveryNpcVoiceDecoder::Reset()
{
    Decoder.Reset();
    SampleRate = SourcePcmBytes = TotalPcmBytes = DecodedPcmBytes = ReadOffset = PacketIndex = 0;
    Version = 0;
    bFailed = bSawPartialPacket = false;
}
