// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Task/DeliveryTaskTypes.h"
#include "DeliverySettlementWidget.generated.h"

class UDeliveryTaskDefinition;
class UTextBlock;
class UAudioComponent;
class USoundBase;
class UDeliverySettlementWidget;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnDeliverySettlementRevealFinished, UDeliverySettlementWidget*);

/**
 * 任务结算界面的 C++ 父类。**WBP 继承它、按名字摆控件。**
 *
 * —— 为什么是「C++ 基类 + WBP 子类」而不是纯 C++ Slate ——
 *
 * 结算界面是**布局活**，而且版式、字号、配色、动画节奏会反复调几十次。
 * 纯 Slate 没有任何预览，改一次要关编辑器 → 编译（本项目一两分钟）→ 重开 → 再跑，
 * 迭代成本很快超过功能本身。小地图走纯 C++ 绘制是因为**圆形裁切和描边 UMG 做不出来**，
 * 那是绘制活、不是布局活，两者别混为一谈。
 *
 * —— `BindWidget` 是双向契约 ——
 *
 * 下面每个带 `meta=(BindWidget)` 的字段，**WBP 里必须有一个同名同类型的控件**，
 * 少一个或类型不对在**编译 WBP 时当场报错**，不是运行时 Accessed None。
 * 这正是要的：以后我改字段名，你那边会被编译器拦住，不会静默变成空文本。
 * 可选的那几个标了 `BindWidgetOptional`，不摆也能编过。
 *
 * —— 谁负责创建和移除 ——
 *
 * **不是这个控件自己，也不是蓝图**，是 `UDeliverySettlementComponent`。
 * 它在同一处做「创建+加视口+登记面板」和「移除+注销面板」，所以光标状态
 * 和面板是否在屏幕上**在结构上不可能失同步**——CLAUDE.md 记过失同步长什么样：
 * 「骑车时鼠标转不了视角，必须按住左键或右键」。
 * 所以 WBP 里**不要**自己 `Add to Viewport` / `Remove from Parent`，
 * 关闭按钮接 `RequestClose()` 就行。
 */
UCLASS(Abstract, BlueprintType, Blueprintable)
class DELIVERY_API UDeliverySettlementWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	UDeliverySettlementWidget(const FObjectInitializer& ObjectInitializer);

	/** 所有明细显示后通知组件，从此时开始计算自动关闭时间 */
	FOnDeliverySettlementRevealFinished OnRevealFinished;

	/** 关闭或替换面板前取消逐行显示和音效，允许重复调用 */
	void CancelReveal();

	/**
	 * 组件填完数据后调一次。蓝图里实现它来播放入场动画、刷新自定义内容。
	 *
	 * 文本已经由 C++ 写进下面那些 `BindWidget` 控件了，这个事件**不负责填字**，
	 * 只负责"动起来"。两边都填的话迟早一处改了另一处没改。
	 */
	UFUNCTION(BlueprintImplementableEvent, Category="Delivery|Settlement")
	void OnSettlementShown(const FDeliveryRewardBreakdown& Reward);

	/** 关闭按钮接这个。**不要**自己 Remove from Parent，见类注释。 */
	UFUNCTION(BlueprintCallable, Category="Delivery|Settlement")
	void RequestClose();

	/** 组件调用，把明细写进控件。蓝图不用管。 */
	void ApplyReward(UDeliveryTaskDefinition* InTask, const FDeliveryRewardBreakdown& InReward);

	/** 这一单的明细，蓝图里想自己排版时读它。 */
	UFUNCTION(BlueprintPure, Category="Delivery|Settlement")
	const FDeliveryRewardBreakdown& GetReward() const { return CachedReward; }

	UFUNCTION(BlueprintPure, Category="Delivery|Settlement")
	UDeliveryTaskDefinition* GetTask() const { return Task; }

protected:

	virtual void NativeDestruct() override;

	/** 开启后标题立即显示，明细逐行出现，总收入最后显示 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|Settlement|Reveal")
	bool bRevealRows = true;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|Settlement|Reveal",
		meta=(ClampMin="0.01", Units="s"))
	float FirstRowDelay = 0.2f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|Settlement|Reveal",
		meta=(ClampMin="0.01", Units="s"))
	float RowRevealInterval = 0.35f;

	/** 显示总收入之前的停顿，替代普通行间隔 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|Settlement|Reveal",
		meta=(ClampMin="0.01", Units="s"))
	float FinalRowDelay = 0.6f;

	/** 每行明细的本机提示音，可替换 Sound Wave / Cue / MetaSound，留空则静音 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|Settlement|Audio")
	TObjectPtr<USoundBase> RowRevealSound;

	/** 总收入的提示音，可与普通行单独配置 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|Settlement|Audio")
	TObjectPtr<USoundBase> FinalRevealSound;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|Settlement|Audio",
		meta=(ClampMin="0", ClampMax="2"))
	float RevealVolume = 1.f;

	/** 每行上升的半音数，1 为半音，2 为全音，0 为同音 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|Settlement|Audio",
		meta=(ClampMin="0", ClampMax="12", DisplayName="Semitones Per Row"))
	float RowPitchStep = 1.f;

	/** 总收入在当前音阶上的额外音高倍率，1 表示继续逐行上升，不额外跳音 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|Settlement|Audio",
		meta=(ClampMin="0.5", ClampMax="2", DisplayName="Final Note Pitch Multiplier"))
	float FinalRevealPitch = 1.f;

	// ———— 必须摆的控件 ————

	/** 任务名。 */
	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> TaskNameText;

	/** 最终到手的钱。 */
	UPROPERTY(meta=(BindWidget))
	TObjectPtr<UTextBlock> FinalRewardText;

	// ———— 可选控件：不摆也能编过 ————
	//
	// 做成可选是因为**第一版界面通常只想显示"任务名 + 到手多少钱"**，
	// 明细是后面才加的。全设成必填会逼你一上来就摆七个控件，
	// 其中五个还没想好放哪。

	UPROPERTY(meta=(BindWidgetOptional))
	TObjectPtr<UTextBlock> BaseRewardText;

	/** 时间评价档名（"准时送达"之类）。**超时交付时这个是空的**，见下面的注释。 */
	UPROPERTY(meta=(BindWidgetOptional))
	TObjectPtr<UTextBlock> TimeGradeText;

	/** 时间倍率，形如 `×1.20`。 */
	UPROPERTY(meta=(BindWidgetOptional))
	TObjectPtr<UTextBlock> TimeMultiplierText;

	/** 特殊事件倍率。没有特殊事件时是 `×1.00`。 */
	UPROPERTY(meta=(BindWidgetOptional))
	TObjectPtr<UTextBlock> SpecialMultiplierText;

	/** 用时，形如 `2:35`。 */
	UPROPERTY(meta=(BindWidgetOptional))
	TObjectPtr<UTextBlock> ElapsedText;

	/**
	 * 超时交付时显示的那行字。
	 *
	 * **超时和"没评上档"是同一件事的两面**：`EvaluateReward` 超时时
	 * `TimeGradeName` 为空、`bOverdue` 为 true。所以 `TimeGradeText` 会被填成
	 * `OverdueGradeText` 的内容，而不是留空——留空的话玩家看到的是"评价那一栏没了"，
	 * 看着像 UI 出了 bug，而不是"我迟到了"。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|Settlement")
	FText OverdueGradeText = NSLOCTEXT("DeliverySettlement", "Overdue", "超时送达");

	/** 倍率的显示格式。`{0}` 是倍率本身。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|Settlement")
	FText MultiplierFormat = NSLOCTEXT("DeliverySettlement", "MultiplierFormat", "×{0}");

	/** 金额的显示格式。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Delivery|Settlement")
	FText CurrencyFormat = NSLOCTEXT("DeliverySettlement", "CurrencyFormat", "¥{0}");

private:

	friend class FDeliverySettlementRevealTest;

	struct FRevealRow
	{
		TWeakObjectPtr<UTextBlock> Text;
		float Opacity = 1.f;
	};

	// 弱引用只用于本次显示，控件本身由 WidgetTree 和上面的绑定字段保活
	TArray<FRevealRow> RevealRows;
	int32 NextRevealRow = 0;
	uint32 RevealGeneration = 0;
	FTimerHandle RevealTimer;

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> RevealAudio;

	void PrepareReveal();
	void RevealNextRow();
	void PlayRevealSound(bool bFinal, int32 RowIndex);
	float GetRevealPitch(bool bFinal, int32 RowIndex) const;
	void StopRevealSound();

	UPROPERTY(Transient)
	TObjectPtr<UDeliveryTaskDefinition> Task;

	// **不能叫 `Reward`**：和 `OnSettlementShown` 的参数同名，
	// UHT 生成的代码里会"声明隐藏了类成员"，而那条警告在本项目是当错误编的。
	UPROPERTY(Transient)
	FDeliveryRewardBreakdown CachedReward;

	/** 把秒数排成 `M:SS`。 */
	FText FormatElapsed(float Seconds) const;
};
