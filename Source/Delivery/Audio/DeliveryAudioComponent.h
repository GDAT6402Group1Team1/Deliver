// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Events/DeliveryEventComponent.h"
#include "DeliveryAudioComponent.generated.h"

class UDeliveryAudioConfig;
struct FDeliverySoundSettings;

/** 监听同一对象的事件，根据配置播放一次性音效 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryAudioComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryAudioComponent();

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="音效", meta=(DisplayName="音效配置"))
    TObjectPtr<UDeliveryAudioConfig> Config;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /** 可由需要特殊播放方式的子类扩展 */
    virtual void PlaySound(const FDeliverySoundSettings& Settings);

private:
    UFUNCTION()
    void HandleEvent(EDeliveryEvent Event);

    UPROPERTY(Transient)
    TObjectPtr<UDeliveryEventComponent> Events;

    friend class FDeliveryAudioEventTest;
};
