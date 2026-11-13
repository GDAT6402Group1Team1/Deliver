#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DeliveryNpcVoiceSet.generated.h"
class USoundBase;
class USoundAttenuation;

UENUM(BlueprintType)
enum class EDeliveryNpcVoiceReaction : uint8
{
    HeardPlayer UMETA(DisplayName="听见玩家"),
    Question UMETA(DisplayName="疑问"),
    Displeased UMETA(DisplayName="不满"),
    Laugh UMETA(DisplayName="笑"),
    Surprised UMETA(DisplayName="惊讶"),
    Hit UMETA(DisplayName="受击")
};

// 保存人物的短反应音和完整台词的播放设置，台词音色仍由人物表选择
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryNpcVoiceSet : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="完整台词",meta=(DisplayName="台词音量",ClampMin="0",ClampMax="2",ToolTip="只调整完整台词，1 为原音量，0 为静音，不影响下方的短反应音")) float DialogueVolume=1.f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="完整台词",meta=(DisplayName="台词距离衰减",ToolTip="设置完整台词的空间声音与距离衰减，留空时使用 600 厘米内保持原音量、再用 1200 厘米逐渐变小的默认配置")) TObjectPtr<USoundAttenuation> DialogueAttenuation;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="分段语音",meta=(DisplayName="开播缓冲",ClampMin="0.16",ClampMax="2",Units="s",ToolTip="声音持续生成时，先准备这段时长再开始播放；短句已经生成完时可直接播放")) float StreamingStartBufferSeconds=.64f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="分段语音",meta=(DisplayName="续播缓冲",ClampMin="0.08",ClampMax="1",Units="s",ToolTip="播放中暂时缺音时，积累这段时长再继续，减少连续的小停顿")) float StreamingResumeBufferSeconds=.32f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="短反应音",meta=(DisplayName="听见玩家")) TObjectPtr<USoundBase> HeardPlayer;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="短反应音",meta=(DisplayName="疑问")) TObjectPtr<USoundBase> Question;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="短反应音",meta=(DisplayName="不满")) TObjectPtr<USoundBase> Displeased;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="短反应音",meta=(DisplayName="笑")) TObjectPtr<USoundBase> Laugh;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="短反应音",meta=(DisplayName="惊讶")) TObjectPtr<USoundBase> Surprised;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="短反应音",meta=(DisplayName="受击")) TObjectPtr<USoundBase> Hit;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="短反应音",meta=(DisplayName="短音距离衰减",ToolTip="控制听见玩家、受击等短音的空间范围，留空时沿用原来的 150 厘米内原音量、再用 800 厘米衰减")) TObjectPtr<USoundAttenuation> ReactionAttenuation;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="播放",meta=(ClampMin="0",ClampMax="2")) float Volume=0.8f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="播放",meta=(ClampMin="0.5",ClampMax="1.5")) float Pitch=1.f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="播放",meta=(ClampMin="0.1",ClampMax="3",Units="s")) float MaxDuration=1.2f;
    USoundBase* GetSound(EDeliveryNpcVoiceReaction Reaction) const;
};
