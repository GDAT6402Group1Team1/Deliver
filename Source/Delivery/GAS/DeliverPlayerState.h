// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemInterface.h"
#include "GameFramework/PlayerState.h"
#include "DeliverPlayerState.generated.h"

class UDeliverAbilitySystemComponent;
class UAbilitySystemComponent;

/**
 * PlayerState
 */
UCLASS()
class DELIVERY_API ADeliverPlayerState : public APlayerState, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:

	// 在 GameMode 创建出 PlayerState 时调用
	ADeliverPlayerState();

	// 在 组件创建完毕时 由引擎调用 
	virtual void PostInitializeComponents() override;

	/** Getter **/
	// 获取 ASC
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	UDeliverAbilitySystemComponent* GetDeliverAbilitySystemComponent() const { return AbilitySystemComponent; }

protected:

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ability")
	TObjectPtr<UDeliverAbilitySystemComponent> AbilitySystemComponent;

	UFUNCTION()
	void HandlePawnSet(APlayerState* Player, APawn* NewPawn, APawn* OldPawn);
};
