// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryCameraComponent.generated.h"

class UDeliveryCameraConfig;
class USpringArmComponent;
class UCameraComponent;

/** 应用镜头配置，处理本机玩家的撞击反馈 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryCameraComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UDeliveryCameraComponent();

    /** 留空时沿用相机组件的设置，撞击反馈使用默认参数 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="镜头", meta=(DisplayName="镜头配置"))
    TObjectPtr<UDeliveryCameraConfig> Config;

    /** 连续撞击只延长保持时间，不会累计拉远距离 */
    UFUNCTION(BlueprintCallable, Category="镜头")
    void PlayImpact();

protected:
    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                               FActorComponentTickFunction* ThisTickFunction) override;

private:
    // 引用角色已有的相机组件，不改变蓝图中的挂接关系
    UPROPERTY(Transient)
    TObjectPtr<USpringArmComponent> Boom;

    UPROPERTY(Transient)
    TObjectPtr<UCameraComponent> Camera;

    // 每名玩家独立保存反馈状态，配置资源只读
    float BaseDistance = 0.0f;
    float HoldUntil = 0.0f;
    bool bImpactActive = false;

    const UDeliveryCameraConfig& GetConfig() const;
    void ApplyConfig();
    void StopImpact();

    friend class FDeliveryCameraImpactTest;
};
