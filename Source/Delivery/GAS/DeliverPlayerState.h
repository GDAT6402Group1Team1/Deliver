// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemInterface.h"
#include "GameFramework/PlayerState.h"
#include "DeliverPlayerState.generated.h"

class UDeliverAbilitySystemComponent;
class UDeliverAttributeSet;
class UAbilitySystemComponent;
class UDeliveryTaskTrackerComponent;

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

	// 获取本玩家的任务追踪组件（任务状态本身是全局的，挂在 GameState 上）
	UDeliveryTaskTrackerComponent* GetTaskTracker() const { return TaskTracker; }

protected:

	/** 子组件 **/
	// ASC
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Ability")
	TObjectPtr<UDeliverAbilitySystemComponent> AbilitySystemComponent;

	// 属性表
	UPROPERTY()
	TObjectPtr<UDeliverAttributeSet> AttributeSet;

	// 当前追踪的任务（每个玩家各自选，只有被追踪的任务显示地图引导）
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Task")
	TObjectPtr<UDeliveryTaskTrackerComponent> TaskTracker;

	/** 回调 **/
	// 角色被控制时的回调
	UFUNCTION()
	void HandlePawnSet(APlayerState* Player, APawn* NewPawn, APawn* OldPawn);
};
