// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryInteractionProbeComponent.generated.h"

class UDeliveryInteractableComponent;

/**
 * 挂在玩家 Pawn 上的"附近有什么能按 F"的探测器。
 *
 * 纯本地表现：只在本机操控的 Pawn 上跑，找到最近的可交互物就把浮窗推给
 * UDeliveryPromptSubsystem。真正的交互要走服务器复核（见 ADeliveryCharacter::ServerInteract），
 * 这里找到的目标只是客户端的"我想按这个"。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryInteractionProbeComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryInteractionProbeComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** 当前瞄到的可交互物；没有就是 null。 */
	UDeliveryInteractableComponent* GetFocused() const { return Focused.Get(); }

	UFUNCTION(BlueprintPure, Category="Interaction")
	AActor* GetFocusedActor() const;

protected:

	/** 关掉就只探测不显示浮窗，留给以后想用 UMG 自己画提示的情况。 */
	UPROPERTY(EditAnywhere, Category="Interaction")
	bool bShowPrompt = true;

private:

	TWeakObjectPtr<UDeliveryInteractableComponent> Focused;
};
