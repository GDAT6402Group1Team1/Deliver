#pragma once

#include "CoreMinimal.h"

class IVoiceDecoder;
class IVoiceEncoder;

/**
 * @brief NPC 语音的压缩和解压用到的常量和工具函数
 *
 * NPC 的台词由 AI 后端合成成声音，交到服务器时是 PCM 格式：一串没压缩过的音量采样点，
 * 每秒 16000 或 24000 个，每个占 2 字节，12 秒就有五十多万字节，直接发给每个玩家太占网络
 * 所以服务器先用 Opus 压小（Opus 是 UE 语音聊天也在用的语音压缩格式，压完只剩原来的一小部分），
 * 再分段发给客户端，客户端解压后播放
 * 一句最长 12 秒，只支持单声道
 */
namespace DeliveryNpcVoice
{
    /// 格式版本 1：整句声音合成完才一次发出，开头就知道总长
    inline constexpr int32 FormatVersion = 1;

    /// 格式版本 2：后端边合成边发，服务器边收边压缩，开始播的时候还不知道这句有多长
    inline constexpr int32 StreamingFormatVersion = 2;

    /// 一句最长 12 秒
    inline constexpr int32 MaxPcmSeconds = 12;

    /// 一句原始声音最多多少字节：每秒 24000 个采样 × 每个 2 字节 × 12 秒
    inline constexpr int32 MaxPcmBytes = 24000 * 2 * MaxPcmSeconds;

    /// 解压后最多多少字节，比原始声音多出 0.04 秒，原因见 GetDecodedPcmBytes
    inline constexpr int32 MaxDecodedPcmBytes = MaxPcmBytes + 24000 * 2 / 25;

    /// 后端用 JSON 发整句声音时会转成 Base64 文本（每 3 字节变 4 个字符），这是它的最大长度
    inline constexpr int32 MaxPcmBase64Chars = ((MaxPcmBytes + 2) / 3) * 4;

    /// 后端边合成边发时，一句最多分几段
    inline constexpr int32 MaxAudioChunks = 48;

    /// 压缩后一句最多多少字节，超过就当数据有问题
    inline constexpr int32 MaxEncodedBytes = 196608;

    /// @brief 一句 12 秒的原始声音在这个采样率下有多少字节
    /// @param SampleRate 每秒采样数，只支持 16000 和 24000
    /// @return 不支持的采样率返回 0
    inline constexpr int32 GetMaxPcmBytes(int32 SampleRate)
    {
        return SampleRate == 16000 || SampleRate == 24000 ? SampleRate * 2 * MaxPcmSeconds : 0;
    }

    /// @brief 一句话解压后有多少字节
    ///
    /// 压缩程序处理到句尾时，最后一点声音可能还留在它内部没输出；
    /// 所以压缩时在句尾接一小段 0.04 秒的无声数据，让压缩程序把最后一个字也输出
    /// 解压出来的长度因此比原始声音多 0.04 秒
    DELIVERY_API int32 GetDecodedPcmBytes(int32 SourcePcmBytes /* 原始声音的字节数 */, int32 SampleRate /* 每秒采样数 */);

    /// @brief 一次压完整句话，用于格式版本 1
    /// @param Pcm 原始声音，单声道，每个采样 2 字节
    /// @param OutEncoded 压缩结果，开头带着采样率和长度，客户端据此解压
    /// @return 失败时返回 false，原因写在 OutError
    DELIVERY_API bool Encode(const TArray<uint8>& Pcm, int32 SampleRate, TArray<uint8>& OutEncoded, FString& OutError);
}

/**
 * @brief 服务器边收边压缩 NPC 的一句话，方便尽早发给其他玩家
 *
 * 每句话开始时调用 Start。之后每收到一小段声音，就调用 Append 接着压缩，
 * 压好的部分马上可以发出去，玩家不用等整句合成完才听到声音。
 * 整句话收完后调用 Finish。压缩程序这时可能还有一点声音没输出，
 * 所以 Finish 会在句尾接上 0.04 秒的无声数据，让最后一个字能播完整
 *
 * 压缩比较耗时，表现组件把它放到后台线程做；UE 要求模块在游戏线程加载，
 * 所以开始前要先在游戏线程加载 UE 的语音模块 FVoiceModule
 */
class DELIVERY_API FDeliveryNpcVoiceStreamEncoder
{
public:
    // ── 压缩 ──

    /// @brief 开始压缩新的一句，先输出一段格式头（采样率等），不用知道这句多长
    /// @return 已经开始过还没 Reset、采样率不支持或 UE 的编码器创建不了时返回 false
    bool Start(int32 InSampleRate, TArray<uint8>& OutEncoded, FString& OutError);

    /// @brief 接着压一段新收到的声音
    ///
    /// Opus 每次只能压固定长度的一小段（20 毫秒叫一帧），这里每凑够 4 帧（0.08 秒）压成一包发出；
    /// 收到的声音凑不够一包时先存着，等下一段来了一起压
    /// @param OutEncoded 只放这次新压好的部分，调用方直接发出去
    /// @return 没出错就返回 true，并不代表这句已经压完
    bool Append(const TArray<uint8>& Pcm, TArray<uint8>& OutEncoded, FString& OutError);

    /// @brief 这句收完了：压完剩下的声音，句尾接 0.04 秒无声数据，最后写上总长
    /// 客户端看到总长才知道这句是完整收到了，而不是中途断线
    bool Finish(TArray<uint8>& OutEncoded, FString& OutError);

    /// @brief NPC 被打断时丢掉这句还没压的声音，可以重复调用
    void Reset();

private:
    /// 把攒够 0.08 秒的声音压成一包；bFinal 时不够一包的也补上无声数据压掉
    bool Flush(bool bFinal /* true 表示这句已经收完 */, TArray<uint8>& OutEncoded, FString& OutError);

    /// 记下失败原因，之后的调用一律失败
    bool Fail(const TCHAR* Reason, TArray<uint8>& OutEncoded, FString& OutError);

    /// UE 的 Opus 编码器
    TSharedPtr<IVoiceEncoder> Encoder;

    /// 还不够一包的原始声音
    TArray<uint8> PendingPcm;

    /// 采样率
    int32 SampleRate = 0;

    /// 已收到的原始字节数
    int32 SourcePcmBytes = 0;

    /// 已输出的压缩字节数
    int32 EncodedBytes = 0;

    /// 下一包的序号
    int32 PacketIndex = 0;

    /// 已经开始
    bool bStarted = false;

    /// 已经结束
    bool bFinished = false;

    /// 已经失败
    bool bFailed = false;
};

/**
 * @brief 客户端把收到的压缩声音还原成能播的声音
 *
 * 每句话一个解码器。每收到一段数据，表现组件就把这句从头到现在收到的全部数据交给 TryDecodeAvailable，
 * 它记得上次解到哪，只解出新增的部分，马上送去播放。
 * 服务器说这句发完了，再调用 FinishInput 核对总长，确认没有中途断掉的数据
 */
class DELIVERY_API FDeliveryNpcVoiceDecoder
{
public:
    // ── 解压 ──

    /// @brief 解出目前能解的部分
    /// @param Encoded 从第一个字节起收到的全部压缩数据
    /// @param InOutOffset 上次解到哪，解完后往后挪
    /// @param OutPcm 只放这次新解出来的声音
    /// @return 数据没问题就返回 true，最后一块可能还没收全、要等下一段；返回 false 时这句数据坏了，改用文字播
    bool TryDecodeAvailable(const TArray<uint8>& Encoded, int32& InOutOffset, TArray<uint8>& OutPcm, FString& OutError);

    /// @brief 服务器说这句发完了：核对解出来的总长和结尾写的一致
    /// 不一致说明中途丢了数据，返回 false，免得把半句话当成完整的一句
    bool FinishInput(FString& OutError);

    /// @brief NPC 被打断或换下一句时清空，再用时从第一个字节开始
    void Reset();

    /// @brief 格式头里的采样率
    int32 GetSampleRate() const { return SampleRate; }

    /// @brief 原始声音的字节数
    int32 GetSourcePcmBytes() const { return SourcePcmBytes; }

    /// @brief 解压后的总字节数，收到末尾前为 0
    int32 GetTotalPcmBytes() const { return TotalPcmBytes; }

    /// @brief 整句都解出来了
    bool IsComplete() const { return !bFailed && TotalPcmBytes > 0 && DecodedPcmBytes == TotalPcmBytes; }

private:
    /// 记下失败原因，之后的调用一律失败
    bool Fail(const TCHAR* Reason, TArray<uint8>& OutPcm, FString& OutError);

    /// UE 的 Opus 解码器
    TSharedPtr<IVoiceDecoder> Decoder;

    /// 采样率，从格式头读出
    int32 SampleRate = 0;

    /// 原始声音的字节数
    int32 SourcePcmBytes = 0;

    /// 解压后的总字节数
    int32 TotalPcmBytes = 0;

    /// 已经解出的字节数
    int32 DecodedPcmBytes = 0;

    /// 压缩数据读到哪
    int32 ReadOffset = 0;

    /// 下一包的序号
    int32 PacketIndex = 0;

    /// 格式版本
    int32 Version = 0;

    /// 碰到过还没收全的包
    bool bSawPartialPacket = false;

    /// 已经失败
    bool bFailed = false;
};
