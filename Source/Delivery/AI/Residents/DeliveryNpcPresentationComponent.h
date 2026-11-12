#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AI/DeliveryNpcTypes.h"
#include "DeliveryNpcPresentationComponent.generated.h"
class UAnimSequence;
class UDeliveryDialogueVoiceComponent;
class UWidgetComponent;

USTRUCT(BlueprintType)
struct FDeliveryNpcSpeechState
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FGuid TurnId;
    UPROPERTY(BlueprintReadOnly) FGuid ReplyToEventId;
    UPROPERTY(BlueprintReadOnly) FString Text;
    UPROPERTY(BlueprintReadOnly) FName Emotion;
    UPROPERTY(BlueprintReadOnly) double StartTime=0;
    UPROPERTY(BlueprintReadOnly) float Duration=0;
    UPROPERTY(BlueprintReadOnly) bool bActive=false;
};

// 服务器只发布对白身份和时序，本机播放动画、电子声线和字幕，不推进玩法
UCLASS(ClassGroup=(Delivery),meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryNpcPresentationComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryNpcPresentationComponent();
    bool PublishSpeech(const FString& Text,FName Emotion,int32 Depth,FGuid ReplyToEventId=FGuid());
    void CancelSpeech();
    UFUNCTION(BlueprintPure) FDeliveryNpcSpeechState GetSpeech() const { return Speech; }
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> IdleAnimation;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> WalkAnimation;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> AttackAnimation;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> DownAnimation;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> TalkAnimation;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> GetUpAnimation;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") TObjectPtr<UAnimSequence> HoldAnimation;
    // 有面部形态键的模型可在蓝图配置，缺少形态键时不伪造面部动画
    UPROPERTY(EditAnywhere,Category="Delivery|表演") FName MouthMorph;
    UPROPERTY(EditAnywhere,Category="Delivery|表演") FName AngryMorph;
    UPROPERTY(EditAnywhere,Category="Delivery|表演",meta=(ClampMin="80",ClampMax="500",Units="Hz")) float VoiceFrequency=200;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* Fn) override;
private:
    UPROPERTY(ReplicatedUsing=OnRep_Speech) FDeliveryNpcSpeechState Speech;
    UFUNCTION() void OnRep_Speech();
    UPROPERTY(Transient) TObjectPtr<UDeliveryDialogueVoiceComponent> Voice;
    UPROPERTY(Transient) TObjectPtr<UWidgetComponent> Caption;
    TWeakObjectPtr<UAnimSequence> Playing;
    FGuid PlayedTurn;
    int32 SpeechDepth=0;
    double ServerTime() const;
};
