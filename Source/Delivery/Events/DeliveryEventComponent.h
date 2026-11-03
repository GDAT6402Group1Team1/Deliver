// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryEventComponent.generated.h"

/** 描述已经发生的操作，不指定声音或界面资源 */
UENUM(BlueprintType)
enum class EDeliveryEvent : uint8
{
    PickupSucceeded UMETA(DisplayName="取件成功"),
    DeliverySucceeded UMETA(DisplayName="交付成功"),
    VehicleEntered UMETA(DisplayName="上车"),
    VehicleExited UMETA(DisplayName="下车"),
    VehicleSummoned UMETA(DisplayName="召唤车辆"),
    VehicleImpacted UMETA(DisplayName="车辆受撞"),
    CameraChanged UMETA(DisplayName="切换视角")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryEventDelegate, EDeliveryEvent, Event);

/** 当前对象的本地事件，网络通知由业务对象负责 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryEventComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryEventComponent();

    UPROPERTY(BlueprintAssignable, Category="事件")
    FDeliveryEventDelegate OnEvent;

    /** 通知当前对象的监听者，不自动向服务器或其他客户端发送 */
    UFUNCTION(BlueprintCallable, Category="事件")
    void Publish(EDeliveryEvent Event);
};
