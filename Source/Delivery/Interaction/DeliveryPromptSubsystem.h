// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DeliveryPromptSubsystem.generated.h"

class SWidget;

/**
 * 显示跟随世界物体的屏幕提示
 * 调用方持续推送提示内容，停止推送后自动隐藏
 * 提示界面由代码创建，不依赖界面蓝图
 */
UCLASS()

class DELIVERY_API UDeliveryPromptSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

  public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;

	static UDeliveryPromptSubsystem* Get(const UObject* WorldContextObject);

	// 提示内容更新
	/** 每帧调用一次；WorldAnchor 是浮窗要贴住的世界坐标 */
	void PushPrompt(const FText& Text, const FVector& WorldAnchor, float HoldProgress = -1.0f);

	/**
	 * 屏幕左下角的常驻提示（"按 R 召唤摩托车"这类）；和世界浮窗是两个独立的槽位，
	 * 各有各的时间戳，互不覆盖——不然走到车边上时召唤提示就被"按 F 驾驶"顶掉了
	 * 同样是"每帧推一次"的约定；bDimmed 用来表示这个功能当前不可用（冷却中）
	 */
	void PushCornerHint(const FText& Text, bool bDimmed = false);

	/**
	 * 更新屏幕顶部的任务提示，与左下角按键提示分别保存
	 * 紧急提示显示为红色，调用方停止推送后自动隐藏
	 */
	void PushObjective(const FText& Title, const FText& Detail, bool bUrgent = false);

	/**
	 * 屏幕下方中央的对白字幕；和三个短时提示槽不同，它会一直保留到 ClearSubtitle，
	 * 因为一句话说完后通常还要停留到通话结束，不能靠 0.25 秒刷新维持
	 */
	void SetSubtitle(const FText& Speaker, const FText& Text);
	void ClearSubtitle();

  private:
	// 控件创建与可见性检查
	void EnsureWidget();
	bool ComputeScreenPosition(FVector2D& OutPosition) const;
	bool IsPromptFresh() const;

	bool IsCornerFresh() const;
	bool IsObjectiveFresh() const;

	TSharedPtr<SWidget> PromptWidget;
	// 左下角操作提示
	FText CornerText;
	bool bCornerDimmed = false;
	double LastCornerPushTime = -1000.0;

	// 顶部任务提示
	FText ObjectiveTitle;
	FText ObjectiveDetail;
	bool bObjectiveUrgent = false;
	double LastObjectivePushTime = -1000.0;
	// 对白字幕
	FText SubtitleSpeaker;
	FText SubtitleText;
	bool bSubtitleVisible = false;
	// 世界物体的屏幕提示
	FText PromptText;
	FVector PromptAnchor = FVector::ZeroVector;
	float PromptHoldProgress = -1.0f;
	double LastPushTime = -1000.0;

	/** 超过这么久没人推送就隐藏；取两三帧的量级，够盖住偶尔掉帧，又不会拖出残留 */
	static constexpr double PromptTimeout = 0.25;
};
