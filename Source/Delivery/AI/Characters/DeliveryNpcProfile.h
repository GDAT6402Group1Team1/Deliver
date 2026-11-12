#pragma once
#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "DeliveryNpcProfile.generated.h"
class UDeliveryNpcVoiceSet;

// 每行配置一名 NPC 的名字、性格、中文 TTS 音色，以及完整台词和短反应音的声音配置
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcProfileRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="人物",meta=(DisplayName="显示名称")) FText DisplayName;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="人物",meta=(MultiLine="true",DisplayName="人物性格")) FString Persona;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="声音",meta=(DisplayName="TTS 音色",ToolTip="完整中文台词由后端朗读，目前支持 male / female；留空保留原设置，播放音量和短反应音在声音配置中选择")) FName TtsVoice;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="声音",meta=(DisplayName="声音配置",ToolTip="配置完整台词的音量和距离衰减，以及笑声、疑问和受击等短反应音")) TObjectPtr<UDeliveryNpcVoiceSet> VoiceSet;
};
