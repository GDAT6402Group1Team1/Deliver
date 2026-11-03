// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DeliveryCameraConfig.generated.h"

/** 镜头的固定参数，多名角色可以使用同一份配置 */
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryCameraConfig : public UDataAsset
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="基础", meta=(DisplayName="镜头距离", ClampMin="0", Units="cm"))
    float Distance = 400.0f;

    /** 相对弹簧臂末端的偏移 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="基础", meta=(DisplayName="镜头偏移", Units="cm"))
    FVector Offset = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="基础", meta=(DisplayName="视野角度", ClampMin="5", ClampMax="170", Units="deg"))
    float FieldOfView = 90.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="跟随", meta=(DisplayName="平滑跟随位置"))
    bool bSmoothPosition = false;

    /** 数值越大，镜头越快追上角色 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="跟随", meta=(DisplayName="位置跟随速度", ClampMin="0.1", EditCondition="bSmoothPosition"))
    float PositionSpeed = 10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="跟随", meta=(DisplayName="平滑跟随旋转"))
    bool bSmoothRotation = false;

    /** 数值越大，镜头越快转向目标方向 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="跟随", meta=(DisplayName="旋转跟随速度", ClampMin="0.1", EditCondition="bSmoothRotation"))
    float RotationSpeed = 10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="撞击", meta=(DisplayName="额外拉远距离", ClampMin="0", Units="cm"))
    float ImpactDistance = 120.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="撞击", meta=(DisplayName="保持时间", ClampMin="0", Units="s"))
    float ImpactHoldTime = 0.35f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="撞击", meta=(DisplayName="拉远速度", ClampMin="0.1"))
    float ImpactOutSpeed = 8.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="撞击", meta=(DisplayName="恢复速度", ClampMin="0.1"))
    float ImpactReturnSpeed = 3.0f;
};
