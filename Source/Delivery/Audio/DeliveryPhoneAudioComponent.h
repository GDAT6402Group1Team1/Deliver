#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryPhoneAudioComponent.generated.h"

class UAudioComponent;
class USoundBase;
class UDeliveryPhoneCallQueueComponent;

/** 监听本机电话状态，播放铃声、接听和挂断音效，手机界面关闭时仍然工作 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryPhoneAudioComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryPhoneAudioComponent();
    void PlayToggle();

    // 在玩家控制器蓝图的 PhoneAudio 组件中配置
    UPROPERTY(EditDefaultsOnly, Category="Phone|Sound")
    TObjectPtr<USoundBase> PhoneToggleSound;
    UPROPERTY(EditDefaultsOnly, Category="Phone|Sound")
    TObjectPtr<USoundBase> IncomingCallSound;
    UPROPERTY(EditDefaultsOnly, Category="Phone|Sound")
    TObjectPtr<USoundBase> AnswerCallSound;
    UPROPERTY(EditDefaultsOnly, Category="Phone|Sound")
    TObjectPtr<USoundBase> HangUpCallSound;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    void BindPhone();
    bool IsLocalPlayer() const;
    void StopRinging();
    UFUNCTION()
    void HandleCallChanged(EDeliveryPhoneCallState State, const FDeliveryPhoneCall& Call);
    TWeakObjectPtr<UDeliveryPhoneCallQueueComponent> Phone;
    FTimerHandle BindTimer;
    UPROPERTY(Transient)
    TObjectPtr<UAudioComponent> RingAudio;
    // 重复的复制通知不能重复播放接听或挂断音效
    bool bHasState = false;
    EDeliveryPhoneCallState LastState = EDeliveryPhoneCallState::Idle;
};
