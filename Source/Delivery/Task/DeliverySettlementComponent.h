// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliverySettlementComponent.generated.h"

class UDeliverySettlementWidget;
class UDeliveryTaskDefinition;
class UDeliveryWalletComponent;
struct FDeliveryRewardBreakdown;

/**
 * 交付成功后弹结算界面。挂 `ADeliveryPlayerController`。
 *
 * —— 数据从哪来 ——
 *
 * 订阅钱包的 `OnTaskPaid(Task, Reward)`。那个委托自带完整明细
 * （`FDeliveryRewardBreakdown`：基础奖励、两个倍率、最终金额、评价档、用时、是否超时），
 * 而且**钱记给谁天然就对**——多人抢单时只有真正交付的那个人的钱包会广播。
 * 所以这里不需要自己判"这单是不是我送的"。
 *
 * **不订阅 GameState 的 `OnTaskCompleted`**：那个是全局的，每个玩家都会收到，
 * 用它的话别人送达也会给我弹结算。钱包那条天生是本人的。
 *
 * —— 为什么创建和移除都归这里 ——
 *
 * 「加视口 + 登记面板」和「移除 + 注销面板」写在同一处，于是光标状态和
 * 面板在不在屏幕上**结构上不可能失同步**。CLAUDE.md 记过失同步的样子：
 * 蓝图在手机键上 `Set bShowMouseCursor = NOT(...)`，和面板开关是两个互不相干的
 * 取反开关，只要有一次只翻了一个就永久错位，表现是
 * 「骑车时鼠标转不了视角、必须按住左键或右键」。
 * 所以 WBP 里**不要**自己 `Add to Viewport` / `Remove from Parent`。
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliverySettlementComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UDeliverySettlementComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/**
	 * 结算界面的 WBP 类。**在 `BP_DeliveryManPC` 上填**。
	 *
	 * 留空就不弹——不是报错，是静默不弹。所以做完 WBP 记得回来填这一格；
	 * `Delivery.Task.Settlement` 会把它当前的值打出来。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Delivery|Settlement")
	TSubclassOf<UDeliverySettlementWidget> SettlementWidgetClass;

	/**
	 * 全部明细出现后保留多久再自动收起，0 = 必须手动关闭
	 *
	 * 默认自动收是刻意的：这游戏一单接一单，每次都弹个要手动点掉的框，
	 * 连送几单就很烦。点击仍然可以提前关。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Delivery|Settlement", meta=(ClampMin="0", Units="s"))
	float AutoHideSeconds = 6.0f;

	/** 显示在视口的哪一层。数字越大越靠前。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Delivery|Settlement")
	int32 ZOrder = 50;

	/** 手动弹一次，调试和"回看上一单"都能用。 */
	UFUNCTION(BlueprintCallable, Category="Delivery|Settlement")
	void ShowSettlement(UDeliveryTaskDefinition* Task, const FDeliveryRewardBreakdown& Reward);

	/** 收起。重复调用安全。 */
	UFUNCTION(BlueprintCallable, Category="Delivery|Settlement")
	void HideSettlement();

	UFUNCTION(BlueprintPure, Category="Delivery|Settlement")
	bool IsSettlementOpen() const { return ActiveWidget != nullptr; }

protected:

	UPROPERTY(Transient)
	TObjectPtr<UDeliverySettlementWidget> ActiveWidget;

private:
	friend class FDeliverySettlementRevealTest;

	/** 订阅钱包。拿不到就定时重试，见实现里的注释。 */
	void BindToWallet();
	void HandleRevealFinished(UDeliverySettlementWidget* Widget);

	UFUNCTION()
	void HandleTaskPaid(UDeliveryTaskDefinition* Task, const FDeliveryRewardBreakdown& Reward);

	UDeliveryWalletComponent* FindOwningWallet() const;

	FTimerHandle BindRetryTimer;
	FTimerHandle AutoHideTimer;
};
