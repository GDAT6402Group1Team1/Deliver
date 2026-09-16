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

	/** 任务列表里显示的一句话。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Task")
	FText SimpleDescription;

	/** 任务详情页显示的完整描述。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Task", meta=(MultiLine="true"))
	FText DetailedDescription;

	/*
	下面这些是策划表里的外部引用 ID。它们指向的东西（快递 Actor、取件点、送达点、收件 NPC、
	特殊事件）目前都还没有对应的系统，所以先原样存字符串，不做解析。
	等交互和关卡系统落地后，再决定是按场景 Actor 的 Tag 查，还是解析成资产引用。
	地图引导要取件点/送达点坐标时，依据也在这里。
	*/

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Reference")
	FName DeliveryItemId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Reference")
	FName PickupLocationId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Reference")
	FName DeliveryLocationId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Reference")
	FName ReceiverNpcId;

	/** 特殊事件表里的 ID。倍率暂时还在下面的 SpecialEventRules 里手配。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Reference")
	FName SpecialEventId;

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

	/**
	 * 时间评价档位，按剩余秒数**从大到小**配置（正数提前、负数超时），
	 * 和策划表里 Time Rating 那一列的写法一致。
	 * 结算时取第一条"剩余时间不低于门槛"的档；比最后一档还差就按最后一档算。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Reward")
	TArray<FDeliveryTimeGrade> TimeGrades;

	/** 特殊事件倍率规则，可叠乘。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Reward")
	TArray<FDeliverySpecialEventRule> SpecialEventRules;

	/** 解锁条件是否全部满足。空条件视为满足。 */
	bool AreUnlockConditionsMet(const UDeliveryTaskManagerComponent* Manager) const;

	/** 按剩余秒数给出倒计时配色。超时后剩余为负，仍然判红。 */
	UFUNCTION(BlueprintPure, Category="Time")
	EDeliveryTaskUrgency GetUrgency(float RemainingSeconds) const;

	/** 按交付时的剩余秒数取评价档。档位为空时返回 nullptr（调用方按 1 倍处理）。 */
	const FDeliveryTimeGrade* FindTimeGrade(float RemainingSeconds) const;

#if WITH_EDITOR
	/**
	 * 编辑器里校验配置。这份资产全是手填的数值，而配错的后果要跑到那一刻才看得出来
	 * （颜色不跳、评价档永远命中不到），所以在保存时就标出来。
	 */
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
};
