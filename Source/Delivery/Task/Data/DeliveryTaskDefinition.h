// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryTaskDefinition.generated.h"

class UDeliveryTaskManagerComponent;
class UDeliveryTaskUnlockCondition;

/**
 * 保存单个任务的固定配置，运行状态由任务管理器维护
 * 游戏状态持有任务资源，客户端可通过同步的资源引用识别任务
 */
UCLASS(BlueprintType)

class DELIVERY_API UDeliveryTaskDefinition : public UDataAsset
{
	GENERATED_BODY()

  public:
	// 任务身份与文字
	/** 日志、存档、外部系统引用用的稳定 ID；上线后不要改 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Task")
	FName TaskId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Task")
	FText DisplayName;

	/** 任务列表里显示的一句话 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Task")
	FText SimpleDescription;

	/** 任务详情页显示的完整描述 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Task", meta = (MultiLine = "true"))
	FText DetailedDescription;

	// 关联物品和地点
	/**
	 * 任务关联的物品、地点、收件人和事件编号
	 * 地点编号通过地点注册表查询，取货点据此匹配可发放的任务
	 */

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Reference")
	FName DeliveryItemId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Reference")
	FName PickupLocationId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Reference")
	FName DeliveryLocationId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Reference")
	FName ReceiverNpcId;

	/** 特殊事件表里的 ID；倍率暂时还在下面的 SpecialEventRules 里手配 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Reference")
	FName SpecialEventId;

	// 解锁与电话内容
	/** 解锁条件，必须全部满足；留空表示开局即解锁 */
	UPROPERTY(EditDefaultsOnly, Instanced, BlueprintReadOnly, Category = "Unlock")
	TArray<TObjectPtr<UDeliveryTaskUnlockCondition>> UnlockConditions;

	/** 解锁时打进电话队列的来电 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Phone")
	FDeliveryPhoneCallContent UnlockCall;

	/** 首次超时的催促来电，同一任务只打一次 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Phone")
	FDeliveryPhoneCallContent OverdueCall;

	// 任务时限与提示颜色
	/** 总限时；归零后不结束任务，只转成正计时并保持红色 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Time", meta = (ClampMin = "0.0", Units = "s"))
	float TimeLimitSeconds = 300.f;

	/** 剩余时间少于这个值转黄 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Time", meta = (ClampMin = "0.0", Units = "s"))
	float YellowRemainingSeconds = 120.f;

	/** 剩余时间少于这个值转红；应当小于 YellowRemainingSeconds */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Time", meta = (ClampMin = "0.0", Units = "s"))
	float RedRemainingSeconds = 60.f;

	// 奖励计算配置
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Reward", meta = (ClampMin = "0"))
	int32 BaseReward = 100;

	/**
	 * 时间评价档位，按剩余秒数从大到小配置（正数提前、负数超时），
	 * 和策划表里 Time Rating 那一列的写法一致
	 * 结算时取第一条"剩余时间不低于门槛"的档；比最后一档还差就按最后一档算
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Reward")
	TArray<FDeliveryTimeGrade> TimeGrades;

	/** 特殊事件倍率规则，可叠乘 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Reward")
	TArray<FDeliverySpecialEventRule> SpecialEventRules;

	// 配置查询与校验
	/** 解锁条件是否全部满足；空条件视为满足 */
	bool AreUnlockConditionsMet(const UDeliveryTaskManagerComponent* Manager) const;

	/** 按剩余秒数给出倒计时配色；超时后剩余为负，仍然判红 */
	UFUNCTION(BlueprintPure, Category = "Time")
	EDeliveryTaskUrgency GetUrgency(float RemainingSeconds) const;

	/** 按交付时的剩余秒数取评价档；档位为空时返回 空指针（调用方按 1 倍处理） */
	const FDeliveryTimeGrade* FindTimeGrade(float RemainingSeconds) const;

#if WITH_EDITOR
	/** 在编辑器中检查任务参数，提前发现颜色阈值和奖励档位配置错误 */
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
};
