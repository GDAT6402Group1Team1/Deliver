// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryEventComponent.generated.h"

/** 取件、交付、上下车等操作的事件名称 */
UENUM(BlueprintType)
enum class EDeliveryEvent : uint8
{
    PickupSucceeded UMETA(DisplayName="取件成功"),
    DeliverySucceeded UMETA(DisplayName="交付成功"),
    VehicleEntered UMETA(DisplayName="上车"),
    VehicleExited UMETA(DisplayName="下车"),
    VehicleSummoned UMETA(DisplayName="召唤车辆"),
    VehicleImpacted UMETA(DisplayName="车辆受撞"),
    CameraChanged UMETA(DisplayName="切换视角"),
    VehicleHitPerson UMETA(DisplayName="车辆撞人"),
    VehicleHorn UMETA(DisplayName="车辆鸣笛")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryEventDelegate, EDeliveryEvent, Event);

/**
 * 将角色或车辆发布的事件交给已绑定到 OnEvent 的函数
 * 此组件不发送网络消息，跨端通知由 DeliveryCharacter 和 DeliveryMotorbike 的 RPC 完成
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryEventComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryEventComponent();

    /** 音效组件将 HandleEvent 绑定到这里，收到事件后查找并播放声音 */
    UPROPERTY(BlueprintAssignable, Category="事件")
    FDeliveryEventDelegate OnEvent;

    /** 立即调用绑定到 OnEvent 的函数，并传入事件类型 */
    UFUNCTION(BlueprintCallable, Category="事件")
    void Publish(EDeliveryEvent Event);
};
