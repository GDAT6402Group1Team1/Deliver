// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "DeliveryTaskUnlockCondition.generated.h"

class UDeliveryTaskDefinition;
class UDeliveryTaskManagerComponent;

/**
 * 任务解锁条件基类。在 Definition 资产里内联配置，每个任务一套。
 * 需要接别的系统（剧情进度、金钱、时间段）时，继承出 C++ 或蓝图子类即可，
 * 不要往管理器里加 if。
 */
UCLASS(Abstract, EditInlineNew, DefaultToInstanced, BlueprintType, Blueprintable, CollapseCategories)
class DELIVERY_API UDeliveryTaskUnlockCondition : public UObject
{
	GENERATED_BODY()

public:

	/** 条件是否满足。只在服务器上被调用。 */
	UFUNCTION(BlueprintNativeEvent, Category="Unlock")
	bool IsSatisfied(const UDeliveryTaskManagerComponent* Manager) const;
	virtual bool IsSatisfied_Implementation(const UDeliveryTaskManagerComponent* Manager) const;
};

/** 前置任务全部完成。链式任务用这一条就够。 */
UCLASS(DisplayName="前置任务已完成")
class DELIVERY_API UDeliveryTaskUnlockCondition_TasksCompleted : public UDeliveryTaskUnlockCondition
{
	GENERATED_BODY()

public:

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Unlock")
	TArray<TObjectPtr<UDeliveryTaskDefinition>> RequiredTasks;

	virtual bool IsSatisfied_Implementation(const UDeliveryTaskManagerComponent* Manager) const override;
};
