// 提供可抓取物体的基础网格、物理与交互配置
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeliveryGrabbableProp.generated.h"

class UStaticMeshComponent;
class UDeliveryGrabbableComponent;

/** 可抓取物体的基础类，可在蓝图子类中替换模型 */
UCLASS(Blueprintable)

class DELIVERY_API ADeliveryGrabbableProp : public AActor
{
	GENERATED_BODY()

  public:
	ADeliveryGrabbableProp();
	virtual void BeginPlay() override;

	// 物理模型与抓取组件
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grab")
	TObjectPtr<UStaticMeshComponent> PhysicsMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grab")
	TObjectPtr<UDeliveryGrabbableComponent> Grabbable;

	// 物体质量
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grab", meta = (ClampMin = "0.1"))
	float MassKg = 3.0f;
};
