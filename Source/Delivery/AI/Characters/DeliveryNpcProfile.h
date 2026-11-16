#pragma once
#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "GameplayTagContainer.h"
#include "DeliveryNpcProfile.generated.h"
class UAnimSequence;
class UDeliveryNpcVoiceSet;

/// 一种行为在某位人物身上用哪段动画；走多快、什么时候算做完仍由原来的执行逻辑决定
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcBehaviorPresentation
{
    GENERATED_BODY()

    /// 这位人物做这个行为时播的动画
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为表现") TObjectPtr<UAnimSequence> Animation;

    /// 这段走路动画原本对应的速度，按它调播放快慢；0 用表现组件的默认值，站着的动作不用
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为表现",meta=(DisplayName="步态参考速度",ClampMin="0",ClampMax="500",Units="cm/s")) float MovementReferenceSpeed=0;

    /// 站着做的动作播放快慢
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为表现",meta=(DisplayName="站定播放倍率",ClampMin="0.25",ClampMax="3")) float PlayRate=1;
};

/// 人物表的一行：一位 NPC 的名字、性格、声音和各行为的动画，角色 ApplyProfile 时套用
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcProfileRow : public FTableRowBase
{
    GENERATED_BODY()

    /// 显示的名字
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="人物",meta=(DisplayName="显示名称")) FText DisplayName;

    /// 性格描述，原样给模型看
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="人物",meta=(MultiLine="true",DisplayName="人物性格")) FString Persona;

    /// 后端朗读台词用的音色（TTS 是把文字合成成语音），目前有 male 和 female
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="声音",meta=(DisplayName="TTS 音色",ToolTip="完整中文台词由后端朗读，目前支持 male / female；留空保留原设置，播放音量和短反应音在声音配置中选择")) FName TtsVoice;

    /// 台词音量、声音随距离变小的方式、笑声和挨打声等短音
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="声音",meta=(DisplayName="声音配置",ToolTip="配置完整台词的音量和距离衰减，以及笑声、疑问和受击等短反应音")) TObjectPtr<UDeliveryNpcVoiceSet> VoiceSet;

    /// 各行为换成这位人物自己的动画
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为表现",meta=(Categories="Npc.Behavior"))
    TMap<FGameplayTag,FDeliveryNpcBehaviorPresentation> BehaviorPresentation;
};
