#pragma once

#include "CoreMinimal.h"

class IVoiceDecoder;

namespace DeliveryNpcVoice
{
    inline constexpr int32 FormatVersion = 1;
    inline constexpr int32 MaxPcmBytes = 384000;
    inline constexpr int32 MaxDecodedPcmBytes = 385920;
    inline constexpr int32 MaxEncodedBytes = 196608;

    // 编码器会延后几毫秒声音，末尾加入 40 ms 静音，让最后的声音完整输出
    DELIVERY_API int32 GetDecodedPcmBytes(int32 SourcePcmBytes, int32 SampleRate);

    // 输入为 16/24 kHz、单声道、16 bit PCM；成功后返回带长度信息的 UE Opus 数据
    DELIVERY_API bool Encode(const TArray<uint8>& Pcm, int32 SampleRate, TArray<uint8>& OutEncoded, FString& OutError);
}

// 每句语音保存独立的解码状态，播放组件按收到的顺序追加压缩数据
class DELIVERY_API FDeliveryNpcVoiceDecoder
{
public:
    // Encoded 保留从第 0 字节开始的已收数据；OutPcm 只返回本次解出的声音
    // true 表示数据有效，也可能仍在等待完整包；false 表示这句语音不能继续播放
    bool TryDecodeAvailable(const TArray<uint8>& Encoded, int32& InOutOffset, TArray<uint8>& OutPcm, FString& OutError);

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
    bool bFailed = false;
};
