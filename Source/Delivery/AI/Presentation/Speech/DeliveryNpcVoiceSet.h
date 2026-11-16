#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DeliveryNpcVoiceSet.generated.h"
class USoundBase;
class USoundAttenuation;

/// 短反应音的种类
UENUM(BlueprintType)
enum class EDeliveryNpcVoiceReaction : uint8
{
    /// 听见有人跟自己说话
    HeardPlayer UMETA(DisplayName="听见玩家"),

    /// 疑问
    Question UMETA(DisplayName="疑问"),

    /// 不满
    Displeased UMETA(DisplayName="不满"),

    /// 笑
    Laugh UMETA(DisplayName="笑"),

    /// 惊讶
    Surprised UMETA(DisplayName="惊讶"),

    /// 挨打
    Hit UMETA(DisplayName="受击")
};

/**
 * @brief 一位人物的声音资产：完整台词的音量和衰减、边收边播的缓冲、各种短反应音
 *
 * 表现组件播台词和 PlayReaction 时读这里；台词用哪个合成音色由人物表决定
 */
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryNpcVoiceSet : public UDataAsset
{
    GENERATED_BODY()
public:
    /// 完整台词的音量，1 是原音量
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="完整台词",meta=(DisplayName="台词音量",ClampMin="0",ClampMax="2",ToolTip="只调整完整台词，1 为原音量，0 为静音，不影响下方的短反应音")) float DialogueVolume=1.f;

    /// 完整台词随距离变小的方式，留空用默认：600 cm 内原音量，再 1200 cm 渐弱
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="完整台词",meta=(DisplayName="台词距离衰减",ToolTip="设置完整台词的空间声音与距离衰减，留空时使用 600 厘米内保持原音量、再用 1200 厘米逐渐变小的默认配置")) TObjectPtr<USoundAttenuation> DialogueAttenuation;

    /// 边收边播时先攒多少秒再开播
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="分段语音",meta=(DisplayName="开播缓冲",ClampMin="0.16",ClampMax="2",Units="s",ToolTip="声音持续生成时，先准备这段时长再开始播放；默认等待两个 0.64 秒音频块，减少生成速度波动引起的停顿；短句已经生成完时可直接播放")) float StreamingStartBufferSeconds=1.28f;

    /// 播到一半断了，攒多少秒再接着播
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="分段语音",meta=(DisplayName="续播缓冲",ClampMin="0.08",ClampMax="1",Units="s",ToolTip="播放中暂时缺音时，积累这段时长再继续，减少连续的小停顿")) float StreamingResumeBufferSeconds=.32f;

    /// 听见有人跟自己说话
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="短反应音",meta=(DisplayName="听见玩家")) TObjectPtr<USoundBase> HeardPlayer;

    /// 疑问
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="短反应音",meta=(DisplayName="疑问")) TObjectPtr<USoundBase> Question;

    /// 不满
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="短反应音",meta=(DisplayName="不满")) TObjectPtr<USoundBase> Displeased;

    /// 笑
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="短反应音",meta=(DisplayName="笑")) TObjectPtr<USoundBase> Laugh;

    /// 惊讶
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="短反应音",meta=(DisplayName="惊讶")) TObjectPtr<USoundBase> Surprised;

    /// 挨打
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="短反应音",meta=(DisplayName="受击")) TObjectPtr<USoundBase> Hit;

    /// 短反应音随距离变小的方式
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="短反应音",meta=(DisplayName="短音距离衰减",ToolTip="控制听见玩家、受击等短音的空间范围，留空时沿用原来的 150 厘米内原音量、再用 800 厘米衰减")) TObjectPtr<USoundAttenuation> ReactionAttenuation;

    /// 短反应音的音量
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="播放",meta=(ClampMin="0",ClampMax="2")) float Volume=0.8f;

    /// 短反应音的音高
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="播放",meta=(ClampMin="0.5",ClampMax="1.5")) float Pitch=1.f;

    /// 短反应音最长播多久
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="播放",meta=(ClampMin="0.1",ClampMax="3",Units="s")) float MaxDuration=1.2f;

    /// @brief 取某种短反应对应的声音，没配时返回空
    USoundBase* GetSound(EDeliveryNpcVoiceReaction Reaction) const;
};
