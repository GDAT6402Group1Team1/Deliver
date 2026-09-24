// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliveryWalletComponent.generated.h"

class UDeliveryTaskDefinition;

/** 余额变化。NewBalance 是变化后的余额，Delta 是这次的增减（负数是扣款）。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDeliveryBalanceChanged, int32, NewBalance, int32, Delta);

/** 送达入账。带完整奖励明细，UI 可以直接拿去做结算飘字（底薪 × 时间档 × 特殊事件）。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDeliveryTaskPaid, UDeliveryTaskDefinition*, Task,
	const FDeliveryRewardBreakdown&, Reward);

/**
 * 玩家的钱包。挂在 PlayerState 上。
 *
 * 为什么在 PlayerState 而不是 Character：钱要在角色死亡/重生/上下载具之后还在，
 * 而 Character 在这些事件里会被销毁重建。GAS 的 ASC 也是因为同样的理由挂在这儿。
 *
 * 入账是**自动的**：组件自己订阅 GameState 上任务管理器的 OnTaskCompleted，
 * 那个委托自带 APlayerState* Deliverer，所以多人抢单时钱记给谁天然就是对的，
 * 不需要调用方判断。任务系统那边一行都不用改。
 *
 * 余额是服务器权威的，用 COND_OwnerOnly 只复制给拥有者——别人的钱包余额
 * 不该出现在你的客户端上。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryWalletComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliveryWalletComponent();

	/** 从任意 Actor（角色、控制器、PlayerState）找到它对应的钱包。找不到返回 nullptr。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Economy", meta=(DisplayName="Get Delivery Wallet"))
	static UDeliveryWalletComponent* FindWallet(AActor* Actor);

	UFUNCTION(BlueprintPure, Category="Delivery|Economy")
	int32 GetBalance() const { return Balance; }

	/**
	 * 加钱。只在服务器上有效，客户端调用会被忽略（不做预测：钱这种东西
	 * 预测错了要回滚，而送达本来就要等服务器确认，没有延迟收益）。
	 * Amount 允许为负，用来做扣款；余额会被夹到 0 以上。
	 */
	UFUNCTION(BlueprintCallable, Category="Delivery|Economy")
	void AddBalance(int32 Amount);

	/**
	 * 尝试扣款。余额不足时不扣、返回 false——调用方据此决定要不要提示玩家。
	 * 同样只在服务器上有效。
	 */
	UFUNCTION(BlueprintCallable, Category="Delivery|Economy")
	bool TrySpend(int32 Amount);

	/** 余额变化时广播。服务器和客户端都会收到（客户端由 OnRep 触发）。 */
	UPROPERTY(BlueprintAssignable, Category="Delivery|Economy")
	FOnDeliveryBalanceChanged OnBalanceChanged;

	/** 送达入账时广播，带上完整的奖励明细，UI 可以直接拿去做结算飘字。 */
	UPROPERTY(BlueprintAssignable, Category="Delivery|Economy")
	FOnDeliveryTaskPaid OnTaskPaid;

protected:

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 初始资金。策划要调就在 PlayerState 蓝图上改。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|Economy")
	int32 StartingBalance = 0;

private:

	UPROPERTY(ReplicatedUsing=OnRep_Balance)
	int32 Balance = 0;

	UFUNCTION()
	void OnRep_Balance(int32 OldBalance);

	/** 订阅任务管理器。GameState 可能比 PlayerState 晚就绪，所以要重试。 */
	void BindToTaskManager();

	UFUNCTION()
	void HandleTaskCompleted(UDeliveryTaskDefinition* Task, const FDeliveryRewardBreakdown& Reward,
		APlayerState* Deliverer);

	FTimerHandle BindRetryTimer;
};
