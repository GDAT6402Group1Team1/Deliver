// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Task/DeliveryLocationMarker.h"
#include "DeliveryDropOffPoint.generated.h"

class UDeliveryInteractableComponent;
class UDeliveryTargetComponent;
class UDeliveryTaskDefinition;

/**
 * 收件点：拖进关卡、填个 `LocationId` 和 `ExpectedTask` 就能交付。
 *
 * 和 [ADeliveryPickupPoint](DeliveryPickupPoint.h) 成对。为什么要这么个 Actor：
 * 一个能用的收件点需要**三个**组件（地点 + 可交互 + 收件），手加三次、
 * 还要各自调对参数（`InteractionKey` 必须是 `PickupE`、`HoldDuration` 必须是 0.5），
 * 漏任何一个的表现都是"走过去按 E 没反应"，而且三种漏法长得一模一样。
 *
 * **和取货点不对称的那一点**：收件点要**手填 `ExpectedTask`**，取货点不填任务。
 * 这不是疏忽——取货点按地点 ID 反查"现在该发哪个任务的货"（一个柜台能先后发好几单），
 * 而收件点是"这一户只收这一单"，任务是它的身份而不是它的状态。
 */
UCLASS()
class DELIVERY_API ADeliveryDropOffPoint : public ADeliveryLocationMarker
{
	GENERATED_BODY()

public:

	ADeliveryDropOffPoint();

	/** 这里收哪个任务的快递。转发给内部的 TargetComponent，所以在 Actor 详情里就能填。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|Location")
	TObjectPtr<UDeliveryTaskDefinition> ExpectedTask;

	/** 玩家离多近才能交付（cm）。转发给收件组件，而它会把这个值写进可交互组件。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Delivery|Location", meta=(ClampMin="0.0", Units="cm"))
	float InteractRadius = 180.f;

	UDeliveryTargetComponent* GetTarget() const { return Target; }
	UDeliveryInteractableComponent* GetInteractable() const { return Interactable; }

protected:

	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UDeliveryInteractableComponent> Interactable;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UDeliveryTargetComponent> Target;
};
