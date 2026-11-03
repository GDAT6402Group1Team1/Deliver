#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryPhoneController.generated.h"

class ADeliveryHUD;
class ADeliveryPlayerController;
class UDeliveryPhoneCallQueueComponent;
class UDeliveryDialogueVoiceComponent;

/** 处理手机开关、接听和挂断，读取通话内容并更新字幕 */
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryPhoneController : public UObject
{
    GENERATED_BODY()
public:
    void Initialize(ADeliveryPlayerController* InPlayer, ADeliveryHUD* InHUD);
    void Shutdown();
    virtual UWorld* GetWorld() const override;

    // 手机界面操作
    UFUNCTION(BlueprintCallable, Category="Phone")
    void Toggle();
    UFUNCTION(BlueprintPure, Category="Phone")
    bool IsOpen() const;
    UFUNCTION(BlueprintCallable, Category="Phone")
    void SetOpen(bool bOpen);
    UFUNCTION(BlueprintCallable, Category="Phone")
    void Answer();
    UFUNCTION(BlueprintCallable, Category="Phone")
    void HangUp();

    static const FName PanelId;
private:
    friend class FDeliveryPhoneControllerTest;
    void BindPhone();
    UFUNCTION()
    void HandleCallChanged(EDeliveryPhoneCallState State, const FDeliveryPhoneCall& Call);
    UFUNCTION()
    void HandleGlyph(const FString& Glyph, int32 GlyphIndex);
    void ClearSubtitle();

    TWeakObjectPtr<ADeliveryPlayerController> Player;
    TWeakObjectPtr<ADeliveryHUD> HUD;
    TWeakObjectPtr<UDeliveryPhoneCallQueueComponent> Phone;
    TWeakObjectPtr<UDeliveryDialogueVoiceComponent> Voice;
    FTimerHandle BindTimer;
    // 同一通电话可能收到多次复制通知，只启动一次对白
    int32 ActiveCallId = 0;
    FText CallerName;
    FString Subtitle;
};
