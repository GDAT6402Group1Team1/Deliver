// 保存物品类型、图标与耐久，物体的场景表现由所属 Actor 处理
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryInventoryItemComponent.generated.h"

class UTexture2D;

UENUM(BlueprintType)
enum class EDeliveryInventoryItemType : uint8
{
	DeliveryItem UMETA(DisplayName = "Delivery Item"),
	BackpackItem UMETA(DisplayName = "Backpack Item")
};

/** 保存单手物品的类型、图标和耐久，场景物理仍由所属物体处理 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryInventoryItemComponent : public UActorComponent
{
	GENERATED_BODY()

  public:
	UDeliveryInventoryItemComponent();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 物品信息
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Item")
	EDeliveryInventoryItemType ItemType = EDeliveryInventoryItemType::BackpackItem;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Item")
	FText DisplayName;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Item")
	TObjectPtr<UTexture2D> Icon;

	// 耐久配置与同步值
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Item|Durability", meta = (ClampMin = "0.0"))
	float MaxDurability = 100.0f;

	/** 任务快递不使用耐久，快捷栏中也不显示耐久条 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Item|Durability")
	bool bUsesDurability = true;

	UPROPERTY(ReplicatedUsing = OnRep_Durability, EditInstanceOnly, BlueprintReadOnly,
	          Category = "Item|Durability", meta = (ClampMin = "0.0"))
	float Durability = 100.0f;

	// 耐久操作与物品规则
	UFUNCTION(BlueprintCallable, Category = "Item|Durability", BlueprintAuthorityOnly)
	void ApplyDurabilityDamage(float Amount);

	UFUNCTION(BlueprintPure, Category = "Item|Durability")
	float GetDurabilityFraction() const;

	UFUNCTION(BlueprintPure, Category = "Item|Durability")

	bool UsesDurability() const
	{
		return bUsesDurability;
	}

	UFUNCTION(BlueprintPure, Category = "Item")

	bool CanEnterBackpack() const
	{
		return ItemType == EDeliveryInventoryItemType::BackpackItem;
	}

  private:
	UFUNCTION()
	void OnRep_Durability();
};
