// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryTargetComponent.generated.h"

class APlayerState;
class UDeliveryTaskDefinition;

/**
 * 挂在收件人（NPC 或投递点）身上，声明"这里收哪个任务的快递"。
 * 交互系统在玩家按下交付键时调 TryDeliver，交付成功由本组件销毁快递 Actor。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryTargetComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryTargetComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Task")
	TObjectPtr<UDeliveryTaskDefinition> ExpectedTask;

	/** 交付判定距离，从收件人算起。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Task", meta=(ClampMin="0.0", Units="cm"))
	float DeliveryRadius = 250.f;

	/** 手里这件快递现在能不能交给我。UI 用它决定要不要显示交付提示。 */
	UFUNCTION(BlueprintPure, Category="Task")
	bool CanAcceptDelivery(AActor* ItemActor, APlayerState* Player) const;

	/** 服务器：完成交付。成功后销毁快递 Actor，任务对所有玩家结束，奖励结算给 Player。 */
	UFUNCTION(BlueprintCallable, Category="Task")
	bool TryDeliver(AActor* ItemActor, APlayerState* Player);
};
