// 提供用于验证快递交付流程的测试目标
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeliveryTestTarget.generated.h"

class UDeliveryInteractableComponent;
class UDeliveryTargetComponent;
class UStaticMeshComponent;

/** 用于验证完整快递流程的可见测试收件目标 */
UCLASS(Blueprintable)

class DELIVERY_API ADeliveryTestTarget : public AActor
{
	GENERATED_BODY()

  public:
	ADeliveryTestTarget();

  protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> TargetMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UDeliveryInteractableComponent> InteractableComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UDeliveryTargetComponent> DeliveryTargetComponent;
};
