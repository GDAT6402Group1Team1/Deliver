// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemInterface.h"
#include "GameFramework/PlayerState.h"
#include "DeliverPlayerState.generated.h"

class UDeliverAbilitySystemComponent;
class UDeliverAttributeSet;
class UAbilitySystemComponent;

/**
 * PlayerState
 */
UCLASS()
class DELIVERY_API ADeliverPlayerState : public APlayerState, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:

	// 在 GameMode 创建 PlayerState 时调用，用于创建子组件（ASC,属性表）
	ADeliverPlayerState();

	// 在 PlayerState的子组件创建完毕时 由引擎调用 
	virtual void PostInitializeComponents() override;

	/** Getter **/
	// 获取 ASC
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	UDeliverAbilitySystemComponent* GetDeliverAbilitySystemComponent() const { return AbilitySystemComponent; }

protected:

	/** 子组件 **/
	// ASC
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ability")
	TObjectPtr<UDeliverAbilitySystemComponent> AbilitySystemComponent;

	// 属性表
	UPROPERTY()
	TObjectPtr<UDeliverAttributeSet> AttributeSet;

	/** 回调 **/
	// 角色被控制时的回调
	UFUNCTION()
	void HandlePawnSet(APlayerState* Player, APawn* NewPawn, APawn* OldPawn);
};
