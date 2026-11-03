// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeliveryLocationMarker.generated.h"

class UBillboardComponent;
class UDeliveryLocationComponent;

/**
 * 可直接放入关卡的地点标记，填写编号后即可被任务系统查询
 * 编辑器中显示图标，游戏中隐藏且不参与碰撞
 */
UCLASS()

class DELIVERY_API ADeliveryLocationMarker : public AActor
{
	GENERATED_BODY()

  public:
	ADeliveryLocationMarker();

	// 地点编号
	/** 策划表里的地点编号；转发给内部的 LocationComponent，所以在 Actor 详情里就能填 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Delivery|Location")
	FName LocationId;

	UDeliveryLocationComponent* GetLocationComponent() const
	{
		return LocationComponent;
	}

  protected:
	virtual void OnConstruction(const FTransform& Transform) override;

	// 位置与编辑器图标
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UDeliveryLocationComponent> LocationComponent;

#if WITH_EDITORONLY_DATA
	/** 编辑器里能看见它在哪，否则一个没有网格的 Actor 在视口里等于隐形 */
	UPROPERTY()
	TObjectPtr<UBillboardComponent> Billboard;
#endif
};
