// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DeliveryHandheldItem.generated.h"

class APawn;
class UDeliveryInteractableComponent;
class UDeliveryInventoryItemComponent;
class UStaticMeshComponent;

/** Physical world actor that can move between a player's hand, backpack and the ground. */
UCLASS(Blueprintable)
class DELIVERY_API ADeliveryHandheldItem : public AActor
{
	GENERATED_BODY()

public:
	ADeliveryHandheldItem();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** Native input dispatch; configured axe swings do not depend on a Blueprint Use override. */
	void UseFromInventory(APawn* User);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Item|Attack")
	bool bUsesAxeSwing = false;

	/** Item-specific LMB behavior for items without the native axe action. */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Item")
	void Use(APawn* User);
	virtual void Use_Implementation(APawn* User);

	void SetInventoryPresentation(bool bInHand, bool bInInventory, USceneComponent* HandParent);
	void DropFromInventory(const FVector& WorldLocation, const FVector& InitialVelocity);

	/**
	 * 焊到载具的货架上。
	 *
	 * 和手上那套的区别是**不走每帧跟随**：`HeldAnchor` 那条路是为了过滤 Chaos 的
	 * 手部抖动，而载具是运动学的、位置由代码直接写，一点抖都没有。硬附着不但更省，
	 * 还天然没有"快递落后车半个身位"的拖影——指数跟随在高速时必然欠一截。
	 */
	void SetVehicleCargoPresentation(USceneComponent* CargoParent);

	UFUNCTION(BlueprintPure, Category="Item")
	UDeliveryInventoryItemComponent* GetItemComponent() const { return ItemComponent; }

	UFUNCTION(BlueprintPure, Category="Item")
	UStaticMeshComponent* GetItemMesh() const { return ItemMesh; }

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UStaticMeshComponent> ItemMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UDeliveryInventoryItemComponent> ItemComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UDeliveryInteractableComponent> InteractableComponent;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Item|Holding")
	FName HandSocket = TEXT("RightHand");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Item|Holding")
	FTransform HeldRelativeTransform = FTransform(FRotator(0.0f, 0.0f, 90.0f), FVector::ZeroVector);

	/** Exponential follow speeds. High enough to feel attached, low enough to filter Chaos hand jitter. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Item|Holding", meta=(ClampMin="0.1"))
	float HeldPositionFollowSpeed = 28.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Item|Holding", meta=(ClampMin="0.1"))
	float HeldRotationFollowSpeed = 24.0f;

private:
	void UpdateHeldTransform(float DeltaSeconds, bool bSnap);

	TWeakObjectPtr<USceneComponent> HeldAnchor;
	bool bHeldPresentationActive = false;

	UFUNCTION()
	void HandleInteract(APawn* Interactor);
};
