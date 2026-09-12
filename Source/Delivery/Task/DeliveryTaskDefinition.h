// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryTaskDefinition.generated.h"

class UDeliveryTaskManagerComponent;
class UDeliveryTaskUnlockCondition;

/**
 * 一个任务的静态配置，一个任务一份资产。
 * 运行时状态（解锁没有、计时到哪了、谁送到的）不在这里，在 UDeliveryTaskManagerComponent 上。
 *
 * 这份资产同时被 GameState 硬引用，所以客户端一定加载得到，
 * 复制任务状态时可以直接把 Definition 指针发过去。
 */
UCLASS(BlueprintType)
class DELIVERY_API UDeliveryTaskDefinition : public UDataAsset
{
	GENERATED_BODY()

public:

	/** 日志、存档、外部系统引用用的稳定 ID。上线后不要改。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Task")
	FName TaskId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Task")
	FText DisplayName;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Task", meta=(MultiLine="true"))
	FText Description;

	/** 解锁条件，必须全部满足。留空表示开局即解锁。 */
	UPROPERTY(EditDefaultsOnly, Instanced, BlueprintReadOnly, Category="Unlock")
	TArray<TObjectPtr<UDeliveryTaskUnlockCondition>> UnlockConditions;

	/** 解锁时打进电话队列的来电。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Phone")
	FDeliveryPhoneCallContent UnlockCall;

	/** 首次超时的催促来电，同一任务只打一次。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Phone")
	FDeliveryPhoneCallContent OverdueCall;

	/** 总限时。归零后不结束任务，只转成正计时并保持红色。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Time", meta=(ClampMin="0.0", Units="s"))
	float TimeLimitSeconds = 300.f;

	/** 剩余时间少于这个值转黄。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Time", meta=(ClampMin="0.0", Units="s"))
	float YellowRemainingSeconds = 120.f;

	/** 剩余时间少于这个值转红。应当小于 YellowRemainingSeconds。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Time", meta=(ClampMin="0.0", Units="s"))
	float RedRemainingSeconds = 60.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Reward", meta=(ClampMin="0"))
	int32 BaseReward = 100;

	/** 时间评价档位，按 WithinSeconds 从小到大配置。用时落在哪一档就吃哪一档倍率。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Reward")
	TArray<FDeliveryTimeGrade> TimeGrades;

	/** 所有档位都超了（超时交付）时的倍率。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Reward", meta=(ClampMin="0.0"))
	float OvertimeMultiplier = 0.5f;

	/** 特殊事件倍率规则，可叠乘。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Reward")
	TArray<FDeliverySpecialEventRule> SpecialEventRules;

	/** 解锁条件是否全部满足。空条件视为满足。 */
	bool AreUnlockConditionsMet(const UDeliveryTaskManagerComponent* Manager) const;

	/** 按剩余秒数给出倒计时配色。超时后剩余为负，仍然判红。 */
	UFUNCTION(BlueprintPure, Category="Time")
	EDeliveryTaskUrgency GetUrgency(float RemainingSeconds) const;

	/** 用时命中的时间评价档；没命中任何档返回 nullptr，调用方按超时倍率处理。 */
	const FDeliveryTimeGrade* FindTimeGrade(float ElapsedSeconds) const;
};
