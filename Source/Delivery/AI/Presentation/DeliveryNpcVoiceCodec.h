#pragma once

#include "CoreMinimal.h"

class IVoiceDecoder;
class IVoiceEncoder;

namespace DeliveryNpcVoice
{
    inline constexpr int32 FormatVersion = 1;
    inline constexpr int32 StreamingFormatVersion = 2;
    inline constexpr int32 MaxPcmBytes = 384000;
    inline constexpr int32 MaxDecodedPcmBytes = 385920;
    inline constexpr int32 MaxEncodedBytes = 196608;

    // 编码器会延后几毫秒声音，末尾加入 40 ms 静音，让最后的声音完整输出
    DELIVERY_API int32 GetDecodedPcmBytes(int32 SourcePcmBytes, int32 SampleRate);

    // 输入为 16/24 kHz、单声道、16 bit PCM；成功后返回带长度信息的 UE Opus 数据
    DELIVERY_API bool Encode(const TArray<uint8>& Pcm, int32 SampleRate, TArray<uint8>& OutEncoded, FString& OutError);
}

// 一句话共用一个 UE 编码器，收到 PCM 后逐段编码，结束时只补一次尾音
// 调用方按顺序使用；进入后台线程前应先在游戏线程加载 FVoiceModule
class DELIVERY_API FDeliveryNpcVoiceStreamEncoder
{
public:
    // Start 返回格式头，不要求提前知道声音总长；重复开始前先 Reset
    bool Start(int32 InSampleRate, TArray<uint8>& OutEncoded, FString& OutError);
    // OutEncoded 只包含本次新增数据；不足一包时暂存，true 不表示一句话已完成
    bool Append(const TArray<uint8>& Pcm, TArray<uint8>& OutEncoded, FString& OutError);
    // 发送剩余声音与总长，完成后拒绝继续追加
    bool Finish(TArray<uint8>& OutEncoded, FString& OutError);
    // 取消时丢弃尚未编码的声音，允许重复调用
    void Reset();

private:
    bool Flush(bool bFinal, TArray<uint8>& OutEncoded, FString& OutError);
    bool Fail(const TCHAR* Reason, TArray<uint8>& OutEncoded, FString& OutError);
    TSharedPtr<IVoiceEncoder> Encoder;
    TArray<uint8> PendingPcm;
    int32 SampleRate = 0;
    int32 SourcePcmBytes = 0;
    int32 EncodedBytes = 0;
    int32 PacketIndex = 0;
    bool bStarted = false;
    bool bFinished = false;
    bool bFailed = false;
};

// 每句语音保存独立的解码状态，播放组件按收到的顺序追加压缩数据
class DELIVERY_API FDeliveryNpcVoiceDecoder
{
public:
    // Encoded 保留从第 0 字节开始的已收数据；OutPcm 只返回本次解出的声音
    // true 表示数据有效，也可能仍在等待完整包；false 表示这句语音不能继续播放
    bool TryDecodeAvailable(const TArray<uint8>& Encoded, int32& InOutOffset, TArray<uint8>& OutPcm, FString& OutError);
    // 传输结束后检查末尾通知，避免把中途断线当成完整语音
    bool FinishInput(FString& OutError);

    // 取消或切换语音时释放解码器，再次使用时从 offset 0 开始
    void Reset();
    int32 GetSampleRate() const { return SampleRate; }
    int32 GetSourcePcmBytes() const { return SourcePcmBytes; }
    int32 GetTotalPcmBytes() const { return TotalPcmBytes; }
    bool IsComplete() const { return !bFailed && TotalPcmBytes > 0 && DecodedPcmBytes == TotalPcmBytes; }

private:
    bool Fail(const TCHAR* Reason, TArray<uint8>& OutPcm, FString& OutError);

    TSharedPtr<IVoiceDecoder> Decoder;
    int32 SampleRate = 0;
    int32 SourcePcmBytes = 0;
    int32 TotalPcmBytes = 0;
    int32 DecodedPcmBytes = 0;
    int32 ReadOffset = 0;
    int32 PacketIndex = 0;
    int32 Version = 0;
    bool bSawPartialPacket = false;
    bool bFailed = false;
};
