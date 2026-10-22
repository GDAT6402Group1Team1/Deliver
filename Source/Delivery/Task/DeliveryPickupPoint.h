// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Task/DeliveryLocationMarker.h"
#include "DeliveryPickupPoint.generated.h"

class ADeliveryPackageItem;
class UDeliveryInteractableComponent;
class UDeliveryPickupPointComponent;

/**
 * 拖进关卡、填个 `LocationId` 就能领快递的取货点。
 *
 * 继承 `ADeliveryLocationMarker`：地点 ID 的转发、编辑器里的 Billboard、
 * "Python 加的组件存不住所以必须是 Actor"这些它都已经解决了，这里只多装两个组件
 * ——按 E 的可交互组件和发货组件。
 *
 * 如果取货点本来就是关卡里的某个模型（便利店柜台、仓库货架），**不用摆这个 Actor**，
 * 直接在那个 Actor 上加 `DeliveryLocationComponent` + `DeliveryInteractableComponent`
 * + `DeliveryPickupPointComponent` 三件套即可，效果完全一样。
 */
UCLASS()
class DELIVERY_API ADeliveryPickupPoint : public ADeliveryLocationMarker
{
	GENERATED_BODY()

public:

	ADeliveryPickupPoint();

	/**
	 * 发什么快递。转发给内部的 PickupPointComponent，所以在 Actor 详情里就能填。
	 *
	 * 不转发的话这个字段只出现在组件详情里——摆点的人要先在 Components 树里
	 * 点中 `PickupPoint` 才看得见，而 `LocationId` 就在 Actor 上，
	 * 两个必填项分散在两个面板是最容易漏配的形状。收件点的 `ExpectedTask` 同理。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|Pickup")
	TSubclassOf<ADeliveryPackageItem> PackageClass;

	/** 快递生成位置相对本点的偏移。同样转发给组件，免得两个面板来回切。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|Pickup")
	FVector SpawnOffset = FVector(0.f, 0.f, 60.f);

	/** 玩家离多近才能取件（cm）。转发给组件，而组件会把它写进可交互组件。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|Pickup", meta=(ClampMin="0.0", Units="cm"))
	float InteractRadius = 180.f;

	UDeliveryPickupPointComponent* GetPickupPoint() const { return PickupPoint; }
	UDeliveryInteractableComponent* GetInteractable() const { return Interactable; }

protected:

	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UDeliveryInteractableComponent> Interactable;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UDeliveryPickupPointComponent> PickupPoint;
};
