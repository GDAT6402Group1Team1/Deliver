#pragma once
#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "GameplayTagContainer.h"
#include "DeliveryNpcProfile.generated.h"
class UAnimSequence;
class UDeliveryNpcVoiceSet;

// 同一行为可按人物更换已导入的片段，步速和动作完成仍由原执行接口决定
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcBehaviorPresentation
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为表现") TObjectPtr<UAnimSequence> Animation;
    // 0 使用表现组件已有的参考速度，静态姿势不使用此值
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为表现",meta=(DisplayName="步态参考速度",ClampMin="0",ClampMax="500",Units="cm/s")) float MovementReferenceSpeed=0;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为表现",meta=(DisplayName="站定播放倍率",ClampMin="0.25",ClampMax="3")) float PlayRate=1;
};

// 每行配置一名 NPC 的名字、性格、声音及已有动作的可选表现
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcProfileRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="人物",meta=(DisplayName="显示名称")) FText DisplayName;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="人物",meta=(MultiLine="true",DisplayName="人物性格")) FString Persona;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="声音",meta=(DisplayName="TTS 音色",ToolTip="完整中文台词由后端朗读，目前支持 male / female；留空保留原设置，播放音量和短反应音在声音配置中选择")) FName TtsVoice;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="声音",meta=(DisplayName="声音配置",ToolTip="配置完整台词的音量和距离衰减，以及笑声、疑问和受击等短反应音")) TObjectPtr<UDeliveryNpcVoiceSet> VoiceSet;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="行为表现",meta=(Categories="Npc.Behavior"))
    TMap<FGameplayTag,FDeliveryNpcBehaviorPresentation> BehaviorPresentation;
};
