// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Events/DeliveryEventComponent.h"
#include "DeliveryAudioComponent.generated.h"

class UDeliveryAudioConfig;
struct FDeliverySoundSettings;

/** 订阅所属角色或车辆的 Events 组件，根据事件类型查找并播放音效 */
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

    /** 按配置在角色或车辆的位置播放声音，或只向操作玩家播放提示音 */
    virtual void PlaySound(const FDeliverySoundSettings& Settings);

private:
    UFUNCTION()
    void HandleEvent(EDeliveryEvent Event);

    UPROPERTY(Transient)
    TObjectPtr<UDeliveryEventComponent> Events;

    friend class FDeliveryAudioEventTest;
};
