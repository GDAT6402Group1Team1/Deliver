// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeliveryLocationMarker.generated.h"

class UBillboardComponent;
class UDeliveryLocationComponent;

/**
 * 只标一个地点的空 Actor：拖进关卡、填 ID 就能被任务系统解析。
 *
 * 为什么要这么个专门的 Actor，而不是"往现有 Actor 上用脚本加个组件"：
 * **编辑器 Python 加的组件存不住**——实例上加/删组件当场生效，但重载后按蓝图 SCS
 * 重建，覆盖数据没地方存（这条在 CLAUDE.md 的「编辑器 Python 写数据的三条边界」
 * 第 2 条里，是实测撞出来的）。而 spawn 出来的 **Actor 是真能存住**的，Actor 上的
 * **属性覆盖**也存得住。所以"摆 Actor + 填属性"是唯一能脚本化的路子。
 *
 * 编辑器里可见（Billboard），游戏里不可见、不挡路、不参与碰撞。
 */
UCLASS()
class DELIVERY_API ADeliveryLocationMarker : public AActor
{
	GENERATED_BODY()

public:

	ADeliveryLocationMarker();

	/** 策划表里的地点编号。转发给内部的 LocationComponent，所以在 Actor 详情里就能填。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|Location")
	FName LocationId;

	UDeliveryLocationComponent* GetLocationComponent() const { return LocationComponent; }

protected:

	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UDeliveryLocationComponent> LocationComponent;

#if WITH_EDITORONLY_DATA
	/** 编辑器里能看见它在哪，否则一个没有网格的 Actor 在视口里等于隐形。 */
	UPROPERTY()
	TObjectPtr<UBillboardComponent> Billboard;
#endif
};
