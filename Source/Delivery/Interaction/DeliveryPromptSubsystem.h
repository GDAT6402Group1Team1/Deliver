// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DeliveryPromptSubsystem.generated.h"

class SWidget;

/**
 * 创建并显示交互浮窗、左下角操作提示、顶部任务提示和对白字幕
 * 交互与任务提示停止更新后自动隐藏，角落提示与字幕由调用者主动清除
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
	void PushPrompt(const FText& Text, const FVector& WorldAnchor, float HoldProgress = -1.0f, bool bConversationIcon = false);

    /** 保存左下角提示，直到同一个来源主动清除 */
    void SetCornerHint(UObject* Source, const FText& Text, bool bDimmed = false);
    void ClearCornerHint(const UObject* Source);

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
    friend class FDeliveryPersistentHintTest;
	// 控件创建与可见性检查
	void EnsureWidget();
	bool ComputeScreenPosition(FVector2D& OutPosition) const;
	bool IsPromptFresh() const;

	bool HasCornerHint() const;
	bool IsObjectiveFresh() const;

	TSharedPtr<SWidget> PromptWidget;
	// 左下角操作提示
	FText CornerText;
	bool bCornerDimmed = false;
	TWeakObjectPtr<UObject> CornerSource;

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
	bool bPromptConversationIcon = false;
	double LastPushTime = -1000.0;

	/** 超过这么久没人推送就隐藏；取两三帧的量级，够盖住偶尔掉帧，又不会拖出残留 */
	static constexpr double PromptTimeout = 0.25;
};
