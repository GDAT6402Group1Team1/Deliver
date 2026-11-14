#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DeliverySpeechStatusWidget.generated.h"

class UDeliverySpeechInputComponent;
class UDeliveryConversationComponent;
class UTextBlock;
class UButton;
class UWidgetAnimation;

// HUD 创建本机语音状态蓝图，麦克风开启期间持续显示，控件树和呼吸动画由蓝图保存
UCLASS()
class DELIVERY_API UDeliverySpeechStatusWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="Delivery|Speech")
    void SetSpeechInput(UDeliverySpeechInputComponent* Value);

protected:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    UPROPERTY(meta=(BindWidgetOptional))
    TObjectPtr<UTextBlock> VoiceLabel;

    UPROPERTY(meta=(BindWidgetOptional))
    TObjectPtr<UTextBlock> VoicePreview;

    UPROPERTY(meta=(BindWidgetOptional))
    TObjectPtr<UButton> VoiceDisableButton;

    UPROPERTY(meta=(BindWidgetOptional))
    TObjectPtr<UTextBlock> VoiceDisableLabel;

    UPROPERTY(Transient, meta=(BindWidgetAnimOptional))
    TObjectPtr<UWidgetAnimation> VoiceStatusPulse;

private:
    UFUNCTION()
    void RefreshStatus();

    UFUNCTION()
    void DisableVoice();

    void SetPulseEnabled(bool bEnabled);
    void HandleVoiceInputError(FName Reason);
    void UnbindConversation();

    TWeakObjectPtr<UDeliverySpeechInputComponent> SpeechInput;
    TWeakObjectPtr<UDeliveryConversationComponent> Conversation;
    FDelegateHandle ConversationErrorHandle;
    bool bConstructed = false;
    bool bPulsePlaying = false;
    friend class FDeliveryConversationVoiceKeyTest;
};
